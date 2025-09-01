# Material Override System CVARs

The material override system is now fully integrated and functional. All CVARs will auto-populate in your config file when changed.

## Core System CVARs

```
r_materialOverride 1              // Enable/disable material override system (0/1)
r_materialAutoGen 1               // Enable automatic PBR map generation (0/1)
r_materialExport 0                // Enable automatic export of generated maps (0/1)
r_materialGenQuality 2            // Generation quality (0=low, 1=medium, 2=high, 3=ultra)
r_materialGenResolution 512       // Resolution for generated maps (256/512/1024/2048)
r_materialGenAsync 1              // Enable async generation to prevent stutters (0/1)
r_materialCacheSize 128           // Cache size in MB (32-512)
r_materialDebug 0                 // Enable debug output (0/1)
```

## Generation Parameters

Fine-tune the automatic PBR map generation algorithms:

```
r_matgen_sobelStrength 1.0        // Sobel edge detection strength (0.1-5.0)
r_matgen_sobelThreshold 0.1       // Sobel edge threshold (0.01-1.0)
r_matgen_luminanceWeight 0.7      // Luminance contribution weight (0.0-1.0)
r_matgen_frequencyScale 1.0       // Frequency analysis scale (0.1-5.0)
r_matgen_chromaticThreshold 0.3   // Chromatic analysis threshold (0.0-1.0)
r_matgen_contrastRadius 5.0       // Local contrast radius in pixels (1.0-20.0)
r_matgen_fusionNormal 0.6:0.4     // Normal map fusion weights (sobel:luminance)
r_matgen_fusionRoughness 0.5:0.5  // Roughness map fusion weights
r_matgen_fusionMetallic 0.7:0.3   // Metallic map fusion weights
```

## Console Commands

```
mat_regenerate        // Force regeneration of all material overrides
mat_export           // Export all generated materials to disk
mat_clear            // Clear material cache
mat_status           // Show material override system status
materiallist         // List all loaded materials
materialinfo <name>  // Show info for specific material
```

## Usage Examples

### Enable high quality PBR generation:
```
r_materialOverride 1
r_materialAutoGen 1
r_materialGenQuality 3
r_materialGenResolution 1024
vid_restart
```

### Export generated maps for external editing:
```
r_materialExport 1
mat_regenerate
mat_export
```

### Optimize for performance:
```
r_materialGenQuality 1
r_materialGenResolution 256
r_materialGenAsync 1
r_materialCacheSize 64
```

### Debug material generation:
```
r_materialDebug 1
mat_status
```

## Notes

- All CVARs with CVAR_ARCHIVE flag will automatically save to your q3config.cfg
- The system initializes during renderer startup
- Generated maps are cached in memory for performance
- Use `vid_restart` after changing major settings
- The override system works with the existing material system
- Custom PBR maps can be placed in `materials/` folder to override generation

## Troubleshooting

If CVARs don't appear in config:
1. Make sure you've changed them from default values
2. Use `/writeconfig` to force save
3. Check that q3config.cfg is not read-only

If materials aren't generating:
1. Check `mat_status` for system state
2. Ensure `r_materialOverride` and `r_materialAutoGen` are both 1
3. Try `mat_regenerate` to force regeneration
4. Check console for error messages with `r_materialDebug 1`