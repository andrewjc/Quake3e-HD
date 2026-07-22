#!/usr/bin/env python3
"""
material_editor_server.py - local backend for the Quake3e-HD material editor.

Serves the material_editor.html app and does the disk I/O the browser sandbox
cannot: it scans the loose PBR textures under baseq3, groups them into materials
(albedo + _n / _r / _metal / _ao companions), streams each map to the browser as
PNG (browsers cannot decode TGA), and writes painted maps back to disk in their
original on-disk format (JPEG for albedo/roughness/metallic/ao, RLE-24 TGA for
normal maps - matching what the engine's loader expects).

Run:
    py tools/material_editor_server.py
    py tools/material_editor_server.py --textures-dir <dir> --port 8770
Then open http://localhost:8770/ in Chrome.
"""

import argparse
import io
import json
import os
import posixpath
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image

Image.MAX_IMAGE_PIXELS = 64 * 1024 * 1024

IMAGE_EXTS = (".tga", ".jpg", ".jpeg", ".png")

# Map-kind suffixes (order matters: check the longest/most specific idea first).
MAP_SUFFIXES = {
    "normal":    ("_n", "_nrm", "_normal", "_local"),
    "roughness": ("_r", "_rough", "_roughness"),
    "metallic":  ("_metal", "_metallic", "_s", "_spec"),
    "ao":        ("_ao", "_occlusion"),
    "emission":  ("_glow", "_emit", "_emission", "_luma"),
}

# Path keyword -> human tag, used as the searchable "description".
CATEGORY_KEYWORDS = (
    ("light", "light"), ("lamp", "light"), ("lava", "lava"), ("liquid", "liquid"),
    ("water", "water"), ("glass", "glass"), ("metal", "metal"), ("pipe", "metal"),
    ("grate", "metal"), ("chrome", "metal"), ("rust", "rusted metal"),
    ("trim", "trim"), ("wall", "wall"), ("floor", "floor"), ("ceil", "ceiling"),
    ("door", "door"), ("button", "button"), ("wood", "wood"), ("stone", "stone"),
    ("rock", "rock"), ("block", "block"), ("skin", "organic"), ("organic", "organic"),
    ("sky", "sky"), ("ctf", "ctf"), ("gothic", "gothic"), ("base", "base"),
)


def strip_ext(name):
    return os.path.splitext(name)[0]


def classify_suffix(stem):
    """Return (map_kind, base_stem) for a texture stem, or (None, stem)."""
    low = stem.lower()
    for kind, suffixes in MAP_SUFFIXES.items():
        for suf in suffixes:
            if low.endswith(suf):
                return kind, stem[: len(stem) - len(suf)]
    return None, stem


def category_for(path):
    p = path.lower()
    tags = []
    for key, tag in CATEGORY_KEYWORDS:
        if key in p and tag not in tags:
            tags.append(tag)
    return " ".join(tags[:3]) if tags else "material"


def scan_materials(root):
    """Group loose textures into materials keyed by their base path."""
    groups = {}  # base_relpath (posix, no ext) -> {kind: relpath_with_ext}
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            ext = os.path.splitext(f)[1].lower()
            if ext not in IMAGE_EXTS:
                continue
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace("\\", "/")
            stem = strip_ext(rel)
            kind, base = classify_suffix(stem)
            slot = kind if kind else "albedo"
            g = groups.setdefault(base, {})
            # First writer wins per slot; albedo prefers a real base image.
            if slot not in g:
                g[slot] = rel

    materials = []
    for base in sorted(groups):
        maps = groups[base]
        if "albedo" not in maps:
            # A group with only companion maps and no base image: synthesise an
            # albedo entry from whatever map exists so it is still selectable.
            continue
        name = posixpath.basename(base)
        materials.append({
            "id": len(materials),
            "name": name,
            "path": base,
            "category": category_for(base),
            "maps": maps,
        })
    for i, m in enumerate(materials):
        m["id"] = i
    return materials


