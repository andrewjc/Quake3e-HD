# Quake3e-HD Renderer, Lighting, and Material CVARs Documentation

## Table of Contents
- [Core Rendering](#core-rendering)
- [Texture Settings](#texture-settings)
- [Lighting System](#lighting-system)
- [Material System](#material-system)
- [RTX Ray Tracing](#rtx-ray-tracing)
- [Path Tracing](#path-tracing)
- [Geometry and LOD](#geometry-and-lod)
- [Post-Processing](#post-processing)
- [Ultra-Wide Support](#ultra-wide-support)
- [Performance](#performance)
- [Debug Options](#debug-options)

---

## Core Rendering

| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_mode` | -1 | -2 to 12 | Video mode (-1 = custom, -2 = desktop) |
| `r_customwidth` | 1024 | 320-7680 | Custom resolution width |
| `r_customheight` | 768 | 240-4320 | Custom resolution height |
| `r_fullscreen` | 0 | 0-2 | Fullscreen mode (0=windowed, 1=fullscreen, 2=borderless) |
| `r_noborder` | 1 | 0-1 | Remove window border |
| `r_displayRefresh` | 0 | 0-240 | Display refresh rate (0=auto) |
| `r_swapInterval` | 0 | 0-2 | VSync (0=off, 1=on, 2=adaptive) |
| `r_gamma` | 1.0 | 0.5-3.0 | Gamma correction |
| `r_ignorehwgamma` | 0 | 0-1 | Ignore hardware gamma |
| `r_presentBits` | 24 | 16-32 | Color depth bits |

## Texture Settings

| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_picmip` | 0 | 0-16 | Texture detail (0=highest) |
| `r_texturebits` | 0 | 0/16/32 | Texture color depth (0=default) |
| `r_textureMode` | GL_LINEAR_MIPMAP_NEAREST | Various | Texture filtering mode |
| `r_detailtextures` | 1 | 0-1 | Enable detail textures |
| `r_ext_texture_filter_anisotropic` | 1 | 0-1 | Enable anisotropic filtering |
| `r_ext_max_anisotropy` | 8 | 1-16 | Maximum anisotropy level |
| `r_nomip` | 0 | 0-1 | Disable mipmapping |
| `r_roundImagesDown` | 1 | 0-1 | Round texture dimensions down |
| `r_simpleMipMaps` | 1 | 0-1 | Simple mipmap generation |
| `r_ext_compressed_textures` | 0 | 0-1 | Use compressed textures |

## Lighting System

### Dynamic Lighting
| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_dynamiclight` | 1 | 0-1 | Enable dynamic lights |
| `r_dlightMode` | 1 | 0-2 | Dynamic light mode (0=off, 1=basic, 2=advanced) |
| `r_dlightScale` | 0.5 | 0.1-2.0 | Dynamic light scale |
| `r_dlightIntensity` | 1.0 | 0.1-5.0 | Dynamic light intensity |
| `r_dlightSaturation` | 1.0 | 0.0-2.0 | Dynamic light color saturation |
| `r_dlightBacks` | 1 | 0-1 | Light backsides of surfaces |
| `r_dynamicLightLimit` | 32 | 1-128 | Maximum dynamic lights |

### Light Management
| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_lightCullMethod` | 2 | 0-3 | Light culling method |
| `r_lightInteractionCull` | 1 | 0-1 | Cull light-surface interactions |
| `r_shadowMapSize` | 1024 | 256-4096 | Shadow map resolution |
| `r_shadowMapLod` | 2 | 0-4 | Shadow map LOD levels |

### Lightmap Settings
| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_vertexLight` | 0 | 0-1 | Use vertex lighting (lower quality) |
| `r_lightmap` | 0 | 0-2 | Lightmap display mode |
| `r_mergeLightmaps` | 1 | 0-1 | Merge lightmaps for performance |
| `r_ambientScale` | 0.6 | 0.0-2.0 | Ambient light scale |
| `r_directedScale` | 1.0 | 0.0-2.0 | Directed light scale |
| `r_intensity` | 1.0 | 0.5-2.0 | Overall light intensity |
| `r_mapOverBrightBits` | 2 | 0-4 | Map overbright bits |
| `r_overBrightBits` | 1 | 0-4 | General overbright bits |

## Material System

### Material Override
| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_materialOverride` | 1 | 0-1 | Enable material override system |
| `r_materialAutoGen` | 1 | 0-1 | Auto-generate PBR materials |
| `r_materialExport` | 0 | 0-1 | Export materials to disk |
| `r_materialGenQuality` | 2 | 1-3 | Material generation quality |
| `r_materialGenResolution` | 512 | 128-2048 | Generated texture resolution |
| `r_materialGenAsync` | 1 | 0-1 | Asynchronous generation |
| `r_materialCacheSize` | 32 | 8-256 | Material cache size (MB) |
| `r_materialDebug` | 0 | 0-1 | Debug material system |

### Material Generation Parameters
| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_matgen_sobelStrength` | 1.0 | 0.1-5.0 | Sobel edge detection strength |
| `r_matgen_sobelThreshold` | 0.1 | 0.01-1.0 | Edge detection threshold |
| `r_matgen_luminanceWeight` | 0.7 | 0.0-1.0 | Luminance analysis weight |
| `r_matgen_frequencyScale` | 1.0 | 0.1-5.0 | Frequency analysis scale |
| `r_matgen_chromaticThreshold` | 0.3 | 0.1-1.0 | Chromatic analysis threshold |
| `r_matgen_contrastRadius` | 5.0 | 1.0-20.0 | Local contrast radius |
| `r_matgen_fusionNormal` | 0.6:0.4 | weights | Normal map fusion weights |
| `r_matgen_fusionRoughness` | 0.5:0.5 | weights | Roughness fusion weights |
| `r_matgen_fusionMetallic` | 0.7:0.3 | weights | Metallic fusion weights |

## RTX Ray Tracing

| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `rtx_enable` | 1 | 0-1 | Enable RTX hardware ray tracing |
| `rtx_quality` | 2 | 1-3 | RTX quality preset |
| `rtx_denoise` | 1 | 0-1 | Enable RTX denoising |
| `rtx_dlss` | 0 | 0-3 | DLSS mode (0=off, 1=perf, 2=balanced, 3=quality) |
| `rtx_reflex` | 0 | 0-1 | NVIDIA Reflex low latency |
| `rtx_gi_bounces` | 2 | 1-8 | Global illumination bounces |
| `rtx_reflection_quality` | 2 | 1-3 | Reflection quality |
| `rtx_shadow_quality` | 2 | 1-3 | Shadow quality |
| `rtx_debug` | 0 | 0-1 | RTX debug mode |

## Path Tracing

| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `rt_enable` | 0 | 0-1 | Enable path tracing |
| `rt_quality` | 2 | 1-3 | Path tracing quality |
| `rt_bounces` | 3 | 1-10 | Light bounce count |
| `rt_samples` | 2 | 1-8 | Samples per pixel |
| `rt_denoise` | 1 | 0-1 | Enable denoising |
| `rt_temporal` | 1 | 0-1 | Temporal accumulation |
| `rt_probes` | 1 | 0-1 | Use light probes |
| `rt_cache` | 1 | 0-1 | Enable caching |
| `rt_debug` | 0 | 0-1 | Debug mode |

## Geometry and LOD

| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_subdivisions` | 1 | 1-80 | Curve subdivision level |
| `r_lodCurveError` | 250 | 0-10000 | LOD curve error threshold |
| `r_lodbias` | -2 | -2 to 2 | LOD bias |
| `r_lodscale` | 5 | 1-20 | LOD scale factor |
| `r_nocurves` | 0 | 0-1 | Disable curved surfaces |
| `r_marksOnTriangleMeshes` | 0 | 0-1 | Decals on triangle meshes |

## Post-Processing

### Bloom
| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_bloom_threshold` | 0.6 | 0.0-1.0 | Bloom brightness threshold |
| `r_bloom_threshold_mode` | 0 | 0-1 | Threshold calculation mode |
| `r_bloom_intensity` | 0.5 | 0.0-2.0 | Bloom intensity |
| `r_bloom_modulate` | 0 | 0-1 | Modulate bloom with color |

### Flares
| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_flares` | 0 | 0-1 | Enable lens flares |
| `r_flareSize` | 40 | 10-200 | Flare size |
| `r_flareFade` | 10 | 1-50 | Flare fade distance |
| `r_flareCoeff` | 150 | 50-500 | Flare coefficient |

## Ultra-Wide Support

| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_ultraWide` | 1 | 0-1 | Enable ultra-wide support |
| `r_ultraWideMode` | 1 | 0-2 | Ultra-wide rendering mode |
| `r_ultraWideFOVScale` | 1.0 | 0.5-2.0 | FOV scale for ultra-wide |
| `r_paniniDistance` | 1.0 | 0.1-5.0 | Panini projection distance |
| `r_hudSafeZone` | 1 | 0-1 | HUD safe zone for ultra-wide |
| `r_ultraWideDebug` | 0 | 0-1 | Debug ultra-wide rendering |

## Performance

| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_smp` | 1 | 0-1 | Enable multi-threading |
| `r_vbo` | 1 | 0-1 | Use vertex buffer objects |
| `r_primitives` | -1 | -1 to 2 | Rendering primitives (-1=auto) |
| `r_finish` | 0 | 0-1 | Force GPU sync |
| `r_facePlaneCull` | 1 | 0-1 | Face plane culling |
| `r_maxpolys` | varies | 1024-65536 | Maximum polygons |
| `r_maxpolyverts` | varies | 1024-262144 | Maximum polygon vertices |

## Debug Options

| CVAR | Default | Range/Options | Description |
|------|---------|---------------|-------------|
| `r_showImages` | 0 | 0-2 | Show loaded images |
| `r_debugLight` | 0 | 0-1 | Debug lighting |
| `r_debugSort` | 0 | 0-1 | Debug sorting |
| `r_printShaders` | 0 | 0-1 | Print shader info |
| `r_speeds` | 0 | 0-1 | Show rendering speeds |
| `r_showtris` | 0 | 0-2 | Show wireframe |
| `r_shownormals` | 0 | 0-2 | Show normals |
| `r_showLightVolumes` | 0 | 0-1 | Show light volumes |
| `r_showShadowVolumes` | 0 | 0-1 | Show shadow volumes |
| `r_showLightScissors` | 0 | 0-1 | Show light scissors |

---

## Quick Presets

### Ultra Quality
```
/exec ultra_quality
```

### Performance Mode
```
seta r_picmip "1"
seta r_textureMode "GL_LINEAR_MIPMAP_NEAREST"
seta r_ext_max_anisotropy "4"
seta r_dlightMode "1"
seta r_shadowMapSize "512"
seta r_materialGenQuality "1"
seta r_materialGenResolution "256"
seta rtx_quality "1"
seta rt_samples "1"
seta rt_bounces "2"
```

### Balanced Mode
```
seta r_picmip "0"
seta r_textureMode "GL_LINEAR_MIPMAP_LINEAR"
seta r_ext_max_anisotropy "8"
seta r_dlightMode "1"
seta r_shadowMapSize "1024"
seta r_materialGenQuality "2"
seta r_materialGenResolution "512"
seta rtx_quality "2"
seta rt_samples "2"
seta rt_bounces "3"
```

---

## Usage

1. **To apply Ultra Quality settings:**
   ```
   /exec ultra_quality
   /vid_restart
   ```

2. **To save current settings:**
   ```
   /writeconfig myconfig
   ```

3. **To load saved settings:**
   ```
   /exec myconfig
   ```

4. **To reset to defaults:**
   ```
   /cvar_restart
   /vid_restart
   ```