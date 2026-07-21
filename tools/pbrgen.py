#!/usr/bin/env python3
"""
pbrgen — PBR map generation pipeline for Quake3e-HD.

Reads the legacy game textures out of pk3 archives (and loose files),
estimates height fields from the albedo, and derives the PBR maps the RTX
material pipeline samples:

    <name>_n     tangent-space normal map        (24-bit RLE TGA)
    <name>_r     roughness                       (grayscale JPEG)
    <name>_metal metallic mask                   (grayscale JPEG, only when nonzero)
    <name>_ao    ambient occlusion / cavity      (grayscale JPEG)

Outputs are packed into a single pk3 (zip) that sorts after the retail
paks, so the engine's filesystem picks the maps up with priority, or
written as loose files with --loose.

Usage:
    py tools/pbrgen.py --input "C:/.../Quake 3 Arena/baseq3" \
                       --output out/zzz-pbrmaps.pk3 \
                       [--filter textures/base_wall] [--workers 8]
                       [--normal-strength 1.8] [--ao-strength 1.0]
                       [--preview textures/base_wall/bluemetal2 --preview-dir out/preview]
"""

import argparse
import fnmatch
import io
import os
import sys
import zipfile
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field

import numpy as np
from PIL import Image

# Pillow safety valve for large scans; game textures are small
Image.MAX_IMAGE_PIXELS = 64 * 1024 * 1024

IMAGE_EXTS = (".tga", ".jpg", ".jpeg", ".png")

# Suffixes that mark a texture as already being a PBR/aux map
PBR_SUFFIXES = ("_n", "_nrm", "_normal", "_local", "_s", "_spec", "_metallic",
                "_metal", "_r", "_rough", "_roughness", "_ao", "_occlusion",
                "_glow", "_emit", "_emission", "_luma")

# Directories that never need material maps
SKIP_DIR_PARTS = ("textures/skies", "textures/effects", "textures/sfx/hologirl",
                  "levelshots", "menu", "gfx", "icons", "models/players",
                  "models/powerups", "sprites", "ui", "fonts")

# Path keywords -> (metallic base, roughness base). First match wins; the
# generated maps modulate these bases with image-derived detail.
CATEGORY_TABLE = (
    ("chrome",      (0.90, 0.30)),
    ("_metal",      (0.75, 0.45)),
    ("metal",       (0.75, 0.45)),
    ("mech",        (0.60, 0.50)),
    ("pipe",        (0.55, 0.45)),
    ("grate",       (0.60, 0.50)),
    ("rust",        (0.45, 0.75)),
    ("trim",        (0.40, 0.55)),
    ("base_support",(0.55, 0.50)),
    ("liquid",      (0.00, 0.25)),
    ("water",       (0.00, 0.20)),
    ("glass",       (0.00, 0.10)),
    ("marble",      (0.00, 0.45)),
    ("lava",        (0.00, 0.85)),
    ("organic",     (0.00, 0.85)),
    ("skin",        (0.00, 0.80)),
    ("stone",       (0.00, 0.85)),
    ("rock",        (0.00, 0.90)),
    ("wall",        (0.05, 0.80)),
    ("floor",       (0.10, 0.75)),
    ("ceil",        (0.05, 0.75)),
    ("wood",        (0.00, 0.70)),
)
DEFAULT_CATEGORY = (0.05, 0.75)


@dataclass
class GenParams:
    normal_strength: float = 1.8
    ao_strength: float = 1.0
    rough_detail: float = 0.30
    jpeg_quality: int = 92


@dataclass
class Result:
    source: str
    outputs: list = field(default_factory=list)
    skipped: str = ""


def is_pbr_aux_name(path):
    stem = os.path.splitext(os.path.basename(path))[0].lower()
    return any(stem.endswith(s) for s in PBR_SUFFIXES)


