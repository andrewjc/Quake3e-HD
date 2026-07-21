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
// tr_memory.c - Memory Tracking System Implementation

#include "tr_memory.h"
#include "../tr_local.h"

memoryState_t gpuMemoryState;

static memAllocation_t* R_GetFreeAllocation( void );
static void R_ReturnAllocation( memAllocation_t *alloc );

qboolean R_InitMemoryTracking( VkPhysicalDevice physicalDevice, VkDevice device ) {
    ri.Printf( PRINT_ALL, "Initializing memory tracking system...\n" );
    
    Com_Memset( &gpuMemoryState, 0, sizeof( gpuMemoryState ) );
    
    vkGetPhysicalDeviceMemoryProperties( physicalDevice, &gpuMemoryState.memoryProperties );
    
    gpuMemoryState.heapCount = gpuMemoryState.memoryProperties.memoryHeapCount;
    for ( uint32_t i = 0; i < gpuMemoryState.heapCount; i++ ) {
        memHeap_t *heap = &gpuMemoryState.heaps[i];
        heap->index = i;
        heap->heap = gpuMemoryState.memoryProperties.memoryHeaps[i];
        heap->available = heap->heap.size;
        heap->maxAllocation = heap->heap.size / 4;
        
        for ( uint32_t j = 0; j < gpuMemoryState.memoryProperties.memoryTypeCount; j++ ) {
            if ( gpuMemoryState.memoryProperties.memoryTypes[j].heapIndex == i ) {
                VkMemoryPropertyFlags flags = gpuMemoryState.memoryProperties.memoryTypes[j].propertyFlags;
                heap->deviceLocal = ( flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ) != 0;
                heap->hostVisible = ( flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) != 0;
                break;
            }
        }
    }
    
    gpuMemoryState.enableDefragmentation = qtrue;
    gpuMemoryState.defragThreshold = 0.3f;
    gpuMemoryState.initialized = qtrue;
    
    ri.Printf( PRINT_ALL, "Memory tracking initialized with %d heaps\n", gpuMemoryState.heapCount );
    return qtrue;
}

void R_ShutdownMemoryTracking( void ) {
    if ( !gpuMemoryState.initialized ) {
        return;
    }
    
    R_CheckMemoryLeaks();
    
    for ( uint32_t i = 0; i < gpuMemoryState.poolCount; i++ ) {
        R_DestroyMemoryPool( &gpuMemoryState.pools[i] );
    }
    
    Com_Memset( &gpuMemoryState, 0, sizeof( gpuMemoryState ) );
    ri.Printf( PRINT_ALL, "Memory tracking system shutdown\n" );
}

