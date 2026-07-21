# RTX Rendering Uplift — Feature Gap Plan

Status: **PROPOSED** — priority-ordered plan for the missing RTX features.
Date: 2026-07-21
Scope: `src/engine/renderer/shaders/rtx/*`, `.../shaders/compute/*`,
`src/engine/renderer/pathtracing/*`.

## Where the pipeline is today

The path tracer already does, and does well: raygen-owned direct lighting
with per-light shadow rays and a light grid, **diffuse** global illumination
(cosine-hemisphere bounces, ~3 deep), SVGF-style denoising (temporal
reprojection + variance-guided à-trous + firefly clamp), ACES tonemap, PBR
material sampling (albedo/normal/roughness/metallic/AO), a depth-aware
composite of rasterized entities, volumetric fog + weapon effects, dynamic
sky, and water caustics.

The gaps below are what a modern RTX renderer has that this one does not.
They are ordered by visual return on effort. Each phase is independently
shippable and ends with a screenshot + autodemo verification.

Key code anchors (current):
- Indirect bounce is diffuse-only: `raygen.rgen` ~line 873 (`throughput *=
  albedo`) and ~885 (`sampleHemisphere`).
- Direct BRDF: `calculatePBR` (`raygen.rgen` ~239), shadow ray
  `traceShadowRay` (~261), light loop `shadeDirectLighting` (~360).
- Surface data from `closesthit.rchit` (albedo/normal/roughness/metallic/
  emission + `materialID` with underwater bit 31).
- Denoise `compute/rt_denoise.comp`, temporal `compute/rt_temporal.comp`,
  composite `compute/rt_composite.comp`.
- Upscaling scaffold `pathtracing/rt_rtx_dlss.c` (behind `#ifdef USE_DLSS`,
  not compiled).

---

## Phase 1 — Specular / glossy / mirror reflections  (highest impact)

**Goal:** metals, polished trim, and smooth floors reflect the environment;
rough surfaces show blurry glossy reflection; the roughness/metallic maps
finally drive real behavior.

**Why first:** it's the single feature most associated with "RTX," it reuses
the existing trace-and-denoise path, and the material inputs already exist.

**Steps:**
1. **Split the bounce BRDF.** In the raygen bounce (currently pure diffuse),
   at each hit choose between a diffuse and a specular lobe by Fresnel and
   metallic:
   - Compute `F0 = mix(vec3(0.04), albedo, metallic)`, Fresnel-Schlick at the
     view angle.
   - Probability of the specular lobe `pSpec = luminance(F)` (clamped);
     Russian-roulette-pick per bounce with `randomFloat`.
   - Diffuse lobe: keep cosine-weighted `sampleHemisphere`, throughput
     `*= albedo * (1 - metallic)`.
   - Specular lobe: **GGX/VNDF importance sampling** — sample a microfacet
     half-vector from the visible-normal distribution for `roughness`,
     reflect the view ray about it, throughput `*= F * G_smith / pdf`
     (the VNDF weight collapses to `G2/G1`). Add a new `sampleGGX(normal,
     viewDir, roughness, seed)` helper next to `sampleHemisphere`.
2. **Perfect-mirror fast path:** for `roughness < ~0.05`, skip sampling and
   reflect exactly (`reflect(currentDir, normal)`) so mirrors and water are
   crisp and cheap.
3. **Recursion budget:** reflections want depth. Keep the existing
   `maxBounces`/roulette; ensure specular bounces don't terminate early
   (weight roulette already handles this). Verify no device-fault at the
   raygen recursion limit (this repo's NVIDIA driver faulted on
   trace-from-hit-shader — reflections stay in the raygen bounce loop, which
   is safe).
4. **G-buffer for denoising:** reflections are view-dependent and can't share
   the diffuse history cleanly. Minimum viable: fold low-roughness specular
   into the existing signal but tag high-variance specular pixels so the
   denoiser (Phase 5) can treat them. Full split deferred to Phase 5.
5. **Tone/energy check:** ensure metals don't blow out — Fresnel + G term
   must conserve energy; validate a chrome/rail surface isn't brighter than
   the incoming light.

**Files:** `raygen.rgen` (bounce loop, new `sampleGGX`), possibly
`closesthit.rchit` (ensure roughness/metallic reach the payload — they
already do).

**Risks:** specular noise (rough reflections are noisy) — mitigated by VNDF
importance sampling and Phase 5 denoising; energy blow-out; recursion faults.

**Verify:** chrome/rail weapon, polished floor in q3dm1, a metal trim close-up
— reflections visible, no fireflies after denoise, autodemo clean.

---

## Phase 2 — Reflective & refractive water and glass

**Goal:** water/slime surfaces reflect the sky and refract the bottom; glass
shaders transmit with refraction.

**Why second:** builds directly on Phase 1's specular path; Q3 has lots of
water/slime/glass; caustics already exist to pair with it.

**Steps:**
1. **Identify transparent surfaces at load.** `rt_bsp_loader.c` already
   collects `CONTENTS_WATER` volumes and sets the underwater atlas bit; extend
   the material classification to mark water/slime/glass surfaces with a
   `MATERIAL_FLAG_REFRACTIVE` + an index of refraction and a tint.
