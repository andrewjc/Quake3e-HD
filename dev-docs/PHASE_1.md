# Phase 1: Frontend/Backend Separation with Command Buffer Architecture

## Executive Summary

Phase 1 establishes the foundational dual-threaded renderer architecture inspired by DOOM 3 BFG's idRenderSystemLocal implementation. This phase creates a complete separation between the game-facing frontend (processing game state into render commands) and the GPU-facing backend (executing draw calls), connected via a lock-free double-buffered command system. This architecture enables parallel scene processing and GPU command submission while maintaining zero-copy efficiency for large data structures.

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

## Architectural Goals

Based on DOOM 3 BFG's design principles:
1. **Zero-Copy Operations**: Large data structures (vertices, indices) remain in shared memory
2. **Lock-Free Communication**: Frontend and backend communicate via atomic operations
3. **Frame Independence**: Backend can run at different rates than game logic
4. **Memory Coherency**: Careful data ownership to prevent race conditions
5. **Platform Abstraction**: Threading implementation abstracted for Windows/Linux/Mac

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

## GPU Resource Management

### Shared Resource Pool
```c
// File: src/engine/renderer/tr_resource.h (NEW FILE)

typedef struct gpuResource_s {
    union {
        GLuint      glHandle;
        VkBuffer    vkHandle;
    };
    size_t          size;
    void*           mappedPtr;      // For persistent mapping
    atomic_int      refCount;       // Reference counting
    qboolean        dirty;          // Needs GPU update
} gpuResource_t;

typedef struct resourcePool_s {
    gpuResource_t   *vertexBuffers[MAX_VERTEX_BUFFERS];
    gpuResource_t   *indexBuffers[MAX_INDEX_BUFFERS];
    gpuResource_t   *uniformBuffers[MAX_UNIFORM_BUFFERS];
    
    // Double-buffered dynamic buffers
    gpuResource_t   *dynamicVB[2];
    gpuResource_t   *dynamicIB[2];
    gpuResource_t   *dynamicUB[2];
    int             currentFrame;
    
    // Synchronization
    void*           resourceMutex;
} resourcePool_t;
```

### Resource Synchronization Strategy
1. **Static Resources**: Immutable after creation, freely shared
2. **Dynamic Resources**: Double-buffered per frame
3. **Persistent Mapping**: Use ARB_buffer_storage for zero-copy updates
4. **Fence Sync**: GPU fences to track resource availability

## Command Execution Pipeline

### Frontend Pipeline
```
Game Thread:
    RE_RenderScene() 
    → R_SetupViewParms()
    → R_GenerateDrawSurfs()
    → R_SortDrawSurfs()
    → R_BuildDrawCommands()
    → R_SwapCommandBuffers()
```

### Backend Pipeline  
```
Render Thread:
    RB_BackendThreadLoop()
    → RB_WaitForCommands()
    → RB_BeginFrame()
    → RB_ExecuteCommandBuffer()
    → RB_EndFrame()
    → RB_PresentFrame()
```

## Performance Optimizations

### Cache-Line Optimization
```c
// Align structures to cache lines (64 bytes typical)
#define CACHE_LINE_SIZE 64

typedef struct alignas(CACHE_LINE_SIZE) {
    // Frequently accessed together
    mat4_t          mvpMatrix;
    vec4_t          color;
    // Padding to fill cache line
    byte            _pad[CACHE_LINE_SIZE - sizeof(mat4_t) - sizeof(vec4_t)];
} renderConstants_t;
```

### SIMD Batch Processing
```c
// Process 4 surfaces at once with SSE/AVX
void R_CullSurfaces_SIMD(drawSurf_t *surfs, int count, vec4_t planes[6]) {
    __m128 planeX[6], planeY[6], planeZ[6], planeD[6];
    
    // Load frustum planes into SIMD registers
    for (int p = 0; p < 6; p++) {
        planeX[p] = _mm_set1_ps(planes[p][0]);
        planeY[p] = _mm_set1_ps(planes[p][1]);
        planeZ[p] = _mm_set1_ps(planes[p][2]);
        planeD[p] = _mm_set1_ps(planes[p][3]);
    }
    
    // Process 4 surfaces per iteration
    for (int i = 0; i < count; i += 4) {
        // SIMD culling implementation
    }
}
```

## Debugging and Profiling

### Performance Metrics
```c
typedef struct frameMetrics_s {
    // Timing
    double      frontendTime;       // Scene generation
    double      backendTime;        // GPU submission
    double      gpuTime;           // Actual GPU execution
    double      presentTime;       // Swap/present
    
    // Counters
    int         numDrawCalls;
    int         numTriangles;
    int         numStateChanges;
    int         numTextureBinds;
    
    // Memory
    size_t      commandBufferUsed;
    size_t      vertexBufferUsed;
    size_t      uniformBufferUsed;
} frameMetrics_t;
```

### Debug Visualization
- Command buffer usage overlay
- Thread timing graphs
- GPU pipeline stalls
- Memory allocation heatmap

## Success Criteria

Phase 1 is complete when:

1. ✓ Double-buffered command system operational
2. ✓ Render thread executing commands independently  
3. ✓ Frontend/backend fully separated
4. ✓ No visual regressions
5. ✓ Performance improvement measurable (15-30% in complex scenes)
6. ✓ All existing features functional
7. ✓ Debug/profiling tools implemented
8. ✓ Documentation updated
9. ✓ Zero-copy resource sharing implemented
10. ✓ Platform-specific threading abstracted

## Next Phase Dependencies

Phase 2 (Structured Scene Representation) requires:
- Command buffer system for drawSurf_t lists
- Thread-safe memory allocation
- Established frontend/backend separation

This foundation enables all subsequent phases to build upon the parallel architecture.