# ---- TGA RLE-24 encoder (matches the engine's loader; shared with pbrgen) ----
def encode_tga_rle24(rgb8):
    import numpy as np
    h, w, _ = rgb8.shape
    header = bytearray(18)
    header[2] = 10                      # RLE true-color
    header[12] = w & 0xFF
    header[13] = (w >> 8) & 0xFF
    header[14] = h & 0xFF
    header[15] = (h >> 8) & 0xFF
    header[16] = 24
    header[17] = 0x00                   # bottom-up origin
    bgr = rgb8[::-1, :, ::-1]           # flip vertical, RGB->BGR
    out = bytearray(header)
    for row in bgr:
        x = 0
        n = row.shape[0]
        while x < n:
            run = 1
            while x + run < n and run < 128 and (row[x + run] == row[x]).all():
                run += 1
            if run >= 2:
                out.append(0x80 | (run - 1))
                out.extend(row[x].tobytes())
                x += run
            else:
                start = x
                x += 1
                while x < n and x - start < 128:
                    if (x + 2 < n and (row[x] == row[x + 1]).all()
                            and (row[x] == row[x + 2]).all()):
                        break
                    x += 1
                out.append((x - start) - 1)
                out.extend(row[start:x].tobytes())
    return bytes(out)


class Handler(BaseHTTPRequestHandler):
    server_version = "MatEdit/1.0"

    # Injected by main()
    textures_root = None
    html_path = None
    materials = None

    def log_message(self, fmt, *args):
        pass  # quiet

    def _send(self, code, body, ctype, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _safe_abs(self, rel):
        rel = rel.replace("\\", "/").lstrip("/")
        abs_path = os.path.normpath(os.path.join(self.textures_root, rel))
        root = os.path.normpath(self.textures_root)
        if os.path.commonpath([abs_path, root]) != root:
            raise ValueError("path escapes textures root")
        return abs_path

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        if path == "/" or path == "/index.html":
            try:
                with open(self.html_path, "rb") as fh:
                    self._send(200, fh.read(), "text/html; charset=utf-8")
            except OSError as e:
                self._send(500, str(e).encode(), "text/plain")
            return

        if path == "/api/materials":
            body = json.dumps({"root": self.textures_root,
                               "materials": self.materials}).encode()
            self._send(200, body, "application/json")
            return

        if path == "/api/texture":
            qs = urllib.parse.parse_qs(parsed.query)
            rel = qs.get("path", [""])[0]
            try:
                abs_path = self._safe_abs(rel)
                img = Image.open(abs_path)
                img.load()
                # Grayscale maps stay single-channel; everything else RGB(A).
                if img.mode not in ("L", "RGB", "RGBA"):
                    img = img.convert("RGB")
                buf = io.BytesIO()
                img.save(buf, "PNG")
                self._send(200, buf.getvalue(), "image/png")
            except Exception as e:
                self._send(404, f"{e}".encode(), "text/plain")
            return

        self._send(404, b"not found", "text/plain")

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/api/texture":
            self._send(404, b"not found", "text/plain")
            return

        qs = urllib.parse.parse_qs(parsed.query)
        rel = qs.get("path", [""])[0]
        try:
            length = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(length)
            abs_path = self._safe_abs(rel)
            ext = os.path.splitext(abs_path)[1].lower()

            img = Image.open(io.BytesIO(data))  # incoming PNG from the canvas
            img.load()

            if ext == ".tga":
                import numpy as np
                rgb = np.asarray(img.convert("RGB"), dtype=np.uint8)
                blob = encode_tga_rle24(rgb)
                with open(abs_path, "wb") as fh:
                    fh.write(blob)
            elif ext in (".jpg", ".jpeg"):
                stem = strip_ext(os.path.basename(abs_path))
                kind, _ = classify_suffix(stem)
                out = img.convert("L") if kind in ("roughness", "metallic", "ao") \
                    else img.convert("RGB")
                out.save(abs_path, "JPEG", quality=95, subsampling=0)
            else:  # .png and anything else: keep as-is
                img.save(abs_path)

            self._send(200, json.dumps({"ok": True, "path": rel}).encode(),
                       "application/json")
        except Exception as e:
            self._send(500, json.dumps({"ok": False, "error": str(e)}).encode(),
                       "application/json")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_tex = os.path.normpath(os.path.join(
        here, "..", "src", "project", "msvc2017", "output", "baseq3", "textures"))

    ap = argparse.ArgumentParser(description="Quake3e-HD material editor backend")
    ap.add_argument("--textures-dir", default=default_tex,
                    help="root of the loose baseq3 textures to edit")
    ap.add_argument("--html", default=os.path.join(here, "material_editor.html"))
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    root = os.path.normpath(args.textures_dir)
    if not os.path.isdir(root):
        print(f"material_editor: textures dir not found: {root}")
        return 1

    Handler.textures_root = root
    Handler.html_path = args.html
    print(f"material_editor: scanning {root} ...")
    Handler.materials = scan_materials(root)
    print(f"material_editor: {len(Handler.materials)} materials")

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"material_editor: serving at {url}  (Ctrl+C to stop)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nmaterial_editor: stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
