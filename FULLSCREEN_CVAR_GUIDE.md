# Fullscreen CVAR System Documentation

## Overview
Quake3e-HD now has a unified fullscreen system that properly supports borderless windowed mode. There are two CVAR systems that work together - the legacy `r_fullscreen` and the new `r_windowMode`.

## CVAR Systems

### 1. **r_fullscreen** (Legacy - Enhanced)
The classic CVAR, now enhanced to support three modes:
- `0` - Windowed mode
- `1` - Exclusive fullscreen
- `2` - Borderless fullscreen (windowed)

```console
r_fullscreen 2    // Set borderless fullscreen
vid_restart       // Apply changes
```

### 2. **r_windowMode** (New - Recommended)
A more descriptive string-based system:
- `"windowed"` - Traditional windowed mode
- `"fullscreen"` - Exclusive fullscreen
- `"fullscreen_windowed"` or `"borderless"` - Borderless fullscreen

```console
r_windowMode "fullscreen_windowed"
vid_restart
```

### 3. **r_noborder** (Window Border Control)
Controls window border in windowed mode:
- `0` - Normal window with title bar
- `1` - Borderless window (no title bar)

```console
r_fullscreen 0    // Windowed mode
r_noborder 1      // Remove borders
vid_restart
```

## CVAR Synchronization

The two systems automatically sync with each other:

- Setting `r_fullscreen 2` automatically sets `r_windowMode "fullscreen_windowed"`
- Setting `r_windowMode "borderless"` automatically sets `r_fullscreen 2`
- Changes to either CVAR update the other

## Resolution Control

### Desktop Resolution
```console
r_mode -2         // Use current desktop resolution
r_fullscreen 2    // Borderless mode
vid_restart
```

### Custom Resolution
```console
r_mode -1         // Use custom resolution
r_customwidth 2560
r_customheight 1440
r_fullscreen 2    // Borderless mode
vid_restart
```

### Dedicated Fullscreen Resolution
```console
r_modeFullscreen "-2"  // Desktop res for exclusive fullscreen
r_fullscreen 1         // Exclusive mode
vid_restart
```

## Keyboard Shortcuts

**Alt+Enter** now cycles through all three modes:
1. Windowed → 
2. Exclusive Fullscreen → 
3. Borderless Fullscreen → 
4. (back to) Windowed

## Common Configurations

### Best for Streaming/Recording
```console
r_fullscreen 2
r_mode -2
vid_restart
```
- No screen flicker when Alt+Tabbing
- Smooth OBS/streaming capture
- Instant window switching

### Maximum Performance
```console
r_fullscreen 1
r_mode 0  // Or your preferred mode
vid_restart
```
- Exclusive fullscreen
- Lowest input latency
- Full GPU control

### Development/Testing
```console
r_fullscreen 0
r_noborder 0
r_mode 3  // 640x480 or preferred
vid_restart
```
- Easy debugging
- Quick resizing
- Multiple monitor friendly

## Conflict Resolution

### Priority Order
1. `r_windowMode` (if modified last)
2. `r_fullscreen` (if modified last)
3. Default: `r_fullscreen 1` (exclusive)

### Removing Old Configs
If you have old configs with conflicting values:
```console
// Clear old settings
r_fullscreen 2
r_windowMode "fullscreen_windowed"
r_noborder 0
writeconfig  // Save clean config
```

## Platform Differences

### Windows
- All three modes fully supported
- DWM composition affects borderless performance
- Exclusive fullscreen bypasses Windows compositor

### Linux (X11/Wayland)
- Borderless support depends on window manager
- Some WMs treat `r_noborder 1` as borderless
- Wayland may have different behavior

### macOS
- Limited exclusive fullscreen support
- Borderless works via `r_noborder 1`
- System fullscreen spaces affect behavior

## Troubleshooting

### CVARs Not Syncing?
```console
// Force sync
r_fullscreen 2
r_windowMode "fullscreen_windowed"
vid_restart
```

### Alt+Enter Not Working?
- Check if another program is capturing Alt+Enter
- Some overlays (Discord, Steam) may interfere
- Try manual CVAR setting instead

### Wrong Mode After Startup?
- Check both `r_fullscreen` and `r_windowMode` in config
- Ensure only one is being set in autoexec.cfg
- Use `writeconfig` after setting desired mode

## Technical Details

### CVAR Flags
- `CVAR_ARCHIVE` - Saved to config
- `CVAR_LATCH` - Requires vid_restart

### Initialization Order
1. `r_windowMode` checked first (if set)
2. Falls back to `r_fullscreen`
3. Defaults to windowed if neither set

### Window Styles
- **Exclusive**: `WS_POPUP` style, TOPMOST
- **Borderless**: `WS_POPUP` style, no TOPMOST
- **Windowed**: `WS_OVERLAPPEDWINDOW` style

## Best Practices

1. **Choose One System**: Use either `r_fullscreen` or `r_windowMode`, not both
2. **Save Settings**: Use `writeconfig` after finding your preferred setup
3. **Test Alt+Tab**: Ensure it works smoothly in your chosen mode
4. **Monitor Performance**: Borderless may have 1-2% overhead vs exclusive

## Migration from Old Configs

If your config has old settings:
```console
// Old way (doesn't support borderless)
seta r_fullscreen "1"

// New way (supports all modes)
seta r_fullscreen "2"  // or
seta r_windowMode "fullscreen_windowed"
```

Remove duplicate entries and keep only one system active.