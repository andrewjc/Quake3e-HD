# Fullscreen Windowed Mode Fix

## Overview
The fullscreen windowed (borderless) mode has been fixed to properly handle Alt+Tab and focus changes like modern games.

## Problems Fixed

### Before:
- Mouse cursor was captured even when Alt+Tabbed
- Window stayed on top preventing access to other windows
- Alt+Tab didn't work properly in borderless fullscreen
- Mouse input continued even when window lost focus
- Cursor remained hidden when switching applications

### After:
- Proper Alt+Tab support like modern games
- Mouse automatically releases when window loses focus
- Cursor becomes visible when switching apps
- Window doesn't force itself on top in borderless mode
- Clean focus transitions without input bleeding

## Technical Changes

### 1. **Mouse Capture Logic** (`win_input.c`)
- Added borderless fullscreen detection
- Mouse only captures when window is active
- Automatic release on focus loss
- Cursor visibility properly managed

### 2. **Window Focus Handling** (`win_wndproc.c`)
- Differentiate between exclusive and borderless fullscreen
- Don't set HWND_TOPMOST in borderless mode
- Proper WM_ACTIVATE handling for borderless

### 3. **Input Frame Updates** (`win_input.c`)
- Check for borderless mode in IN_Frame
- Release mouse in console when borderless
- Allow Alt+Tab in all non-exclusive modes

## Usage

### Setting Fullscreen Windowed Mode:

**Method 1: Using r_fullscreen (Legacy)**
```
r_fullscreen 2    // 0=windowed, 1=exclusive, 2=borderless
vid_restart
```

**Method 2: Via Window Mode**
```
r_windowMode 2      // 0=windowed, 1=exclusive, 2=borderless
vid_restart
```

**Method 3: Borderless Window**
```
r_fullscreen 0
r_noborder 1
vid_restart
```

### Best Settings for Modern Experience:
```
// Borderless fullscreen at desktop resolution
r_fullscreen 2
r_mode -2           // Use desktop resolution
vid_restart

// Or manually set resolution
r_fullscreen 2
r_customwidth 2560
r_customheight 1440
r_mode -1
vid_restart
```

## Benefits

1. **Seamless Alt+Tab** - Switch between game and other apps instantly
2. **Multi-Monitor Support** - Move cursor to other monitors when in menus
3. **Streaming Friendly** - No screen flicker when switching windows
4. **Modern UX** - Behaves like contemporary games (CS:GO, Valorant, etc.)
5. **No Performance Loss** - Same FPS as exclusive fullscreen

## Keyboard Shortcuts

- **Alt+Tab** - Switch between applications (now works properly)
- **Win+D** - Show desktop (game minimizes correctly)
- **Alt+Enter** - Toggle between windowed and fullscreen
- **Win+Tab** - Task view (game doesn't interfere)

## Troubleshooting

### Mouse Still Captured After Alt+Tab?
- Make sure `r_fullscreen` is set to 2, not 1
- Try `in_mouse -1` for Win32 mouse mode
- Restart with `vid_restart` after changing settings

### Window Still On Top?
- Verify you're using borderless mode (`r_fullscreen 2`)
- Check that exclusive fullscreen is not forced
- Some overlays may interfere (disable if needed)

### Performance Issues?
- Borderless mode may have slight overhead on some systems
- Try exclusive fullscreen (`r_fullscreen 1`) for maximum performance
- Disable Windows Game Mode if causing issues

## Developer Notes

The fix works by:
1. Detecting borderless fullscreen state
2. Bypassing HWND_TOPMOST window flag in borderless
3. Releasing mouse capture on focus loss
4. Managing cursor visibility based on focus state
5. Allowing normal Windows focus switching

This brings Quake3e-HD in line with modern game windowing standards.