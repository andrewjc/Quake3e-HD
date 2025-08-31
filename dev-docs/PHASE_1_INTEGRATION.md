# Phase 1 Integration Guide

## Overview

This document provides step-by-step instructions for integrating the Phase 1 multi-threaded renderer implementation into the existing Quake3e-HD codebase.

## New Files Created

1. **src/engine/renderer/tr_cmdbuf.h** - Command buffer structures and declarations
2. **src/engine/renderer/tr_cmdbuf.c** - Command buffer implementation
3. **src/engine/renderer/tr_backend_thread.c** - Render thread management
4. **src/engine/renderer/tr_sync.c** - Platform synchronization primitives
5. **src/engine/renderer/tr_local.h** - Updated with new declarations

## Required Modifications to Existing Files

### 1. tr_init.c Modifications

Add initialization calls to `R_Init()`:

```c
// In R_Init() function, after existing initialization:
R_InitCommandBuffers();

// Only create render thread if SMP is enabled
if (r_smp->integer) {
    R_CreateRenderThread();
}
```

Add shutdown calls to `R_Shutdown()`:

```c
// In R_Shutdown() function:
if (r_smp->integer) {
    R_ShutdownRenderThread();
}
R_ShutdownCommandBuffers();
```

### 2. tr_cmds.c Modifications

Replace the existing `R_GetCommandBuffer()` function to use the new multi-threaded version:

```c
void *R_GetCommandBuffer(int bytes) {
    // Use the multi-threaded version if SMP is enabled
    if (r_smp && r_smp->integer) {
        return R_GetCommandBufferMT(bytes);
    }
    
    // Fall back to original implementation for single-threaded mode
    return R_GetCommandBufferReserved(bytes, PAD(sizeof(swapBuffersCommand_t), sizeof(void*)));
}
```

Modify `RE_EndFrame()` to use command buffer swapping:

```c
void RE_EndFrame(int *frontEndMsec, int *backEndMsec) {
    swapBuffersCommand_t *cmd;
    
    if (!tr.registered) {
        return;
    }
    
    // Add swap buffers command
    cmd = R_GetCommandBuffer(sizeof(*cmd));
    if (!cmd) {
        return;
    }
    cmd->commandId = RC_SWAP_BUFFERS;
    
    // Use new command buffer system if SMP is enabled
    if (r_smp && r_smp->integer) {
        R_SwapCommandBuffers();  // This triggers backend processing
    } else {
        R_IssueRenderCommands();  // Original single-threaded path
    }
    
    R_PerformanceCounters();
    R_InitNextFrame();
    
    if (frontEndMsec) {
        *frontEndMsec = tr.frontEndMsec;
    }
    tr.frontEndMsec = 0;
    
    if (backEndMsec) {
        *backEndMsec = backEnd.pc.msec;
    }
    backEnd.pc.msec = 0;
}
```

### 3. tr_scene.c Modifications

Update `RE_RenderScene()` to use the new draw view command:

```c
void RE_RenderScene(const refdef_t *fd) {
    viewParms_t parms;
    int startTime;
    
    // ... existing validation code ...
    
    if (r_smp && r_smp->integer) {
        // Multi-threaded path
        drawViewCommand_t *cmd;
        
        cmd = (drawViewCommand_t*)R_GetCommandBuffer(sizeof(drawViewCommand_t));
        if (!cmd) {
            return;
        }
        
        cmd->header.commandId = RC_DRAW_VIEW;
        cmd->header.commandSize = sizeof(drawViewCommand_t);
        
        // Deep copy parameters to avoid race conditions
        Com_Memcpy(&cmd->viewParms, &tr.viewParms, sizeof(viewParms_t));
        Com_Memcpy(&cmd->refdef, &tr.refdef, sizeof(refdef_t));
        
        // Generate and sort draw surfaces
        R_GenerateDrawSurfs();
        R_SortDrawSurfs(tr.refdef.drawSurfs, tr.refdef.numDrawSurfs);
        
        // Store pointers (data must remain valid until backend processes)
        cmd->drawSurfs = tr.refdef.drawSurfs;
        cmd->numDrawSurfs = tr.refdef.numDrawSurfs;
        cmd->dlights = tr.refdef.dlights;
        cmd->numDlights = tr.refdef.num_dlights;
        
#ifdef USE_PMLIGHT
        cmd->litSurfs = tr.refdef.litSurfs;
        cmd->numLitSurfs = tr.refdef.numLitSurfs;
#endif
    } else {
        // Original single-threaded path
        // ... existing code ...
    }
}
```

