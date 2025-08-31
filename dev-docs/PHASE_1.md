# Phase 1: Multi-threading and Command Buffer Implementation

## Executive Summary

Phase 1 establishes the foundational architecture for the renderer upgrade by implementing a true multi-threaded frontend/backend separation with a double-buffered command system. This phase transforms the existing single-threaded renderer into a parallel architecture while maintaining full compatibility with existing game code and assets.

## Current State Analysis

### Existing Architecture
- **Single-threaded execution**: All rendering occurs on the main thread
- **Command structure**: Uses `renderCommandList_t` with enum-based commands
- **Data structures**:
  - `backEndData_t`: Contains drawSurfs, entities, dlights, and commands
  - `renderCommandList_t`: Simple byte buffer with command data
  - Command types: RC_DRAW_SURFS, RC_SET_COLOR, RC_SWAP_BUFFERS, etc.
- **Backend location**: `tr_backend.c` with `RB_ExecuteRenderCommands()`
- **Frontend location**: `tr_main.c`, `tr_cmds.c`

### Current Data Flow
```
Game → RE_RenderScene() → R_AddDrawSurf() → backEndData->commands → RB_ExecuteRenderCommands()
```

## Implementation Requirements

### 1. New Command Buffer System

#### Data Structures to Add

```c
// File: src/engine/renderer/tr_cmdbuf.h (NEW FILE)

#define CMD_BUFFER_SIZE (16 * 1024 * 1024)  // 16MB per buffer
#define NUM_CMD_BUFFERS 2

typedef struct commandBuffer_s {
    byte        data[CMD_BUFFER_SIZE];
    size_t      used;
    size_t      reserved;      // For in-progress allocations
    int         frameNumber;
    qboolean    ready;         // Ready for backend consumption
    qboolean    inUse;        // Currently being written/read
} commandBuffer_t;

typedef struct commandBufferState_s {
    commandBuffer_t buffers[NUM_CMD_BUFFERS];
    int             currentBuffer;    // Index for frontend writing
    int             renderBuffer;     // Index for backend reading
    
    // Synchronization
    void*           swapMutex;        // Platform-specific mutex
    void*           renderSemaphore;  // Signals backend to start
    void*           completeSemaphore; // Signals frontend completion done
} commandBufferState_t;

// High-level command structures (replacing enum-based system)
typedef struct baseCommand_s {
    renderCommand_t commandId;
    size_t          commandSize;
} baseCommand_t;

typedef struct drawViewCommand_s {
    baseCommand_t   header;
    viewParms_t     viewParms;       // Copied, not pointer
    drawSurf_t**    drawSurfs;       // Pointer to sorted list
    int             numDrawSurfs;
    dlight_t*       dlights;         // Dynamic lights
    int             numDlights;
#ifdef USE_PMLIGHT
    litSurf_t*      litSurfs;
    int             numLitSurfs;
#endif
} drawViewCommand_t;

typedef struct swapBuffersCommand_s {
    baseCommand_t   header;
    int             vsync;
} swapBuffersCommand_t;
```

#### Functions to Implement

```c
// File: src/engine/renderer/tr_cmdbuf.c (NEW FILE)

void R_InitCommandBuffers(void);
void R_ShutdownCommandBuffers(void);
void* R_GetCommandBuffer(size_t bytes);
void R_SwapCommandBuffers(void);
void R_WaitForBackend(void);
qboolean R_BackendBusy(void);
```

### 2. Render Thread Implementation

#### Thread Management Structure

```c
// File: src/engine/renderer/tr_backend_thread.c (NEW FILE)

typedef struct renderThread_s {
    void*           threadHandle;     // Platform-specific thread handle
    qboolean        running;
    qboolean        shouldExit;
    int             frameCount;
    
    // Performance metrics
    double          lastFrameTime;
    double          avgFrameTime;
    double          maxFrameTime;
} renderThread_t;

static renderThread_t renderThread;
```

#### Platform-Specific Thread Creation

```c
// Windows implementation
#ifdef _WIN32
static DWORD WINAPI RB_BackendThreadProc(LPVOID param) {
    RB_BackendThreadLoop();
    return 0;
}

void R_CreateRenderThread(void) {
    renderThread.threadHandle = CreateThread(
        NULL, 0, RB_BackendThreadProc, NULL, 0, NULL);
    SetThreadPriority(renderThread.threadHandle, THREAD_PRIORITY_ABOVE_NORMAL);
}
#endif

// Linux/Unix implementation  
#ifdef __linux__
static void* RB_BackendThreadProc(void* param) {
    RB_BackendThreadLoop();
    return NULL;
}

void R_CreateRenderThread(void) {
    pthread_t thread;
    pthread_attr_t attr;
    
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    
    pthread_create(&thread, &attr, RB_BackendThreadProc, NULL);
    renderThread.threadHandle = (void*)thread;
    
    pthread_attr_destroy(&attr);
}
#endif
```

### 3. Frontend Modifications

#### Modified Files and Functions

**File: src/engine/renderer/tr_main.c**

