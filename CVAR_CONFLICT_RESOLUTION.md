# CVAR Conflict Resolution Documentation

## Resolved Conflicts

### 1. Fullscreen Mode CVARs
**Conflict**: Multiple CVARs controlling fullscreen behavior
- `r_fullscreen` (legacy, now extended)
- `r_windowMode` (new string-based)
- `r_noborder` (window border control)

**Resolution**:
- Extended `r_fullscreen` to support value `2` for borderless mode
- Implemented bidirectional synchronization between `r_fullscreen` and `r_windowMode`
- Last-modified CVAR takes precedence
- Alt+Enter cycles through all three modes (0→1→2→0)

### 2. Resolution CVARs
**Potential Conflict**: Multiple ways to set resolution
- `r_mode` (preset resolutions)
- `r_customwidth`/`r_customheight` (custom resolution)
- `r_modeFullscreen` (dedicated fullscreen resolution)

**Resolution**:
- `r_mode -1` uses custom resolution
- `r_mode -2` uses desktop resolution
- `r_modeFullscreen` only applies in exclusive fullscreen
- Clear priority order established

## Implementation Details

### Code Changes Made

1. **src/game/client/cl_main.c**
   - Extended `r_fullscreen` to accept value 2
   - Added sync logic to update `r_windowMode` when `r_fullscreen` changes
   - Added sync logic to update `r_fullscreen` when `r_windowMode` changes

2. **src/platform/windows/win_glimp.c**
   - Modified `GetWindowMode()` to recognize `r_fullscreen 2` as borderless
   - Updated window creation logic for three distinct modes

3. **src/platform/windows/win_wndproc.c**
   - Alt+Enter handler cycles through 0→1→2→0
   - Different window styles for each mode

4. **src/platform/sdl/sdl_glimp.c**
   - SDL backend recognizes all three modes
   - Proper SDL window flags for borderless

## Usage Guidelines

### For Users
1. **Choose one system** - Either use `r_fullscreen` OR `r_windowMode`, not both
2. **Save settings** - Use `writeconfig` after finding preferred setup
3. **Clean migration** - Remove duplicate entries from old configs

### For Developers
1. **Check both CVARs** - When reading fullscreen state, check both systems
2. **Maintain sync** - Any changes to one CVAR must update the other
3. **Preserve user preference** - Last-modified CVAR wins in conflicts

## Testing Checklist

- [x] `r_fullscreen 0` sets windowed mode
- [x] `r_fullscreen 1` sets exclusive fullscreen
- [x] `r_fullscreen 2` sets borderless fullscreen
- [x] `r_windowMode "windowed"` sets `r_fullscreen 0`
- [x] `r_windowMode "fullscreen"` sets `r_fullscreen 1`
- [x] `r_windowMode "borderless"` sets `r_fullscreen 2`
- [x] Alt+Enter cycles through all three modes
- [x] Config files save and restore correct mode
- [x] No conflicts on startup with mixed old/new configs

## Migration Path

### From Old Configs
```
// Old config (no borderless support)
seta r_fullscreen "1"

// New config (full support)
seta r_fullscreen "2"  // or
seta r_windowMode "fullscreen_windowed"
```

### Cleanup Script
```console
// Remove old settings
cvar_reset r_fullscreen
cvar_reset r_windowMode
cvar_reset r_noborder

// Set new preference
r_fullscreen 2  // or r_windowMode "borderless"
writeconfig
```

## Known Issues

1. **Platform Differences**
   - Linux: Window manager dependent behavior
   - macOS: Limited exclusive fullscreen support
   - Windows: Full support for all three modes

2. **Performance**
   - Borderless may have 1-2% overhead vs exclusive
   - DWM composition affects borderless on Windows

## Future Improvements

1. **Automatic Migration**
   - Detect old configs and auto-convert
   - Warning for conflicting settings

2. **Enhanced Detection**
   - Better multi-monitor support
   - HDR mode detection

3. **Additional Modes**
   - Fullscreen exclusive with vsync override
   - Per-monitor fullscreen settings