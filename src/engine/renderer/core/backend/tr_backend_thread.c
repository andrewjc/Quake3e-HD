/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

This file is part of Quake3e-HD.

Quake3e-HD is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Quake3e-HD is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake3e-HD; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
===========================================================================
*/
// tr_backend_thread.c - Render thread management

#include "../tr_local.h"
#include "../command/tr_cmdbuf.h"

// External cvar
extern cvar_t *r_showCommandBuffer;

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>
#endif

// Legacy SMP toggle (re-created for compatibility)
static cvar_t *r_smp = NULL;

// Render thread state
typedef struct renderThread_s {
    void*           threadHandle;     // Platform-specific thread handle
    qboolean        running;
    qboolean        shouldExit;
    int             frameCount;
    
    // Performance metrics
    double          lastFrameTime;
    double          avgFrameTime;
    double          maxFrameTime;
    double          minFrameTime;
    
    // Thread-local memory pool
    byte*           tempBuffer;
    size_t          tempBufferSize;
    size_t          tempBufferUsed;
} renderThread_t;

static renderThread_t renderThread;

// External functions
extern void Sys_WaitSemaphore(void* sem);
extern void Sys_SignalSemaphore(void* sem);
extern qboolean Sys_TryWaitSemaphore(void* sem);

// Forward declarations
static void RB_BackendThreadLoop(void);
static void RB_InitBackendContext(void);
static void RB_ShutdownBackendContext(void);

/*
================
Thread-Local Memory Management
================
*/
static void R_InitThreadMemory(void) {
    // Allocate 4MB thread-local temporary buffer
    renderThread.tempBufferSize = 4 * 1024 * 1024;
    renderThread.tempBuffer = (byte*)ri.Hunk_Alloc(renderThread.tempBufferSize, h_low);
    renderThread.tempBufferUsed = 0;
}

static void R_FreeThreadMemory(void) {
    // Memory is freed when hunk is cleared
    renderThread.tempBuffer = NULL;
    renderThread.tempBufferSize = 0;
    renderThread.tempBufferUsed = 0;
}

void* R_ThreadAllocTemp(size_t size) {
    void* ptr;
    
    // Align to 16 bytes for SIMD
    size = PAD(size, 16);
    
    if (renderThread.tempBufferUsed + size > renderThread.tempBufferSize) {
        ri.Error(ERR_DROP, "R_ThreadAllocTemp: overflow");
        return NULL;
    }
    
    ptr = renderThread.tempBuffer + renderThread.tempBufferUsed;
    renderThread.tempBufferUsed += size;
    
    return ptr;
}

void R_ThreadResetTemp(void) {
    renderThread.tempBufferUsed = 0;
}

/*
================
Platform-Specific Thread Creation
================
*/
#ifdef _WIN32

static DWORD WINAPI RB_BackendThreadProc(LPVOID param) {
    // Set thread name for debugging
    typedef struct tagTHREADNAME_INFO {
        DWORD dwType;
        LPCSTR szName;
        DWORD dwThreadID;
        DWORD dwFlags;
    } THREADNAME_INFO;
    
    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = "Render Backend";
    info.dwThreadID = GetCurrentThreadId();
    info.dwFlags = 0;
    
    __try {
        RaiseException(0x406D1388, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    
    // Run the backend loop
    RB_BackendThreadLoop();
    return 0;
}

void R_CreateRenderThread(void) {
    DWORD threadId;
    HANDLE thread;
    
    if (!r_smp) {
        r_smp = ri.Cvar_Get("r_smp", "0", CVAR_ARCHIVE | CVAR_LATCH);
    }

    if (!r_smp || !r_smp->integer) {
        return;
    }
    
    renderThread.shouldExit = qfalse;
    
    // Create the render thread
    thread = CreateThread(
        NULL,                   // Security attributes
        0,                      // Stack size (default)
        RB_BackendThreadProc,   // Thread function
        NULL,                   // Parameter
        0,                      // Creation flags
        &threadId              // Thread ID
    );
    
    if (!thread) {
        ri.Printf(PRINT_WARNING, "Failed to create render thread\n");
        ri.Cvar_Set("r_smp", "0");
        return;
    }
    
    // Set thread priority to above normal for consistent frame timing
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    
    // Set thread affinity to second core if available
    DWORD_PTR processAffinityMask, systemAffinityMask;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processAffinityMask, &systemAffinityMask)) {
        // Find second available core
        DWORD_PTR secondCore = 2;
        if (processAffinityMask & secondCore) {
            SetThreadAffinityMask(thread, secondCore);
        }
    }
    
    renderThread.threadHandle = thread;
    ri.Printf(PRINT_ALL, "Render thread created (ID: %lu)\n", threadId);
}