2. **Water surface shading (raygen):** when the primary ray hits a water
   surface:
   - Reflect the environment via the Phase-1 specular path (Fresnel-weighted
     against view angle — grazing angles reflect more).
   - Refract into the water (`refract()` with the surface normal perturbed by
     the existing caustic/wave function at `raygen.rgen` ~377) and trace the
     transmitted ray to shade the bottom, tinted by water color and
     Beer-Lambert absorption over depth.
   - Combine by Fresnel: `mix(refracted, reflected, fresnel)`.
3. **Glass:** same reflect/refract split with the material IOR; two-sided
   handling (entering vs exiting) via the geometric normal sign.
4. **Animated normal:** reuse the water wave/caustic pattern to perturb the
   surface normal so reflections/refractions ripple.
5. **Total internal reflection:** handle `refract()` returning zero (fall back
   to full reflection).

**Files:** `raygen.rgen` (water/glass branch keyed off `materialID` flags),
`rt_bsp_loader.c` / `rt_rtx_material.c` (flag + IOR + tint), `closesthit.rchit`
(pass flags/IOR through the payload).

**Risks:** double-counting with the existing caustic light approximation
(reconcile — caustics become the *refracted* light, not an add-on); depth
sorting of transparent surfaces vs the raster composite.

**Verify:** q3dm2/q3ctf2 pools — sky reflection on the surface, visible
tinted bottom, ripple; a glass surface transmits; autodemo clean.

---

## Phase 3 — Soft / area shadows

**Goal:** replace hard binary shadow edges with penumbra that widens with
distance from the occluder (contact-hardening).

**Why third:** big realism jump, self-contained, no new buffers.

**Steps:**
1. **Area light sampling:** each scene light gets a radius (point lights are
   small spheres, the sun a disc). In `shadeDirectLighting`, sample a random
   point on the light's disc/sphere (concentric-disk sample) per shadow ray
   instead of the light center.
2. **Sample count vs noise:** 1 jittered shadow-ray sample per light per
   frame (keep the cost the same), and lean on the temporal + variance
   denoiser to resolve the penumbra over frames. Optionally 2–4 samples for
   the dominant/sun light.
3. **Contact hardening comes for free** from area sampling: near the occluder
   the penumbra is tight, far away it's wide.
4. **Blue-noise the sample** (interleaved gradient noise already used for the
   volumetrics dither) so the penumbra denoises cleanly.

**Files:** `raygen.rgen` (`shadeDirectLighting`, `traceShadowRay` call sites),
light struct radius already present (`attenuation`/radius).

**Risks:** shadow noise — relies on the denoiser; sun disc size tuning.

**Verify:** statue/pillar shadows on q3dm1 show soft edges that harden at
contact; autodemo clean.

---

## Phase 4 — Upscaling & ray reconstruction (perf force-multiplier)

**Goal:** render the path tracer at a lower internal resolution and upscale,
freeing budget for more samples/bounces/reflections.

**Why fourth:** it multiplies everything above — but it's only worth doing once
there's enough signal (reflections/soft shadows) to benefit.

**Steps (pick one track):**
1. **Track A — wire up the existing DLSS scaffold** (`rt_rtx_dlss.c`): define
   `USE_DLSS`, link the NGX SDK, feed it color + motion vectors (already
   written to `motionVectorImage`) + depth + jitter. Requires the SDK and an
   NVIDIA card; gate behind a cvar with a clean fallback.
2. **Track B — engine-side temporal upscaler (FSR2-style)** if avoiding
   vendor SDKs: render RT at 50–67% res, reproject with the existing motion
   vectors, accumulate with the temporal pass, and sharpen. Reuses the
   temporal infrastructure already built.
3. **Jitter:** the raygen already jitters the camera per sample — extend to a
   Halton sequence sized to the upscale ratio.
4. **Fix the dead TAA path** while here: the loader references a non-existent
   `taa_velocity` shader — either implement it or remove the reference.

**Files:** `rt_rtx_dlss.c`, `rt_rtx_impl.c` (dispatch at internal res, resolve
to display res), `rt_taa_pipeline.c` (dead `taa_velocity` reference),
`raygen.rgen` (Halton jitter).

**Risks:** vendor SDK availability (Track A); upscaling ghosting (Track B) —
the variance clamp from the temporal pass helps.

**Verify:** frame-time drop at equal quality, or more samples at equal
frame-time; no smearing under motion; autodemo clean.

---

## Phase 5 — Dedicated specular / reflection denoising

**Goal:** clean reflections without smearing them; they're higher-frequency
and view-dependent, so they can't share the diffuse history.

**Why fifth:** needed once Phase 1/2 reflections are noticeable; not before.

**Steps:**
1. **Split the signal:** raygen writes diffuse illumination and specular
   illumination to separate images (a second color target).
2. **Separate history:** the temporal pass reprojects specular using a
   *virtual* hit point (reflect the reflection ray, reproject the reflected
   position) rather than the surface position, so reflections track correctly
   under camera motion.
