# Shadow and Lighting Systems Test Guide

## Shadow Modes (cg_shadows)

The shadow system uses `cg_shadows` (not r_shadows):

- **0**: No shadows
- **1**: Simple blob shadows (fast, low quality)
- **2**: Stencil volume shadows (requires stencil buffer)
- **3**: Projection shadows (planar shadows)
- **4**: Advanced per-light shadows (experimental)

## Current Issues

1. **Stencil shadows not working**: `cg_shadows 2` requires:
   - Stencil buffer enabled (`r_stencilbits 8`)
   - `r_dlightMode 2` for proper light direction
   - Models must have `SS_OPAQUE` shaders

2. **Path tracing not visible**: The RT code only calculates lighting but doesn't output to screen
   - `RT_RenderPathTracedLighting()` only prepares data
   - No actual ray dispatch for pixels
   - No framebuffer output

3. **Shadow volumes not invoked**: Files `tr_shadow_volume.c` and `tr_stencil_shadow.c` aren't being called
   - These are for the advanced shadow system (mode 4)
   - Requires `r_shadows->integer >= 4` in `tr_light_backend.c`

## Test Commands

```
// Test different shadow modes
/cg_shadows 0  // No shadows
/cg_shadows 1  // Blob shadows (default)
/cg_shadows 2  // Stencil shadows
/cg_shadows 3  // Projection shadows
/cg_shadows 4  // Advanced shadows

// Enable stencil buffer for mode 2
/r_stencilbits 8
/vid_restart

// Enable proper lighting for shadows
/r_dlightMode 2  // Advanced dynamic lights
/r_dynamiclight 1
/r_dlightIntensity 1.5

// Shadow maps (modern alternative)
/r_shadowMapSize 2048
/r_shadowMapLod 3

// Path tracing (currently non-functional)
/rt_enable 1
/rt_mode all
/rt_quality 3
/rt_debug 1  // Shows debug info

// RTX (framework only, no actual rendering)
/rtx_enable 1
/rtx_status  // Check RTX availability
```

## Shadow Rendering Pipeline

### Mode 1: Blob Shadows
- Rendered in `RB_ShadowTessEnd()`
- Simple dark circle under entities
- No actual geometry

### Mode 2: Stencil Volume Shadows
- Requires `glConfig.stencilBits >= 4`
- Uses `RB_ShadowTessEnd()` for stencil volumes
- `RB_ShadowFinish()` darkens stencil areas
- Needs proper light direction from `r_dlightMode 2`

### Mode 3: Projection Shadows
- Requires `RF_SHADOW_PLANE` render flag
- Projects entity onto a plane
- Used for specific effects

### Mode 4: Advanced Per-Light Shadows
- Would use `tr_shadow_volume.c` and `tr_stencil_shadow.c`
- Currently not reached due to condition check
- Requires `r_shadows->integer >= 4` but `r_shadows` is aliased to `cg_shadows`

## Problems to Fix

1. **Stencil buffer not enabled by default**
   - Need to set `r_stencilbits 8` in config
   - Requires `vid_restart`

2. **Shadow volume code unreachable**
   - `r_shadows` is actually `cg_shadows` in the code
   - Mode 4 check in `tr_light_backend.c` never passes
   - Need to fix CVAR aliasing

3. **Path tracing has no output**
   - Need to implement actual screen ray dispatch
   - Need to write results to framebuffer
   - Current code only does calculations

4. **RTX not actually rendering**
   - Extensions are initialized but no shaders compiled
   - No BLAS/TLAS built from geometry
   - No ray dispatch happening

## Verification Steps

1. **Check stencil buffer**:
   ```
   /r_stencilbits
   ```
   Should show 8 for stencil shadows

2. **Check shadow mode**:
   ```
   /cg_shadows
   ```
   Try values 0-3 (4 won't work currently)

3. **Check lighting mode**:
   ```
   /r_dlightMode
   ```
   Should be 2 for proper shadows

4. **Check path tracing**:
   ```
   /rt_debug 1
   /rt_enable 1
   ```
   Should show debug output if working

5. **Check RTX status**:
   ```
   /rtx_status
   ```
   Shows if hardware is detected