### 4. Build System Integration

#### CMakeLists.txt or Makefile

Add the new source files to the renderer build:

```cmake
# In the renderer sources section:
set(RENDERER_SOURCES
    # ... existing sources ...
    src/engine/renderer/tr_cmdbuf.c
    src/engine/renderer/tr_backend_thread.c
    src/engine/renderer/tr_sync.c
)
```

#### Visual Studio Project

Add the new files to the renderer project:
- Add tr_cmdbuf.h to Header Files
- Add tr_cmdbuf.c to Source Files
- Add tr_backend_thread.c to Source Files
- Add tr_sync.c to Source Files

### 5. Platform-Specific Considerations

#### Windows
- Requires Windows.h for threading primitives
- Link with kernel32.lib (usually automatic)

#### Linux/Unix
- Requires pthread library
- Add `-pthread` to compiler and linker flags
- May need `-D_GNU_SOURCE` for some pthread extensions

#### macOS
- Similar to Linux but may need additional frameworks
- pthread is part of the system libraries

## Testing Checklist

### Basic Functionality

- [ ] Game starts without errors
- [ ] Renderer initializes correctly
- [ ] r_smp 0 works (single-threaded mode)
- [ ] r_smp 1 works (multi-threaded mode)
- [ ] No visual artifacts or corruption
- [ ] Performance is equal or better

### Advanced Testing

- [ ] Alt-tab works correctly
- [ ] Resolution changes work
- [ ] Video settings changes work
- [ ] Demo recording/playback works
- [ ] Screenshots work
- [ ] No memory leaks
- [ ] No thread synchronization issues

### Debug Commands

Add console commands for testing:

```c
// In R_Register() or similar:
ri.Cmd_AddCommand("r_printThreadStats", R_PrintRenderThreadStats);
ri.Cmd_AddCommand("r_printBufferStats", R_PrintCommandBufferStats);
```

## Performance Monitoring

### CVars for Debugging

- `r_smp` - Enable/disable multi-threading (0/1)
- `r_showCommandBuffer` - Display command buffer usage
- `r_showThreadTiming` - Show thread timing statistics
- `r_maxCommandBuffer` - Limit command buffer size for testing

### Expected Performance

- **Single-threaded (r_smp 0)**: Should match original performance
- **Multi-threaded (r_smp 1)**: 15-30% improvement in complex scenes
- **CPU Usage**: Should show ~200% usage on dual-core or better

## Troubleshooting

### Common Issues and Solutions

1. **Crash on startup**
   - Check that all new files are included in build
   - Verify synchronization primitives initialized
   - Check for NULL pointer access in command buffer

2. **Visual corruption**
   - Ensure data is copied, not referenced
   - Check command buffer alignment
   - Verify thread synchronization

3. **Performance regression**
   - Check for mutex contention
   - Verify command buffer size is adequate
   - Profile synchronization overhead

4. **Build errors**
   - Add pthread library on Linux/Unix
   - Include Windows.h on Windows
   - Check for missing function declarations

## Rollback Plan

If issues arise, the implementation can be disabled:

1. Set `r_smp 0` to disable multi-threading
2. Comment out initialization in tr_init.c
3. Use original R_GetCommandBuffer implementation

The code is designed to fall back gracefully to single-threaded operation.

## Next Steps

Once Phase 1 is stable:

1. Profile and optimize synchronization
2. Begin Phase 2 (Structured Scene Representation)
3. Implement command buffer statistics UI
4. Add more granular threading controls

## Contact

For questions or issues with the integration, refer to the main PHASE_1.md document for technical details.