void R_ShutdownRenderThread(void) {
    if (!renderThread.threadHandle) {
        return;
    }
    
    // Signal thread to exit
    renderThread.shouldExit = qtrue;
    Sys_SignalSemaphore(cmdBufferState.renderSemaphore);
    
    // Wait for thread to finish
    WaitForSingleObject(renderThread.threadHandle, 5000);
    
    // Close thread handle
    CloseHandle(renderThread.threadHandle);
    renderThread.threadHandle = NULL;
    
    ri.Printf(PRINT_ALL, "Render thread shut down\n");
}

#elif defined(__linux__) || defined(__APPLE__)

static void* RB_BackendThreadProc(void* param) {
    // Set thread name for debugging
#ifdef __linux__
    pthread_setname_np(pthread_self(), "Render Backend");
#elif defined(__APPLE__)
    pthread_setname_np("Render Backend");
#endif
    
    // Run the backend loop
    RB_BackendThreadLoop();
    return NULL;
}

void R_CreateRenderThread(void) {
    pthread_t thread;
    pthread_attr_t attr;
    int result;
    
    if (!r_smp || !r_smp->integer) {
        return;
    }
    
    renderThread.shouldExit = qfalse;
    
    // Initialize thread attributes
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    
    // Create the render thread
    result = pthread_create(&thread, &attr, RB_BackendThreadProc, NULL);
    pthread_attr_destroy(&attr);
    
    if (result != 0) {
        ri.Printf(PRINT_WARNING, "Failed to create render thread: %d\n", result);
        ri.Cvar_Set("r_smp", "0");
        return;
    }
    
    // Set thread scheduling priority
#ifdef __linux__
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO) / 2;
    pthread_setschedparam(thread, SCHED_FIFO, &param);
#endif
    
    // Set CPU affinity to second core if available
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);  // Second core
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
#endif
    
    renderThread.threadHandle = (void*)thread;
    ri.Printf(PRINT_ALL, "Render thread created\n");
}

void R_ShutdownRenderThread(void) {
    pthread_t thread;
    void* retval;
    
    if (!renderThread.threadHandle) {
        return;
    }
    
    thread = (pthread_t)renderThread.threadHandle;
    
    // Signal thread to exit
    renderThread.shouldExit = qtrue;
    Sys_SignalSemaphore(cmdBufferState.renderSemaphore);
    
    // Wait for thread to finish
    pthread_join(thread, &retval);
    
    renderThread.threadHandle = NULL;
    ri.Printf(PRINT_ALL, "Render thread shut down\n");
}

#endif

/*
================
RB_InitBackendContext

Initialize rendering context for backend thread
Called once when thread starts
================
*/
static void RB_InitBackendContext(void) {
    // Initialize thread-local memory
    R_InitThreadMemory();
    
#ifdef USE_VULKAN
    if (glConfig.vidWidth) {
        // Vulkan context is already initialized and shared
        // Just need to ensure we're on the right thread for command buffer recording
        vk_initialize_backend_thread();
    }
#else
    // OpenGL context sharing would go here
    // Note: We're Vulkan-only per requirements
#endif
    
    // Initialize performance tracking
    renderThread.minFrameTime = 999999.0;
    renderThread.maxFrameTime = 0.0;
    renderThread.avgFrameTime = 0.0;
    renderThread.frameCount = 0;
    
    ri.Printf(PRINT_ALL, "Backend context initialized\n");
}