def wants_processing(path, pattern):
    p = path.replace("\\", "/").lower()
    if not p.startswith("textures/"):
        return False
    if any(part in p for part in SKIP_DIR_PARTS):
        return False
    if is_pbr_aux_name(p):
        return False
    if pattern and not fnmatch.fnmatch(p, pattern) and not p.startswith(pattern):
        return False
    return os.path.splitext(p)[1] in IMAGE_EXTS


def collect_sources(input_dir, pattern):
    """Map texture path (no extension) -> newest bytes, honoring pk3 order:
    later pk3s override earlier ones; loose files override everything."""
    sources = {}
    existing_aux = set()

    paks = sorted(f for f in os.listdir(input_dir) if f.lower().endswith(".pk3"))
    for pak in paks:
        with zipfile.ZipFile(os.path.join(input_dir, pak)) as zf:
            for info in zf.infolist():
                name = info.filename.replace("\\", "/")
                lower = name.lower()
                if os.path.splitext(lower)[1] not in IMAGE_EXTS:
                    continue
                if is_pbr_aux_name(lower):
                    existing_aux.add(os.path.splitext(lower)[0])
                    continue
                if wants_processing(lower, pattern):
                    sources[os.path.splitext(lower)[0]] = zf.read(info)

    # Loose files on top
    loose_root = os.path.join(input_dir, "textures")
    if os.path.isdir(loose_root):
        for root, _dirs, files in os.walk(loose_root):
            for f in files:
                full = os.path.join(root, f)
                rel = os.path.relpath(full, input_dir).replace("\\", "/").lower()
                if is_pbr_aux_name(rel):
                    existing_aux.add(os.path.splitext(rel)[0])
                    continue
                if wants_processing(rel, pattern):
                    with open(full, "rb") as fh:
                        sources[os.path.splitext(rel)[0]] = fh.read()

    return sources, existing_aux


def category_for(path):
    p = path.lower()
    for key, bases in CATEGORY_TABLE:
        if key in p:
            return bases
    return DEFAULT_CATEGORY


def gaussian_blur(arr, sigma):
    """Separable gaussian via repeated box filters (numpy only)."""
    if sigma <= 0:
        return arr
    # Three box passes approximate a gaussian well
    radius = max(1, int(sigma * 0.6))
    out = arr
    for _ in range(3):
        out = box_blur(out, radius)
    return out


def box_blur(arr, radius):
    size = radius * 2 + 1
    # Horizontal
    padded = np.pad(arr, ((0, 0), (radius, radius)), mode="wrap")
    csum = np.cumsum(padded, axis=1)
    out = (csum[:, size - 1:] - np.concatenate(
        [np.zeros((arr.shape[0], 1)), csum[:, :-size]], axis=1)) / size
    # Vertical
    padded = np.pad(out, ((radius, radius), (0, 0)), mode="wrap")
    csum = np.cumsum(padded, axis=0)
    out = (csum[size - 1:, :] - np.concatenate(
        [np.zeros((1, arr.shape[1])), csum[:-size, :]], axis=0)) / size
    return out


def estimate_height(lum):
    """Band-passed luminance: local detail reads as relief while large-scale
    paint/lighting variations are flattened out."""
    fine = lum - gaussian_blur(lum, 2.5)
    mid = gaussian_blur(lum, 2.5) - gaussian_blur(lum, 9.0)
    height = 0.65 * fine + 0.35 * mid
    lo, hi = np.percentile(height, (2, 98))
    if hi - lo < 1e-6:
        return np.full_like(lum, 0.5)
    return np.clip((height - lo) / (hi - lo), 0.0, 1.0)


