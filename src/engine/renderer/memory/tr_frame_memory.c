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
#include "tr_frame_memory.h"

/*
================================================================================
Phase 10: Frame Memory System Implementation

High-performance double-buffered memory allocation for per-frame rendering data.
================================================================================
*/

// Double-buffered frame data
static frameData_t frameData[2];
static int currentFrame;
static qboolean frameMemoryInitialized = qfalse;

/*
================
Frame_Init

Initialize the frame memory system
================
*/
void Frame_Init(void) {
    int i;
    
    if (frameMemoryInitialized) {
        return;
    }
    
    Com_Memset(frameData, 0, sizeof(frameData));
    
    // Allocate memory for both frames
    for (i = 0; i < 2; i++) {
        frameData[i].size = FRAME_MEMORY_SIZE;
        frameData[i].base = ri.Hunk_Alloc(FRAME_MEMORY_SIZE, h_low);
        
        if (!frameData[i].base) {
            ri.Error(ERR_FATAL, "Frame_Init: Failed to allocate frame memory");
        }
        
        // Setup sub-pools
        byte *current = frameData[i].base;
        
        frameData[i].vertexPool.base = current;
        frameData[i].vertexPool.size = VERTEX_POOL_SIZE;
        current += VERTEX_POOL_SIZE;
        
        frameData[i].indexPool.base = current;
        frameData[i].indexPool.size = INDEX_POOL_SIZE;
        current += INDEX_POOL_SIZE;
        
        frameData[i].uniformPool.base = current;
        frameData[i].uniformPool.size = UNIFORM_POOL_SIZE;
        current += UNIFORM_POOL_SIZE;
        
        frameData[i].tempPool.base = current;
        frameData[i].tempPool.size = TEMP_POOL_SIZE;
    }
    
    currentFrame = 0;
    frameMemoryInitialized = qtrue;
    
    ri.Printf(PRINT_ALL, "Frame memory system initialized: %d MB per frame\n", 
              FRAME_MEMORY_SIZE / (1024 * 1024));
}

/*
================
Frame_Shutdown

Shutdown the frame memory system
================
*/
void Frame_Shutdown(void) {
    if (!frameMemoryInitialized) {
        return;
    }
    
    // Report final statistics
    Frame_Statistics();
    
    Com_Memset(frameData, 0, sizeof(frameData));
    frameMemoryInitialized = qfalse;
}

/*
================
Frame_Clear

Clear current frame and swap to next
================
*/
void Frame_Clear(void) {
    frameData_t *fd;
    
    if (!frameMemoryInitialized) {
        return;
    }
    
    // Track peak usage
    fd = &frameData[currentFrame];
    if (fd->used > fd->peakUsage) {
        fd->peakUsage = fd->used;
    }
    
    // Swap frames
    currentFrame = !currentFrame;
    
    // Clear new frame
    fd = &frameData[currentFrame];
    fd->used = 0;
    fd->vertexPool.used = 0;
    fd->indexPool.used = 0;
    fd->uniformPool.used = 0;
    fd->tempPool.used = 0;
    fd->allocCount = 0;
}

/*
================
Frame_Alloc

Allocate memory from the current frame
================
*/
void* Frame_Alloc(size_t size) {
    return Frame_AllocAligned(size, 16);  // Default to 16-byte alignment for SIMD
}

/*
================
Frame_AllocAligned

Allocate aligned memory from the current frame
================
*/
void* Frame_AllocAligned(size_t size, size_t alignment) {
    frameData_t *fd;
    void *ptr;
    size_t alignedSize;
    
    if (!frameMemoryInitialized) {
        Frame_Init();
    }
    
    fd = &frameData[currentFrame];
    alignedSize = ALIGN_SIZE(size, alignment);
    
    // Use temp pool for general allocations
    if (fd->tempPool.used + alignedSize > fd->tempPool.size) {
        ri.Error(ERR_DROP, "Frame_AllocAligned: temp pool overflow (%d bytes requested)",
                 alignedSize);
        return NULL;
    }
    
    ptr = fd->tempPool.base + fd->tempPool.used;
    fd->tempPool.used += alignedSize;
    fd->used += alignedSize;
    fd->allocCount++;
    
    return ptr;
}