3. **Roughness-aware à-trous:** the specular denoiser's spatial radius scales
   with roughness (mirror = no blur, rough = wide), driven by the roughness
   G-buffer.
4. **Recombine** before tonemap in the finalize pass.

**Files:** `raygen.rgen` (dual output), `rt_temporal.comp` (specular
reprojection), `rt_denoise.comp` (roughness-scaled specular pass),
`rt_rtx_impl.c` (extra images + barriers).

**Risks:** the most complex denoising change; virtual-hit reprojection is
fiddly. Ship Phase 1 with reflections folded into the diffuse denoiser first,
then upgrade here.

**Verify:** reflections stay sharp on mirrors and blur correctly on rough
metal, no lag under motion; autodemo clean.

---

## Phase 6 — Emissive surfaces as true area lights

**Goal:** lava, screens, and light panels cast soft colored glow and bleed
onto nearby surfaces as real emitters, not approximated point lights.

**Steps:**
1. **Emissive triangle list:** at load, collect emissive surfaces (the
   `RTX_TrySpawnEmissiveLight*` helpers already find them) into an emitter
   list with area + radiance instead of only spawning point lights.
2. **Next-event estimation to emitters:** in `shadeDirectLighting`, sample a
   point on a nearby emissive triangle (importance-weighted by area/intensity
   via the light grid) and shadow-ray to it.
3. **MIS:** weight NEE against the BRDF-sampled bounce hitting an emitter, so
   both small bright and large dim emitters converge (power heuristic).
4. **Keep the point-light approximation** as a fallback for tiny emitters.

**Files:** `rt_bsp_loader.c` (emitter list), `raygen.rgen` (NEE + MIS),
light grid (emitter indexing).

**Risks:** cost of sampling many emitters — the light grid must index them;
MIS bookkeeping.

**Verify:** lava glow bleeds warm light onto adjacent walls with soft
falloff; autodemo clean.

---

## Phase 7 — Many-light scaling (ReSTIR) and MIS

**Goal:** correct, low-noise lighting when a scene has dozens of dynamic
lights (rockets, explosions, muzzle flashes all at once).

**Steps:**
1. **Reservoir sampling (RIS):** per pixel, keep a weighted reservoir over
   candidate lights; resample by unshadowed contribution before the single
   shadow ray.
2. **Spatiotemporal reuse (ReSTIR DI):** reuse neighbors' and previous
   frame's reservoirs (guarded by the existing normal/depth checks) for
   dramatically lower variance at fixed ray cost.
3. **MIS** between light and BRDF sampling (shared with Phase 6).

**Files:** new `compute/rt_restir.comp` (reservoir pass) + `raygen.rgen`
integration, reservoir buffers in `rt_rtx_impl.c`.

**Risks:** the most advanced item; correctness of the reuse weights. Only
needed once scenes are light-heavy.

**Verify:** a rocket-spam firefight stays clean where it currently grains;
autodemo clean.

---

## Phase 8 — Camera & post effects on the RT path

**Goal:** wire the existing post-process shaders to the traced HDR and add
the missing camera effects.

**Steps:**
1. **Bloom the traced HDR:** apply the existing bloom pass to RT emissives
   (muzzle flashes, lava, explosions) before tonemap so bright sources glow.
2. **Motion blur:** reuse the `motionVectorImage` already written by raygen
   with the existing `motion_blur` post shader.
3. **Depth of field:** thin-lens circle-of-confusion from the depth G-buffer
   with the existing `dof` shader (optional/toggle — arena shooters usually
   want it off, but useful for spectator/demo).
4. **Wire the remaining post `.spv`** (chromatic_aberration, film_grain,
   vignette, god_rays) as optional cvar-gated passes; god-rays can reuse the
   volumetric sun data.
5. **Remove/repair dead TAA references** (also noted in Phase 4).

**Files:** `tr_postprocess.c`, `rt_rtx_impl.c` (insert post passes after
finalize), the existing `postprocess/*.spv`.

**Risks:** ordering vs the raster composite; keeping arena readability (DoF/CA
off by default).

**Verify:** muzzle flash blooms, motion blur under fast turns, effects
toggle cleanly; autodemo clean.

---

## Cross-cutting

- **Every phase** ends with: a scripted screenshot at a representative
  vantage, a 60 s autodemo (exit 0, zero `DEVICE_LOST`/Vulkan errors), and a
  memory-file note on the calibration.
- **Driver constraint:** keep all light transport in the raygen bounce loop —
  tracing from a closest-hit/bounce hit shader device-faults this repo's
  NVIDIA driver (documented in the RTX architecture memory).
- **New cvars** per feature (`rt_reflections`, `rt_refraction`,
  `rt_softShadows`, `rt_upscale`, `rt_restir`, `rt_bloom` …) all archived,
  defaulting on except the experimental/perf-heavy ones.

## Suggested sequencing

Fastest visible transformation: **Phase 1 → 2 → 3** (reflections, then
reflective water, then soft shadows) — each reuses the existing trace/denoise
path and each is a clear on-screen upgrade. **Phase 4** (upscaling) once the
sample budget gets tight. **Phase 5–7** deepen quality where noise shows.
**Phase 8** is polish and can slot in any time.
