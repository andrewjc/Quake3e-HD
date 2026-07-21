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
// tr_memory.h - Memory Tracking System

#ifndef __TR_MEMORY_H
#define __TR_MEMORY_H

#include "../tr_local.h"
#include "vulkan/vulkan.h"

#define MEM_MAX_POOLS           32
#define MEM_MAX_ALLOCATIONS    4096
#define MEM_MAX_HEAPS          16
#define MEM_POOL_NAME_LENGTH   64

typedef enum {
    MEM_TYPE_VERTEX_BUFFER = 0,
    MEM_TYPE_INDEX_BUFFER,
    MEM_TYPE_UNIFORM_BUFFER,
    MEM_TYPE_STORAGE_BUFFER,
    MEM_TYPE_TEXTURE_2D,
    MEM_TYPE_TEXTURE_3D,
    MEM_TYPE_TEXTURE_CUBE,
    MEM_TYPE_RENDER_TARGET,
    MEM_TYPE_DEPTH_BUFFER,
    MEM_TYPE_STAGING,
    MEM_TYPE_DYNAMIC,
    MEM_TYPE_CUSTOM,
} memAllocationType_t;

typedef struct memAllocation_s {
    void                   *ptr;
    VkDeviceMemory          memory;
    VkDeviceSize            size;
    VkDeviceSize            offset;
    memAllocationType_t     type;
    uint32_t                heapIndex;
    uint32_t                poolIndex;
    char                    name[MEM_POOL_NAME_LENGTH];
    uint64_t                frameAllocated;
    uint64_t                lastAccessed;
    qboolean                mapped;
    qboolean                dedicated;
    struct memAllocation_s *next;
    struct memAllocation_s *prev;
} memAllocation_t;

typedef struct memPool_s {
    char                    name[MEM_POOL_NAME_LENGTH];
    VkDeviceMemory          memory;
    VkDeviceSize            size;
    VkDeviceSize            used;
    VkDeviceSize            wasted;
    uint32_t                heapIndex;
    uint32_t                allocationCount;
    memAllocation_t        *allocations;
    qboolean                canGrow;
    float                   fragmentationRatio;
} memPool_t;

typedef struct memHeap_s {
    uint32_t                index;
    VkMemoryHeap            heap;
    VkDeviceSize            used;
    VkDeviceSize            allocated;
    VkDeviceSize            available;
    VkDeviceSize            maxAllocation;
    uint32_t                allocationCount;
    qboolean                deviceLocal;
    qboolean                hostVisible;
    float                   utilizationRatio;
} memHeap_t;

typedef struct memStats_s {
    VkDeviceSize            totalAllocated;
    VkDeviceSize            totalUsed;
    VkDeviceSize            totalWasted;
    VkDeviceSize            peakAllocated;
    VkDeviceSize            peakUsed;
    uint32_t                allocationCount;
    uint32_t                deallocationCount;
    uint32_t                defragmentationCount;
    float                   fragmentationRatio;
    VkDeviceSize            typeUsage[MEM_TYPE_CUSTOM];
} memStats_t;

typedef struct memoryState_s {
    qboolean                initialized;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    memHeap_t               heaps[MEM_MAX_HEAPS];
    uint32_t                heapCount;
    memPool_t               pools[MEM_MAX_POOLS];
    uint32_t                poolCount;
    memAllocation_t        *allocations;
    memAllocation_t         allocationPool[MEM_MAX_ALLOCATIONS];
    uint32_t                allocationCount;
    memStats_t              stats;
    qboolean                enableDefragmentation;
    float                   defragThreshold;
    uint32_t                framesSinceDefrag;
} memoryState_t;

extern memoryState_t gpuMemoryState;

// Initialization
qboolean R_InitMemoryTracking( VkPhysicalDevice physicalDevice, VkDevice device );
void R_ShutdownMemoryTracking( void );

// Allocation management
memAllocation_t* R_AllocateGPUMemory( VkDeviceSize size, VkMemoryRequirements requirements, 
                                      memAllocationType_t type, const char *name );
void R_FreeGPUMemory( memAllocation_t *allocation );
void* R_MapGPUMemory( memAllocation_t *allocation );
void R_UnmapGPUMemory( memAllocation_t *allocation );

// Pool management
memPool_t* R_CreateMemoryPool( const char *name, VkDeviceSize size, uint32_t heapIndex );
void R_DestroyMemoryPool( memPool_t *pool );
memAllocation_t* R_AllocateFromPool( memPool_t *pool, VkDeviceSize size, VkDeviceSize alignment );
void R_ResetMemoryPool( memPool_t *pool );

// Heap management
uint32_t R_FindMemoryHeap( VkMemoryRequirements requirements, VkMemoryPropertyFlags properties );
VkDeviceSize R_GetHeapAvailable( uint32_t heapIndex );
float R_GetHeapUtilization( uint32_t heapIndex );

// Defragmentation
void R_DefragmentMemory( void );
qboolean R_NeedsDefragmentation( void );
void R_CompactMemoryPool( memPool_t *pool );

// Statistics
void R_UpdateMemoryStats( void );
void R_PrintMemoryStats( void );
memStats_t* R_GetMemoryStats( void );
VkDeviceSize R_GetMemoryUsage( memAllocationType_t type );

// Debugging
void R_ValidateMemory( void );
void R_DumpMemoryAllocations( const char *filename );
void R_CheckMemoryLeaks( void );

#endif // __TR_MEMORY_H