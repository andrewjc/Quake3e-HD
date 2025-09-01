# Ultra Quality Settings Applied to q3config.cfg

## Summary
All ultra quality settings have been directly applied to the q3config.cfg file.
The game will now automatically use these settings on launch.

## Key Settings Applied

### Display Settings
- **Resolution**: 2560x1440 (adjust r_customWidth/r_customHeight as needed)
- **Refresh Rate**: 144Hz
- **VSync**: Enabled
- **Borderless Window**: Enabled

### Texture Quality (Maximum)
- `r_picmip "0"` - Highest texture detail
- `r_texturebits "32"` - 32-bit textures
- `r_textureMode "GL_LINEAR_MIPMAP_LINEAR"` - Trilinear filtering
- `r_ext_max_anisotropy "16"` - 16x anisotropic filtering
- `r_detailtextures "1"` - Detail textures enabled

### Lighting (Enhanced)
- `r_dlightMode "2"` - Advanced dynamic lighting
- `r_dlightIntensity "1.5"` - 50% brighter lights
- `r_dynamicLightLimit "64"` - Double the light limit
- `r_shadowMapSize "2048"` - High-res shadows
- `r_shadowMapLod "3"` - Maximum shadow LOD
- `r_ambientScale "1.0"` - Full ambient lighting
- `r_directedScale "1.5"` - Enhanced directed lighting
- `r_intensity "1.2"` - 20% brightness boost

### Material System (Maximum Quality)
- `r_materialGenQuality "3"` - Maximum generation quality
- `r_materialGenResolution "1024"` - 1024x1024 PBR maps
- `r_materialCacheSize "128"` - 128MB cache
- Enhanced generation parameters for better PBR

### RTX Ray Tracing (All Maximum)
- `rtx_enable "1"` - RTX enabled
- `rtx_quality "3"` - Maximum quality
- `rtx_dlss "2"` - DLSS Quality mode
- `rtx_reflex "1"` - NVIDIA Reflex enabled
- `rtx_gi_bounces "4"` - 4 GI bounces
- `rtx_reflection_quality "3"` - Max reflections
- `rtx_shadow_quality "3"` - Max shadows

### Path Tracing (Enhanced)
- `rt_enable "1"` - Path tracing enabled
- `rt_quality "3"` - Maximum quality
- `rt_bounces "5"` - 5 light bounces
- `rt_samples "4"` - 4 samples per pixel

### Geometry & LOD (High Detail)
- `r_subdivisions "1"` - Maximum subdivisions
- `r_lodCurveError "10000"` - Less aggressive LOD
- `r_lodbias "-2"` - Prefer high detail models
- `r_lodscale "10"` - Increased LOD scale

### Post-Processing (All Effects)
- `r_bloom_intensity "0.8"` - Bloom enabled
- `r_bloom_threshold "0.5"` - Bloom threshold
- `r_flares "1"` - Lens flares enabled
- `r_flareSize "60"` - Larger flares

### Ultra-Wide Support
- `r_ultraWide "1"` - Ultra-wide enabled
- `r_ultraWideMode "2"` - Advanced mode
- `r_ultraWideFOVScale "1.2"` - 20% FOV increase

### Performance
- `r_smp "1"` - Multi-threading enabled
- `com_hunkMegs "512"` - 512MB memory allocation
- `com_maxfps "250"` - 250 FPS cap

## Files Modified
1. `src/project/msvc2017/output/baseq3/q3config.cfg` - Main config (updated)
2. `baseq3/q3config_ultra.cfg` - Backup copy
3. `baseq3/ultra_quality.cfg` - Reference config

## To Revert Settings
If you need to revert to default settings:
1. Delete q3config.cfg
2. Launch the game (it will recreate with defaults)
3. OR in console: `/cvar_restart` then `/vid_restart`

## Launch Instructions
Simply launch the game normally:
```
quake3e-debug.x64.exe
```

The ultra quality settings will be automatically loaded.

## System Requirements
- **GPU**: RTX 3070 or better recommended
- **RAM**: 16GB minimum
- **VRAM**: 8GB minimum
- **CPU**: 6+ cores recommended for r_smp

## Notes
- First launch may take longer as PBR materials are generated
- RTX features require compatible hardware
- Adjust r_customWidth/r_customHeight to match your display
- Some settings may impact performance on lower-end systems