```c
// Replace direct command generation with buffered approach
void RE_RenderScene(const refdef_t *fd) {
    drawViewCommand_t *cmd;
    
    // Previous validation code remains...
    
    // Allocate command from new buffer system
    cmd = (drawViewCommand_t*)R_GetCommandBuffer(sizeof(drawViewCommand_t));
    
    // Copy view parameters (not pointer)
    cmd->header.commandId = RC_DRAW_VIEW;
    cmd->header.commandSize = sizeof(drawViewCommand_t);
    
    // Deep copy viewParms to avoid race conditions
    Com_Memcpy(&cmd->viewParms, &tr.viewParms, sizeof(viewParms_t));
    
    // Generate draw surfaces as before
    R_GenerateDrawSurfs();
    
    // Sort surfaces
    R_SortDrawSurfs(tr.refdef.drawSurfs, tr.refdef.numDrawSurfs);
    
    // Store sorted list pointer
    cmd->drawSurfs = tr.refdef.drawSurfs;
    cmd->numDrawSurfs = tr.refdef.numDrawSurfs;
    
    // Copy dlights
    cmd->dlights = tr.refdef.dlights;
    cmd->numDlights = tr.refdef.num_dlights;
}
```

**File: src/engine/renderer/tr_cmds.c**

```c
// Modified swap buffers command
void RE_SwapBuffers(void) {
    swapBuffersCommand_t *cmd;
    
    // Wait if backend is too far behind (>1 frame)
    if (R_BackendBusy()) {
        R_WaitForBackend();
    }
    
    cmd = (swapBuffersCommand_t*)R_GetCommandBuffer(sizeof(swapBuffersCommand_t));
    cmd->header.commandId = RC_SWAP_BUFFERS;
    cmd->header.commandSize = sizeof(swapBuffersCommand_t);
    cmd->vsync = r_swapInterval->integer;
    
    // Signal backend and swap buffers
    R_SwapCommandBuffers();
}
```

### 4. Backend Modifications

**File: src/engine/renderer/tr_backend.c**

```c
// New command execution function
void RB_ExecuteCommandBuffer(const commandBuffer_t *buffer) {
    const byte *data = buffer->data;
    const byte *dataEnd = data + buffer->used;
    
    while (data < dataEnd) {
        const baseCommand_t *cmd = (const baseCommand_t*)data;
        
        switch (cmd->commandId) {
        case RC_DRAW_VIEW:
            {
                const drawViewCommand_t *dvc = (const drawViewCommand_t*)cmd;
                RB_DrawView(&dvc->viewParms, dvc->drawSurfs, 
                           dvc->numDrawSurfs, dvc->dlights, dvc->numDlights);
            }
            break;
            
        case RC_SWAP_BUFFERS:
            {
                const swapBuffersCommand_t *sc = (const swapBuffersCommand_t*)cmd;
                RB_SwapBuffers(sc->vsync);
            }
            break;
            
        // Other commands...
        }
        
        data += cmd->commandSize;
    }
}

// Backend thread main loop
void RB_BackendThreadLoop(void) {
    renderThread.running = qtrue;
    
    // Initialize backend GL/Vulkan context for this thread
    RB_InitBackendContext();
    
    while (!renderThread.shouldExit) {
        // Wait for frontend signal
        Sys_WaitSemaphore(cmdBufferState.renderSemaphore);
        
        if (renderThread.shouldExit)
            break;
            
        // Process commands
        commandBuffer_t *buffer = &cmdBufferState.buffers[cmdBufferState.renderBuffer];
        
        double startTime = Sys_Milliseconds();
        RB_ExecuteCommandBuffer(buffer);
        double frameTime = Sys_Milliseconds() - startTime;
        
        // Update metrics
        renderThread.frameCount++;
        renderThread.lastFrameTime = frameTime;
        renderThread.avgFrameTime = (renderThread.avgFrameTime * 0.95) + (frameTime * 0.05);
        
        // Signal completion
        Sys_SignalSemaphore(cmdBufferState.completeSemaphore);
    }
    
    RB_ShutdownBackendContext();
    renderThread.running = qfalse;
}
```

### 5. Synchronization Points

#### Critical Synchronization Locations

1. **Command Buffer Swap** - Mutex protected
2. **Frame Start** - Semaphore signaling
3. **Resource Updates** - Deferred to safe points
4. **Shutdown** - Graceful thread termination

```c
// File: src/engine/renderer/tr_cmdbuf.c

void R_SwapCommandBuffers(void) {
    Sys_LockMutex(cmdBufferState.swapMutex);
    
    // Mark current buffer as ready
    cmdBufferState.buffers[cmdBufferState.currentBuffer].ready = qtrue;
    cmdBufferState.buffers[cmdBufferState.currentBuffer].inUse = qfalse;
    
    // Swap indices
    int temp = cmdBufferState.currentBuffer;
    cmdBufferState.currentBuffer = cmdBufferState.renderBuffer;
    cmdBufferState.renderBuffer = temp;
    
    // Reset new current buffer
    cmdBufferState.buffers[cmdBufferState.currentBuffer].used = 0;
    cmdBufferState.buffers[cmdBufferState.currentBuffer].ready = qfalse;
    cmdBufferState.buffers[cmdBufferState.currentBuffer].inUse = qtrue;
    
    Sys_UnlockMutex(cmdBufferState.swapMutex);
    
    // Signal render thread
    Sys_SignalSemaphore(cmdBufferState.renderSemaphore);
}
```