/*
================
Frame_AllocVertex

Allocate from vertex pool
================
*/
void* Frame_AllocVertex(size_t size) {
    frameData_t *fd;
    void *ptr;
    size_t alignedSize;
    
    if (!frameMemoryInitialized) {
        Frame_Init();
    }
    
    fd = &frameData[currentFrame];
    alignedSize = ALIGN_SIZE(size, 16);
    
    if (fd->vertexPool.used + alignedSize > fd->vertexPool.size) {
        ri.Error(ERR_DROP, "Frame_AllocVertex: vertex pool overflow (%d bytes requested)",
                 alignedSize);
        return NULL;
    }
    
    ptr = fd->vertexPool.base + fd->vertexPool.used;
    fd->vertexPool.used += alignedSize;
    fd->used += alignedSize;
    fd->allocCount++;
    
    return ptr;
}

/*
================
Frame_AllocIndex

Allocate from index pool
================
*/
void* Frame_AllocIndex(size_t size) {
    frameData_t *fd;
    void *ptr;
    size_t alignedSize;
    
    if (!frameMemoryInitialized) {
        Frame_Init();
    }
    
    fd = &frameData[currentFrame];
    alignedSize = ALIGN_SIZE(size, 4);  // 4-byte alignment for indices
    
    if (fd->indexPool.used + alignedSize > fd->indexPool.size) {
        ri.Error(ERR_DROP, "Frame_AllocIndex: index pool overflow (%d bytes requested)",
                 alignedSize);
        return NULL;
    }
    
    ptr = fd->indexPool.base + fd->indexPool.used;
    fd->indexPool.used += alignedSize;
    fd->used += alignedSize;
    fd->allocCount++;
    
    return ptr;
}

/*
================
Frame_AllocUniform

Allocate from uniform pool
================
*/
void* Frame_AllocUniform(size_t size) {
    frameData_t *fd;
    void *ptr;
    size_t alignedSize;
    
    if (!frameMemoryInitialized) {
        Frame_Init();
    }
    
    fd = &frameData[currentFrame];
    alignedSize = ALIGN_SIZE(size, 256);  // 256-byte alignment for uniform buffers
    
    if (fd->uniformPool.used + alignedSize > fd->uniformPool.size) {
        ri.Error(ERR_DROP, "Frame_AllocUniform: uniform pool overflow (%d bytes requested)",
                 alignedSize);
        return NULL;
    }
    
    ptr = fd->uniformPool.base + fd->uniformPool.used;
    fd->uniformPool.used += alignedSize;
    fd->used += alignedSize;
    fd->allocCount++;
    
    return ptr;
}

/*
================
Frame_AllocTemp

Allocate from temp pool
================
*/
void* Frame_AllocTemp(size_t size) {
    return Frame_AllocAligned(size, 16);
}

/*
================
Frame_GetUsage

Get current frame memory usage
================
*/
size_t Frame_GetUsage(void) {
    if (!frameMemoryInitialized) {
        return 0;
    }
    
    return frameData[currentFrame].used;
}

/*
================
Frame_GetPeakUsage

Get peak frame memory usage
================
*/
size_t Frame_GetPeakUsage(void) {
    size_t peak = 0;
    int i;
    
    if (!frameMemoryInitialized) {
        return 0;
    }
    
    for (i = 0; i < 2; i++) {
        if (frameData[i].peakUsage > peak) {
            peak = frameData[i].peakUsage;
        }
    }
    
    return peak;
}

/*
================
Frame_Statistics

Print frame memory statistics
================
*/
void Frame_Statistics(void) {
    frameData_t *fd;
    
    if (!frameMemoryInitialized || !r_speeds->integer) {
        return;
    }
    
    fd = &frameData[currentFrame];
    
    ri.Printf(PRINT_ALL, "Frame Memory Stats (Frame %d):\n", currentFrame);
    ri.Printf(PRINT_ALL, "  Total: %d KB / %d KB (%.1f%%)\n",
              fd->used / 1024, fd->size / 1024,
              (float)fd->used * 100.0f / fd->size);
    ri.Printf(PRINT_ALL, "  Vertex Pool: %d KB / %d KB\n",
              fd->vertexPool.used / 1024, fd->vertexPool.size / 1024);
    ri.Printf(PRINT_ALL, "  Index Pool: %d KB / %d KB\n",
              fd->indexPool.used / 1024, fd->indexPool.size / 1024);
    ri.Printf(PRINT_ALL, "  Uniform Pool: %d KB / %d KB\n",
              fd->uniformPool.used / 1024, fd->uniformPool.size / 1024);
    ri.Printf(PRINT_ALL, "  Temp Pool: %d KB / %d KB\n",
              fd->tempPool.used / 1024, fd->tempPool.size / 1024);
    ri.Printf(PRINT_ALL, "  Allocations: %d\n", fd->allocCount);
    ri.Printf(PRINT_ALL, "  Peak Usage: %d KB\n", fd->peakUsage / 1024);
}