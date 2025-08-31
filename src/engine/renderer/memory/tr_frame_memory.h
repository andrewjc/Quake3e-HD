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

#ifndef TR_FRAME_MEMORY_H
#define TR_FRAME_MEMORY_H

#include "../tr_local.h"

/*
================================================================================
Phase 10: Frame Memory System

High-performance memory allocation for per-frame data with double buffering.
================================================================================
*/

#define FRAME_MEMORY_SIZE       (32 * 1024 * 1024)    // 32MB per frame
#define VERTEX_POOL_SIZE        (8 * 1024 * 1024)     // 8MB for vertices
#define INDEX_POOL_SIZE         (4 * 1024 * 1024)     // 4MB for indices
#define UNIFORM_POOL_SIZE       (2 * 1024 * 1024)     // 2MB for uniforms
#define TEMP_POOL_SIZE          (4 * 1024 * 1024)     // 4MB for temp data

// Alignment macro
#define ALIGN_SIZE(size, alignment) (((size) + (alignment) - 1) & ~((alignment) - 1))

// Frame data structure
typedef struct frameData_s {
    byte        *base;
    size_t      size;
    size_t      used;
    
    // Sub-allocators
    struct {
        byte    *base;
        size_t  used;
        size_t  size;
    } vertexPool, indexPool, uniformPool, tempPool;
    
    // Statistics
    int         allocCount;
    int         peakUsage;
} frameData_t;

// Function declarations
void        Frame_Init(void);
void        Frame_Shutdown(void);
void        Frame_Clear(void);
void*       Frame_Alloc(size_t size);
void*       Frame_AllocAligned(size_t size, size_t alignment);

// Pool allocators
void*       Frame_AllocVertex(size_t size);
void*       Frame_AllocIndex(size_t size);
void*       Frame_AllocUniform(size_t size);
void*       Frame_AllocTemp(size_t size);

// Statistics
void        Frame_Statistics(void);
size_t      Frame_GetUsage(void);
size_t      Frame_GetPeakUsage(void);

// Macros for common allocations
#define FRAME_ALLOC_VERTS(count, type)     ((type*)Frame_AllocVertex((count) * sizeof(type)))
#define FRAME_ALLOC_INDICES(count)         ((int*)Frame_AllocIndex((count) * sizeof(int)))
#define FRAME_ALLOC_UNIFORMS(count, type)  ((type*)Frame_AllocUniform((count) * sizeof(type)))

#endif // TR_FRAME_MEMORY_H