/*
================
RB_ShutdownBackendContext

Cleanup rendering context for backend thread
Called once when thread exits
================
*/
static void RB_ShutdownBackendContext(void) {
#ifdef USE_VULKAN
    if (glConfig.vidWidth) {
        vk_shutdown_backend_thread();
    }
#endif
    
    // Free thread-local memory
    R_FreeThreadMemory();
    
    ri.Printf(PRINT_ALL, "Backend context shut down\n");
}

/*
================
RB_BackendThreadLoop

Main loop for the render thread
Waits for commands and executes them
================
*/
static void RB_BackendThreadLoop(void) {
    commandBuffer_t *buffer;
    double startTime, frameTime;
    
    renderThread.running = qtrue;
    
    // Initialize backend context for this thread
    RB_InitBackendContext();
    
    ri.Printf(PRINT_ALL, "Render thread started\n");
    
    while (!renderThread.shouldExit) {
        // Wait for frontend to signal new commands
        Sys_WaitSemaphore(cmdBufferState.renderSemaphore);
        
        if (renderThread.shouldExit) {
            break;
        }
        
        // Get the buffer to process
        buffer = &cmdBufferState.buffers[cmdBufferState.renderBuffer];
        
        if (!buffer->ready) {
            continue;  // Nothing to do
        }
        
        // Mark buffer as being processed
        buffer->inUse = qtrue;
        
        // Reset thread-local temp memory for this frame
        R_ThreadResetTemp();
        
        // Track frame timing
        startTime = ri.Milliseconds();
        
        // Execute all commands in the buffer
        RB_ExecuteCommandBuffer(buffer);
        
        // Calculate frame time
        frameTime = ri.Milliseconds() - startTime;
        
        // Update statistics
        renderThread.frameCount++;
        renderThread.lastFrameTime = frameTime;
        renderThread.avgFrameTime = (renderThread.avgFrameTime * 0.95) + (frameTime * 0.05);
        
        if (frameTime < renderThread.minFrameTime) {
            renderThread.minFrameTime = frameTime;
        }
        if (frameTime > renderThread.maxFrameTime) {
            renderThread.maxFrameTime = frameTime;
        }
        
        // Mark buffer as processed
        buffer->ready = qfalse;
        buffer->inUse = qfalse;
        
        // Signal frontend that we're done
        Sys_SignalSemaphore(cmdBufferState.completeSemaphore);
        
        // Debug output
        if (r_showCommandBuffer && r_showCommandBuffer->integer) {
            if (renderThread.frameCount % 100 == 0) {
                ri.Printf(PRINT_ALL, "Backend: frame %d, time %.2fms (avg %.2fms, min %.2fms, max %.2fms)\n",
                         renderThread.frameCount, frameTime, renderThread.avgFrameTime,
                         renderThread.minFrameTime, renderThread.maxFrameTime);
            }
        }
    }
    
    // Cleanup backend context
    RB_ShutdownBackendContext();
    
    renderThread.running = qfalse;
    ri.Printf(PRINT_ALL, "Render thread stopped\n");
}

/*
================
R_GetRenderThreadStats

Get performance statistics from render thread
================
*/
void R_GetRenderThreadStats(double* avgTime, double* minTime, double* maxTime, int* frameCount) {
    if (avgTime) *avgTime = renderThread.avgFrameTime;
    if (minTime) *minTime = renderThread.minFrameTime;
    if (maxTime) *maxTime = renderThread.maxFrameTime;
    if (frameCount) *frameCount = renderThread.frameCount;
}

/*
================
R_IsRenderThreadRunning

Check if render thread is active
================
*/
qboolean R_IsRenderThreadRunning(void) {
    return renderThread.running;
}