## Integration Points

### Modified Existing Files

1. **tr_init.c**
   - Add `R_InitCommandBuffers()` to `R_Init()`
   - Add `R_CreateRenderThread()` after GL/Vulkan context creation
   - Add `R_ShutdownRenderThread()` to `R_Shutdown()`

2. **tr_local.h**
   - Add new command buffer structures
   - Add thread-related function declarations
   - Add synchronization primitive declarations

3. **tr_backend.c**
   - Split into thread-safe and thread-local sections
   - Move GL/Vulkan context management to thread-local

4. **tr_main.c**
   - Replace direct `backEndData` usage with command buffer allocation
   - Ensure all data is copied, not referenced

### New Files to Create

1. **tr_cmdbuf.h** - Command buffer definitions
2. **tr_cmdbuf.c** - Command buffer implementation
3. **tr_backend_thread.c** - Render thread management
4. **tr_sync.c** - Platform-specific synchronization wrappers

## Memory Management Considerations

### Per-Frame Allocations

- Command buffers are pre-allocated (16MB x 2)
- DrawSurf arrays remain in main memory, only pointers passed
- View parameters are deep-copied to avoid races
- Temporary allocations use thread-local storage

### Thread-Local Storage

```c
// Each thread gets its own temporary memory pool
typedef struct threadMemory_s {
    byte    tempBuffer[4 * 1024 * 1024];  // 4MB temp space
    size_t  tempUsed;
    int     frameNum;
} threadMemory_t;

__thread threadMemory_t threadMemory;  // GCC/Clang
__declspec(thread) threadMemory_t threadMemory;  // MSVC
```

## Compatibility Requirements

### Maintained Interfaces

1. All public RE_* functions remain unchanged
2. Existing cvars continue to work
3. Mod compatibility preserved
4. Demo playback unaffected

### Backward Compatibility

- Single-threaded fallback mode via `r_smp 0` cvar
- Legacy command system wrapper for old render paths
- Existing shader/material system unchanged (Phase 3 will address)

## Testing Strategy

### Validation Steps

1. **Functional Testing**
   - Verify identical visual output
   - Test all render paths (world, models, sprites, etc.)
   - Validate dynamic lights and effects
   - Check demo recording/playback

2. **Performance Testing**
   - Measure frame time improvements
   - Monitor CPU core utilization
   - Check memory usage patterns
   - Profile synchronization overhead

3. **Stress Testing**
   - High entity counts
   - Complex scenes with many lights
   - Rapid view changes
   - Alt-tab and resolution changes

### Debug Features

```c
// Debug cvars
cvar_t *r_showCommandBuffer;  // Display buffer usage
cvar_t *r_showThreadTiming;   // Show thread timing stats
cvar_t *r_smp;                // Enable/disable threading
cvar_t *r_maxCommandBuffer;   // Limit buffer size for testing
```

## Performance Targets

### Expected Improvements

- **CPU Utilization**: 180-200% (2 cores fully utilized)
- **Frame Time**: 15-30% reduction in complex scenes
- **Latency**: < 1 frame additional latency
- **Memory**: +32MB for double buffering

### Bottleneck Mitigation

1. **Command Buffer Size**: Dynamically adjust based on usage
2. **Synchronization**: Use lock-free structures where possible
3. **Memory Bandwidth**: Minimize data copying
4. **Cache Coherency**: Align structures to cache lines

## Risk Assessment

### Technical Risks

1. **GL Context Sharing**: Some drivers have issues with multi-threaded GL
   - Mitigation: Vulkan-first approach, GL compatibility mode
   
2. **Synchronization Overhead**: Mutex contention could negate benefits
   - Mitigation: Fine-grained locking, lock-free alternatives
   
3. **Memory Coherency**: Data races in shared structures
   - Mitigation: Clear ownership model, extensive testing

### Implementation Risks

1. **Regression Bugs**: Subtle timing-dependent issues
   - Mitigation: Extensive automated testing, gradual rollout
   
2. **Platform Differences**: Threading behavior varies by OS
   - Mitigation: Platform-specific implementations, fallback modes

## Success Criteria

Phase 1 is complete when:

1. ✓ Double-buffered command system operational
2. ✓ Render thread executing commands independently  
3. ✓ Frontend/backend fully separated
4. ✓ No visual regressions
5. ✓ Performance improvement measurable
6. ✓ All existing features functional
7. ✓ Debug/profiling tools implemented
8. ✓ Documentation updated

## Next Phase Dependencies

Phase 2 (Structured Scene Representation) requires:
- Command buffer system for drawSurf_t lists
- Thread-safe memory allocation
- Established frontend/backend separation

This foundation enables all subsequent phases to build upon the parallel architecture.