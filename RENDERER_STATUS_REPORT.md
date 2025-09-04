# Renderer Features Status Report

## ❌ NON-FUNCTIONAL Features (Need Fixing)

### 1. **Path Tracing (RT)**
- **Problem**: Only calculates lighting, no screen output
- **Files**: `rt_pathtracer.c`, `rt_screen_dispatch.c`
- **Issue**: Missing actual pixel rendering to framebuffer
- **Fix Needed**: Implement proper screen-space ray dispatch

### 2. **RTX Hardware Ray Tracing**
- **Problem**: Framework exists but no actual ray tracing
- **Files**: `rt_rtx.c`, `rt_rtx_impl.c`
- **Issue**: 
  - No compiled ray tracing shaders
  - No BLAS/TLAS acceleration structures built
  - Extensions initialized but not used
- **Fix Needed**: Complete Vulkan RT implementation

### 3. **Advanced Shadow Volumes (Mode 4)**
- **Problem**: Code never reached
- **Files**: `tr_shadow_volume.c`, `tr_stencil_shadow.c`
- **Issue**: `r_shadows->integer >= 4` check fails because `r_shadows` = `cg_shadows` (max 3)
- **Fix Needed**: Fix CVAR aliasing or add new shadow mode

### 4. **Stencil Shadows (Mode 2)**
- **Problem**: May not work without proper setup
- **Issue**: Requires `r_stencilbits 8` and `vid_restart`
- **Fix Needed**: Set stencil bits in config by default

## ✅ WORKING Features

### 1. **PBR Material System**
- Auto-generates PBR textures from legacy assets
- Material override system functional
- CVARs working: `r_materialOverride`, `r_materialGenQuality`, etc.

### 2. **Ultra-Wide Support**
- Working at 3440x1440
- FOV scaling functional
- CVARs: `r_ultraWide`, `r_ultraWideMode`, `r_ultraWideFOVScale`

### 3. **Basic Shadows (Modes 0-3)**
- Mode 0: No shadows ✅
- Mode 1: Blob shadows ✅
- Mode 2: Stencil shadows (with proper config) ✅
- Mode 3: Projection shadows ✅

### 4. **Dynamic Lighting**
- `r_dlightMode` 0-2 working
- Light intensity and scale CVARs functional
- Shadow maps working (`r_shadowMapSize`)

### 5. **Post-Processing**
- Bloom working (`r_bloom`, `r_bloom_intensity`)
- Tone mapping functional
- HDR pipeline in place

## 🔧 PARTIALLY WORKING

### 1. **Material Generation**
- Generates textures but quality varies
- Some materials look incorrect
- Export/import works but needs refinement

### 2. **Lighting System**
- Point queries work (for lightgrid)
- Full scene lighting incomplete
- Static light extraction implemented but unused

## Configuration Files Created

1. **test_shadows.cfg** - Test shadow modes
2. **ultra_quality.cfg** - Maximum visual settings
3. **SHADOW_AND_LIGHTING_TEST.md** - Testing guide

## Console Commands for Testing

```bash
# Test shadows (WORKING)
/exec test_shadows
/cg_shadows 2
/r_dlightMode 2
/vid_restart

# Test path tracing (NOT WORKING - no visual output)
/rt_enable 1
/rt_mode all
/rt_debug 1

# Test RTX (NOT WORKING - framework only)
/rtx_enable 0  # Disabled to prevent issues
/rtx_status

# Test materials (WORKING)
/r_materialOverride 1
/r_materialGenQuality 3
/r_materialDebug 1

# Test bloom (WORKING)
/r_bloom 1
/r_bloom_intensity 2.0
```

## Priority Fixes Needed

1. **Make path tracing visible** - Add actual screen rendering
2. **Fix shadow volume mode 4** - Resolve CVAR aliasing  
3. **Complete RTX implementation** - Add shaders and BLAS/TLAS
4. **Default stencil buffer** - Set `r_stencilbits 8` in config
5. **Debug visualization** - Add visual feedback for RT features

## Evidence of Working Features

To see working features:
1. Blob shadows: `/cg_shadows 1` - See dark circles under players
2. Bloom: `/r_bloom 1` - See glowing lights
3. Dynamic lights: Shoot weapons, see light effects
4. Materials: Textures should look different with PBR enabled
5. Ultra-wide: Resolution fills 21:9 screen properly

## Evidence of Non-Working Features

1. Path tracing: `/rt_enable 1` - No visual change
2. RTX: `/rtx_enable 1` - No ray traced output
3. Shadow volumes: `/cg_shadows 4` - Doesn't work
4. Debug output: No ray visualization on screen