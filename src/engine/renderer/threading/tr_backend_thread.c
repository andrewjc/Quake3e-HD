/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "../tr_local.h"
#include "../tr_cmdbuf.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

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
} renderThread_t;

static renderThread_t renderThread;

// External synchronization functions
extern void* Sys_CreateSemaphore(void);
extern void Sys_DestroySemaphore(void* sem);
extern void Sys_WaitSemaphore(void* sem);
extern void Sys_SignalSemaphore(void* sem);

// Forward declarations
void RB_BackendThreadLoop(void);
void RB_InitBackendContext(void);
void RB_ShutdownBackendContext(void);

/*
================
RB_BackendThreadLoop

Main loop for the render thread
Waits for commands from frontend and executes them
================
*/
void RB_BackendThreadLoop(void) {
    commandBuffer_t *buffer;
    double startTime, frameTime;
    
    renderThread.running = qtrue;
    renderThread.minFrameTime = 999999.0;
    
    ri.Printf(PRINT_ALL, "Render thread started\n");
    
    // Initialize backend GL/Vulkan context for this thread
    RB_InitBackendContext();
    
    while (!renderThread.shouldExit) {
        // Wait for frontend to signal that commands are ready
        Sys_WaitSemaphore(cmdBufferState.renderSemaphore);
        
        if (renderThread.shouldExit) {
            break;
        }
        
        // Get the buffer to process
        buffer = &cmdBufferState.buffers[cmdBufferState.renderBuffer];
        
        if (!buffer->ready) {
            ri.Printf(PRINT_WARNING, "RB_BackendThreadLoop: buffer not ready\n");
            continue;
        }
        
        // Mark buffer as being processed
        buffer->inUse = qtrue;
        
        // Record start time
        startTime = ri.Milliseconds();
        
        // Execute all commands in the buffer
        RB_ExecuteCommandBuffer(buffer);
        
        // Calculate frame time
        frameTime = ri.Milliseconds() - startTime;
        
        // Update metrics
        renderThread.frameCount++;
        renderThread.lastFrameTime = frameTime;
        renderThread.avgFrameTime = (renderThread.avgFrameTime * 0.95) + (frameTime * 0.05);
        
        if (frameTime > renderThread.maxFrameTime) {
            renderThread.maxFrameTime = frameTime;
        }
        if (frameTime < renderThread.minFrameTime) {
            renderThread.minFrameTime = frameTime;
        }
        
        // Mark buffer as processed
        buffer->ready = qfalse;
        buffer->inUse = qfalse;
        
        // Signal frontend that we're done
        Sys_SignalSemaphore(cmdBufferState.completeSemaphore);
    }
    
    // Cleanup backend context
    RB_ShutdownBackendContext();
    
    ri.Printf(PRINT_ALL, "Render thread stopped\n");
    renderThread.running = qfalse;
}

/*
================
RB_InitBackendContext

Initialize OpenGL/Vulkan context for the render thread
This needs to happen on the thread that will use it
================
*/
void RB_InitBackendContext(void) {
#ifdef USE_VULKAN
    // Vulkan doesn't need per-thread context
    // Commands will be recorded from this thread
#else
    // OpenGL context needs to be made current on this thread
    // Platform-specific GL context handling would go here
    // This is highly platform-dependent and would need integration
    // with the windowing system
#endif
    
    // Initialize any thread-local backend state
    backEnd.pc.msec = 0;
    backEnd.isHyperspace = qfalse;
    backEnd.currentEntity = NULL;
}

/*
================
RB_ShutdownBackendContext

Cleanup backend context when thread exits
================
*/
void RB_ShutdownBackendContext(void) {
#ifdef USE_VULKAN
    // Vulkan cleanup if needed
#else
    // Release GL context from this thread
#endif
}

// Platform-specific thread creation

#ifdef _WIN32

/*
================
RB_BackendThreadProc

Windows thread procedure
================
*/
static DWORD WINAPI RB_BackendThreadProc(LPVOID param) {
    RB_BackendThreadLoop();
    return 0;
}

/*
================
R_CreateRenderThread

Create the render thread (Windows)
================
*/
void R_CreateRenderThread(void) {
    DWORD threadId;
    
    Com_Memset(&renderThread, 0, sizeof(renderThread));
    
    renderThread.threadHandle = CreateThread(
        NULL,                   // Security attributes
        0,                      // Stack size (default)
        RB_BackendThreadProc,   // Thread function
        NULL,                   // Thread parameter
        0,                      // Creation flags
        &threadId              // Thread ID
    );
    
    if (!renderThread.threadHandle) {
        ri.Error(ERR_FATAL, "Failed to create render thread");
    }
    
    // Set thread priority slightly above normal for better performance
    SetThreadPriority(renderThread.threadHandle, THREAD_PRIORITY_ABOVE_NORMAL);
    
    ri.Printf(PRINT_ALL, "Render thread created (ID: %lu)\n", threadId);
}