memAllocation_t* R_AllocateGPUMemory( VkDeviceSize size, VkMemoryRequirements requirements, 
                                      memAllocationType_t type, const char *name ) {
    if ( !gpuMemoryState.initialized ) {
        return NULL;
    }
    
    uint32_t heapIndex = R_FindMemoryHeap( requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
    if ( heapIndex == UINT32_MAX ) {
        return NULL;
    }
    
    memAllocation_t *alloc = R_GetFreeAllocation();
    if ( !alloc ) {
        return NULL;
    }
    
    alloc->size = size;
    alloc->type = type;
    alloc->heapIndex = heapIndex;
    alloc->frameAllocated = tr.frameCount;
    Q_strncpyz( alloc->name, name ? name : "unnamed", MEM_POOL_NAME_LENGTH );
    
    memHeap_t *heap = &gpuMemoryState.heaps[heapIndex];
    heap->used += size;
    heap->allocated += requirements.size;
    heap->allocationCount++;
    heap->utilizationRatio = (float)heap->used / (float)heap->heap.size;
    
    gpuMemoryState.stats.totalAllocated += requirements.size;
    gpuMemoryState.stats.totalUsed += size;
    gpuMemoryState.stats.allocationCount++;
    gpuMemoryState.stats.typeUsage[type] += size;
    
    if ( gpuMemoryState.stats.totalAllocated > gpuMemoryState.stats.peakAllocated ) {
        gpuMemoryState.stats.peakAllocated = gpuMemoryState.stats.totalAllocated;
    }
    
    return alloc;
}

void R_FreeGPUMemory( memAllocation_t *allocation ) {
    if ( !allocation ) {
        return;
    }
    
    memHeap_t *heap = &gpuMemoryState.heaps[allocation->heapIndex];
    heap->used -= allocation->size;
    heap->allocationCount--;
    heap->utilizationRatio = (float)heap->used / (float)heap->heap.size;
    
    gpuMemoryState.stats.totalUsed -= allocation->size;
    gpuMemoryState.stats.deallocationCount++;
    gpuMemoryState.stats.typeUsage[allocation->type] -= allocation->size;
    
    R_ReturnAllocation( allocation );
}

uint32_t R_FindMemoryHeap( VkMemoryRequirements requirements, VkMemoryPropertyFlags properties ) {
    for ( uint32_t i = 0; i < gpuMemoryState.memoryProperties.memoryTypeCount; i++ ) {
        if ( ( requirements.memoryTypeBits & ( 1 << i ) ) && 
             ( gpuMemoryState.memoryProperties.memoryTypes[i].propertyFlags & properties ) == properties ) {
            return gpuMemoryState.memoryProperties.memoryTypes[i].heapIndex;
        }
    }
    return UINT32_MAX;
}

static void R_UpdateGPUMemoryStats( void ) {
    if ( !gpuMemoryState.initialized ) {
        return;
    }
    
    VkDeviceSize totalWasted = 0;
    VkDeviceSize totalFragmented = 0;
    
    for ( uint32_t i = 0; i < gpuMemoryState.poolCount; i++ ) {
        memPool_t *pool = &gpuMemoryState.pools[i];
        totalWasted += pool->wasted;
        
        if ( pool->size > 0 ) {
            VkDeviceSize fragmented = pool->size - pool->used - pool->wasted;
            totalFragmented += fragmented;
            pool->fragmentationRatio = (float)fragmented / (float)pool->size;
        }
    }
    
    gpuMemoryState.stats.totalWasted = totalWasted;
    
    if ( gpuMemoryState.stats.totalAllocated > 0 ) {
        gpuMemoryState.stats.fragmentationRatio = 
            (float)totalFragmented / (float)gpuMemoryState.stats.totalAllocated;
    }
}

void R_PrintMemoryStats( void ) {
    if ( !gpuMemoryState.initialized ) {
        return;
    }
    
    ri.Printf( PRINT_ALL, "===== GPU Memory Statistics =====\n" );
    ri.Printf( PRINT_ALL, "Total Allocated: %.2f MB\n", 
              gpuMemoryState.stats.totalAllocated / ( 1024.0f * 1024.0f ) );
    ri.Printf( PRINT_ALL, "Total Used: %.2f MB\n", 
              gpuMemoryState.stats.totalUsed / ( 1024.0f * 1024.0f ) );
    ri.Printf( PRINT_ALL, "Peak Allocated: %.2f MB\n", 
              gpuMemoryState.stats.peakAllocated / ( 1024.0f * 1024.0f ) );
    ri.Printf( PRINT_ALL, "Fragmentation: %.1f%%\n", 
              gpuMemoryState.stats.fragmentationRatio * 100.0f );
    
    ri.Printf( PRINT_ALL, "\nHeap Usage:\n" );
    for ( uint32_t i = 0; i < gpuMemoryState.heapCount; i++ ) {
        memHeap_t *heap = &gpuMemoryState.heaps[i];
        ri.Printf( PRINT_ALL, "  Heap %d: %.2f / %.2f MB (%.1f%%)\n", i,
                  heap->used / ( 1024.0f * 1024.0f ),
                  heap->heap.size / ( 1024.0f * 1024.0f ),
                  heap->utilizationRatio * 100.0f );
    }
    
    ri.Printf( PRINT_ALL, "\nType Usage:\n" );
    const char *typeNames[] = {
        "Vertex Buffer", "Index Buffer", "Uniform Buffer", "Storage Buffer",
        "Texture 2D", "Texture 3D", "Texture Cube", "Render Target",
        "Depth Buffer", "Staging", "Dynamic"
    };
    
    for ( int i = 0; i < MEM_TYPE_CUSTOM; i++ ) {
        if ( gpuMemoryState.stats.typeUsage[i] > 0 ) {
            ri.Printf( PRINT_ALL, "  %s: %.2f MB\n", typeNames[i],
                      gpuMemoryState.stats.typeUsage[i] / ( 1024.0f * 1024.0f ) );
        }
    }
}

static memAllocation_t* R_GetFreeAllocation( void ) {
    if ( gpuMemoryState.allocationCount >= MEM_MAX_ALLOCATIONS ) {
        return NULL;
    }
    
    for ( uint32_t i = 0; i < MEM_MAX_ALLOCATIONS; i++ ) {
        if ( !gpuMemoryState.allocationPool[i].ptr ) {
            gpuMemoryState.allocationCount++;
            return &gpuMemoryState.allocationPool[i];
        }
    }
    
    return NULL;
}

static void R_ReturnAllocation( memAllocation_t *alloc ) {
    Com_Memset( alloc, 0, sizeof( memAllocation_t ) );
    gpuMemoryState.allocationCount--;
}

void R_CheckMemoryLeaks( void ) {
    uint32_t leakCount = 0;
    VkDeviceSize leakSize = 0;
    
    for ( uint32_t i = 0; i < MEM_MAX_ALLOCATIONS; i++ ) {
        if ( gpuMemoryState.allocationPool[i].ptr ) {
            leakCount++;
            leakSize += gpuMemoryState.allocationPool[i].size;
            ri.Printf( PRINT_WARNING, "Memory leak: %s (%.2f KB)\n",
                      gpuMemoryState.allocationPool[i].name,
                      gpuMemoryState.allocationPool[i].size / 1024.0f );
        }
    }
    
    if ( leakCount > 0 ) {
        ri.Printf( PRINT_WARNING, "Total leaks: %d allocations, %.2f MB\n",
                  leakCount, leakSize / ( 1024.0f * 1024.0f ) );
    }
}