def make_normal(height, strength):
    # Central differences with wrap (tiling textures)
    gx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * strength
    gy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * strength
    nz = np.ones_like(height)
    # Image +y runs downward; tangent-space +Y follows the OpenGL convention
    n = np.stack([-gx, gy, nz], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return (n * 0.5 + 0.5)


def local_contrast(lum):
    mean = gaussian_blur(lum, 4.0)
    var = gaussian_blur((lum - mean) ** 2, 4.0)
    contrast = np.sqrt(np.maximum(var, 0.0))
    hi = np.percentile(contrast, 98)
    if hi < 1e-6:
        return np.zeros_like(lum)
    return np.clip(contrast / hi, 0.0, 1.0)


def make_roughness(lum, rough_base, detail_amount):
    # Smooth, bright areas trend glossier; contrasty grime trends rougher
    contrast = local_contrast(lum)
    rough = rough_base + detail_amount * (contrast - 0.35) - 0.10 * (lum - 0.5)
    return np.clip(rough, 0.08, 0.98)


def make_metallic(rgb, lum, metal_base):
    if metal_base <= 0.01:
        return None
    # Metals in the legacy art are desaturated; colored paint is dielectric
    mx = rgb.max(axis=-1)
    mn = rgb.min(axis=-1)
    sat = (mx - mn) / np.maximum(mx, 1e-4)
    metal = metal_base * np.clip(1.15 - 1.6 * sat, 0.0, 1.0)
    # Very dark pits (grime) are not bare metal
    metal *= np.clip(lum * 4.0, 0.25, 1.0)
    if metal.max() < 0.03:
        return None
    return np.clip(metal, 0.0, 1.0)


def make_ao(height, strength):
    # Cavity: points below their smoothed neighborhood are occluded
    cavity = gaussian_blur(height, 6.0) - height
    ao = 1.0 - strength * 1.8 * np.clip(cavity, 0.0, 1.0)
    return np.clip(ao, 0.25, 1.0)


def encode_tga_rle24(rgb8):
    """24-bit RLE TGA encoder (bottom-up origin, BGR order) — matches the
    engine's TGA loader; Pillow's TGA writer does not do RLE for RGB."""
    h, w, _ = rgb8.shape
    header = bytearray(18)
    header[2] = 10                      # RLE true-color
    header[12] = w & 0xFF
    header[13] = (w >> 8) & 0xFF
    header[14] = h & 0xFF
    header[15] = (h >> 8) & 0xFF
    header[16] = 24
    header[17] = 0x00                   # bottom-up

    bgr = rgb8[::-1, :, ::-1]           # flip vertically, RGB->BGR
    out = bytearray(header)
    for row in bgr:
        x = 0
        while x < row.shape[0]:
            # Find run of identical pixels
            run = 1
            while (x + run < row.shape[0] and run < 128 and
                   (row[x + run] == row[x]).all()):
                run += 1
            if run >= 2:
                out.append(0x80 | (run - 1))
                out.extend(row[x].tobytes())
                x += run
            else:
                # Raw packet: gather until next run of >=3 or 128 pixels
                start = x
                x += 1
                while (x < row.shape[0] and x - start < 128):
                    if (x + 2 < row.shape[0] and
                            (row[x] == row[x + 1]).all() and
                            (row[x] == row[x + 2]).all()):
                        break
                    x += 1
                count = x - start
                out.append(count - 1)
                out.extend(row[start:x].tobytes())
    return bytes(out)


def encode_gray_jpeg(gray, quality):
    img = Image.fromarray((np.clip(gray, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), "L")
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=quality, subsampling=0)
    return buf.getvalue()


def process_one(args):
    path, data, params, existing_aux = args
    result = Result(source=path)
    try:
        img = Image.open(io.BytesIO(data))
        img = img.convert("RGB")
    except Exception as e:
        result.skipped = f"decode failed: {e}"
        return result

    rgb = np.asarray(img, dtype=np.float32) / 255.0
    if rgb.shape[0] < 16 or rgb.shape[1] < 16:
        result.skipped = "too small"
        return result

    lum = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    metal_base, rough_base = category_for(path)

    height = estimate_height(lum)

    outputs = []
    if (path + "_n") not in existing_aux:
        normal = make_normal(height, params.normal_strength)
        rgb8 = (normal * 255.0 + 0.5).astype(np.uint8)
        outputs.append((path + "_n.tga", encode_tga_rle24(rgb8)))

    if (path + "_r") not in existing_aux:
        rough = make_roughness(lum, rough_base, params.rough_detail)
        outputs.append((path + "_r.jpg", encode_gray_jpeg(rough, params.jpeg_quality)))

    if (path + "_metal") not in existing_aux:
        metal = make_metallic(rgb, lum, metal_base)
        if metal is not None:
            outputs.append((path + "_metal.jpg", encode_gray_jpeg(metal, params.jpeg_quality)))

    if (path + "_ao") not in existing_aux:
        ao = make_ao(height, params.ao_strength)
        outputs.append((path + "_ao.jpg", encode_gray_jpeg(ao, params.jpeg_quality)))

    result.outputs = outputs
    return result


def main():
    ap = argparse.ArgumentParser(description="Generate PBR maps from legacy Quake 3 textures")
    ap.add_argument("--input", required=True, help="baseq3 directory containing pk3s / loose textures")
    ap.add_argument("--output", required=True, help="output .pk3 path, or a directory with --loose")
    ap.add_argument("--loose", action="store_true", help="write loose files instead of a pk3")
    ap.add_argument("--filter", default="", help="only process paths starting with / matching this pattern")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--normal-strength", type=float, default=1.8)
    ap.add_argument("--ao-strength", type=float, default=1.0)
    ap.add_argument("--rough-detail", type=float, default=0.30)
    ap.add_argument("--jpeg-quality", type=int, default=92)
    ap.add_argument("--preview", default="", help="also dump PNG previews for this texture path (no extension)")
    ap.add_argument("--preview-dir", default="pbr_preview")
    args = ap.parse_args()

    params = GenParams(args.normal_strength, args.ao_strength,
                       args.rough_detail, args.jpeg_quality)

    print(f"pbrgen: scanning {args.input}")
    sources, existing_aux = collect_sources(args.input, args.filter.lower())
    print(f"pbrgen: {len(sources)} source textures "
          f"({len(existing_aux)} existing aux maps respected)")
    if not sources:
        print("pbrgen: nothing to do")
        return 1

    work = [(path, data, params, existing_aux) for path, data in sorted(sources.items())]

    results = []
    if args.workers > 1:
        with ProcessPoolExecutor(max_workers=args.workers) as pool:
            for i, res in enumerate(pool.map(process_one, work, chunksize=8)):
                results.append(res)
                if (i + 1) % 100 == 0:
                    print(f"pbrgen: {i + 1}/{len(work)}")
    else:
        for i, w in enumerate(work):
            results.append(process_one(w))
            if (i + 1) % 100 == 0:
                print(f"pbrgen: {i + 1}/{len(work)}")

    generated = 0
    skipped = 0
    if args.loose:
        for res in results:
            if res.skipped:
                skipped += 1
                continue
            for relpath, blob in res.outputs:
                full = os.path.join(args.output, relpath)
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "wb") as fh:
                    fh.write(blob)
                generated += 1
    else:
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as zf:
            for res in results:
                if res.skipped:
                    skipped += 1
                    continue
                for relpath, blob in res.outputs:
                    zf.writestr(relpath, blob)
                    generated += 1

    if args.preview:
        key = args.preview.lower()
        if key in sources:
            os.makedirs(args.preview_dir, exist_ok=True)
            res = process_one((key, sources[key], params, set()))
            Image.open(io.BytesIO(sources[key])).convert("RGB").save(
                os.path.join(args.preview_dir, "albedo.png"))
            for relpath, blob in res.outputs:
                name = os.path.basename(relpath)
                stem, ext = os.path.splitext(name)
                img = Image.open(io.BytesIO(blob))
                img.save(os.path.join(args.preview_dir, stem + ".png"))
            print(f"pbrgen: previews in {args.preview_dir}")
        else:
            print(f"pbrgen: preview source '{args.preview}' not found")

    print(f"pbrgen: wrote {generated} maps "
          f"({skipped} sources skipped) -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