/*
================
R_ShutdownRenderThread

Shutdown the render thread (Windows)
================
*/
void R_ShutdownRenderThread(void) {
    if (!renderThread.threadHandle) {
        return;
    }
    
    // Signal thread to exit
    renderThread.shouldExit = qtrue;
    
    // Wake up the thread so it can exit
    Sys_SignalSemaphore(cmdBufferState.renderSemaphore);
    
    // Wait for thread to finish
    WaitForSingleObject(renderThread.threadHandle, INFINITE);
    
    // Close thread handle
    CloseHandle(renderThread.threadHandle);
    renderThread.threadHandle = NULL;
}

#else // Unix/Linux implementation

/*
================
RB_BackendThreadProc

POSIX thread procedure
================
*/
static void* RB_BackendThreadProc(void* param) {
    RB_BackendThreadLoop();
    return NULL;
}

/*
================
R_CreateRenderThread

Create the render thread (Unix/Linux)
================
*/
void R_CreateRenderThread(void) {
    pthread_t thread;
    pthread_attr_t attr;
    int result;
    
    Com_Memset(&renderThread, 0, sizeof(renderThread));
    
    // Initialize thread attributes
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    
    // Create the thread
    result = pthread_create(&thread, &attr, RB_BackendThreadProc, NULL);
    
    if (result != 0) {
        pthread_attr_destroy(&attr);
        ri.Error(ERR_FATAL, "Failed to create render thread: %d", result);
    }
    
    renderThread.threadHandle = (void*)thread;
    
    // Clean up attributes
    pthread_attr_destroy(&attr);
    
    // Set thread name for debugging
#ifdef __linux__
    pthread_setname_np(thread, "Q3Render");
#endif
    
    ri.Printf(PRINT_ALL, "Render thread created\n");
}

/*
================
R_ShutdownRenderThread

Shutdown the render thread (Unix/Linux)
================
*/
void R_ShutdownRenderThread(void) {
    pthread_t thread;
    
    if (!renderThread.threadHandle) {
        return;
    }
    
    thread = (pthread_t)renderThread.threadHandle;
    
    // Signal thread to exit
    renderThread.shouldExit = qtrue;
    
    // Wake up the thread so it can exit
    Sys_SignalSemaphore(cmdBufferState.renderSemaphore);
    
    // Wait for thread to finish
    pthread_join(thread, NULL);
    
    renderThread.threadHandle = NULL;
}

#endif

/*
================
R_IsRenderThreadRunning

Check if render thread is running
================
*/
qboolean R_IsRenderThreadRunning(void) {
    return renderThread.running;
}

/*
================
R_GetRenderThreadStats

Get render thread performance statistics
================
*/
void R_GetRenderThreadStats(double *avgTime, double *maxTime, double *minTime, int *frameCount) {
    if (avgTime) *avgTime = renderThread.avgFrameTime;
    if (maxTime) *maxTime = renderThread.maxFrameTime;
    if (minTime) *minTime = renderThread.minFrameTime;
    if (frameCount) *frameCount = renderThread.frameCount;
}

/*
================
R_PrintRenderThreadStats

Print render thread statistics
================
*/
void R_PrintRenderThreadStats(void) {
    if (!r_smp->integer) {
        ri.Printf(PRINT_ALL, "Render thread disabled (r_smp = 0)\n");
        return;
    }
    
    ri.Printf(PRINT_ALL, "Render Thread Stats:\n");
    ri.Printf(PRINT_ALL, "  Status: %s\n", renderThread.running ? "Running" : "Stopped");
    ri.Printf(PRINT_ALL, "  Frames rendered: %d\n", renderThread.frameCount);
    ri.Printf(PRINT_ALL, "  Last frame time: %.2f ms\n", renderThread.lastFrameTime);
    ri.Printf(PRINT_ALL, "  Average frame time: %.2f ms\n", renderThread.avgFrameTime);
    ri.Printf(PRINT_ALL, "  Max frame time: %.2f ms\n", renderThread.maxFrameTime);
    ri.Printf(PRINT_ALL, "  Min frame time: %.2f ms\n", renderThread.minFrameTime);
}