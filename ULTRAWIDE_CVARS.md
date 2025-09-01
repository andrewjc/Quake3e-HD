# Ultrawide Display Support CVARs

## Correct CVARs (Use These)

The following CVARs control ultrawide display support in Quake3e-HD:

### Main Controls
- **`r_ultraWide`** (0/1, default: 1)  
  Enable ultra-widescreen perspective correction

- **`r_ultraWideMode`** (0-4, default: 1)  
  Ultra-wide rendering mode:
  - 0 = Single frustum (standard)
  - 1 = Triple region (recommended for 21:9)
  - 2 = Quintuple region (for 32:9)
  - 3 = Cylindrical projection
  - 4 = Panini projection

- **`r_ultraWideFOVScale`** (0.8-1.2, default: 1.0)  
  FOV scaling factor for ultra-wide displays

### Additional Settings
- **`r_paniniDistance`** (0.5-1.5, default: 1.0)  
  Panini projection distance parameter (only used when r_ultraWideMode=4)

- **`r_hudSafeZone`** (0/1, default: 1)  
  Constrain HUD elements to 16:9 safe zone on ultra-wide displays

- **`r_ultraWideDebug`** (0/1, default: 0)  
  Show ultra-wide debug info and region boundaries

## Deprecated/Invalid CVARs

The following CVARs may appear in old configs but are NOT used:
- ~~`r_ultrawide_enable`~~ - Use `r_ultraWide` instead
- ~~`r_ultrawide_fov_scale`~~ - Use `r_ultraWideFOVScale` instead
- ~~`r_ultrawide_hud_scale`~~ - Use `r_hudSafeZone` instead
- ~~`r_ultrawide_weapon_fov`~~ - Not implemented

## Recommended Settings

### For 21:9 (2560x1080, 3440x1440)
```
seta r_ultraWide "1"
seta r_ultraWideMode "1"
seta r_ultraWideFOVScale "1.0"
seta r_hudSafeZone "1"
```

### For 32:9 (3840x1080, 5120x1440)
```
seta r_ultraWide "1"
seta r_ultraWideMode "2"
seta r_ultraWideFOVScale "1.0"
seta r_hudSafeZone "1"
```

### For Panini Projection (Less distortion at edges)
```
seta r_ultraWide "1"
seta r_ultraWideMode "4"
seta r_paniniDistance "1.0"
seta r_ultraWideFOVScale "1.0"
seta r_hudSafeZone "1"
```

## Cleaning Up Your Config

To clean up your config file, remove these lines:
```
seta r_ultrawide_enable
seta r_ultrawide_fov_scale
seta r_ultrawide_hud_scale
seta r_ultrawide_weapon_fov
```

And ensure you only have the correct CVARs listed above.