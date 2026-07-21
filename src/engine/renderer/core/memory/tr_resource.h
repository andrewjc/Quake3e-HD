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
// tr_resource.h - GPU resource management for multi-threaded rendering

#ifndef TR_RESOURCE_H
#define TR_RESOURCE_H

#include "../tr_local.h"

#define MAX_VERTEX_BUFFERS   1024
#define MAX_INDEX_BUFFERS    1024
#define MAX_UNIFORM_BUFFERS  256
#define MAX_TEXTURE_RESOURCES 2048

// Resource types
typedef enum {
    RESOURCE_VERTEX_BUFFER,
    RESOURCE_INDEX_BUFFER,
    RESOURCE_UNIFORM_BUFFER,
    RESOURCE_TEXTURE,
    RESOURCE_RENDER_TARGET,
    RESOURCE_DEPTH_BUFFER,
    RESOURCE_COMMAND_BUFFER,
    RESOURCE_DESCRIPTOR_SET
} gpuResourceType_t;

// Resource usage flags
typedef enum {
    RESOURCE_USAGE_STATIC   = 0x0001,  // Immutable after creation
    RESOURCE_USAGE_DYNAMIC  = 0x0002,  // Updated frequently
    RESOURCE_USAGE_STREAM   = 0x0004,  // Updated every frame
    RESOURCE_USAGE_STAGING  = 0x0008,  // CPU-GPU transfer
    RESOURCE_USAGE_SHARED   = 0x0010,  // Shared between threads
    RESOURCE_USAGE_PERSISTENT = 0x0020  // Persistently mapped
} gpuResourceUsage_t;

// GPU resource structure
typedef struct gpuResource_s {
    gpuResourceType_t   type;
    gpuResourceUsage_t  usage;
    
    // Vulkan handles
#ifdef USE_VULKAN
    union {
        VkBuffer        buffer;
        VkImage         image;
        VkImageView     imageView;
        VkSampler       sampler;
        VkCommandBuffer commandBuffer;
        VkDescriptorSet descriptorSet;
    } vk;
    
    VkDeviceMemory      memory;
    VkDeviceSize        offset;
#endif
    
    // Common properties
    size_t              size;
    void*               mappedPtr;      // For persistent mapping
    
    // Reference counting for thread safety
    volatile int        refCount;
    
    // Synchronization state
    qboolean            dirty;          // Needs GPU update
    int                 lastFrameUsed;  // For garbage collection
    
    // Linked list for pool management
    struct gpuResource_s* next;
    struct gpuResource_s* prev;
} gpuResource_t;

// Buffer resource specific
typedef struct bufferResource_s {
    gpuResource_t       base;
    
    // Buffer properties
    int                 stride;         // Vertex stride
    int                 count;          // Element count
    
    // Double buffering for dynamic resources
    int                 currentBuffer;  // 0 or 1
    gpuResource_t*      buffers[2];
} bufferResource_t;

// Texture resource specific
typedef struct textureResource_s {
    gpuResource_t       base;
    
    // Texture properties
    int                 width, height, depth;
    int                 mipLevels;
    int                 layerCount;
    VkFormat            format;
    VkImageLayout       currentLayout;
    
    // Views for different mip levels
    VkImageView*        mipViews;
} textureResource_t;

// Resource pool for efficient allocation
typedef struct resourcePool_s {
    // Buffers
    bufferResource_t*   vertexBuffers[MAX_VERTEX_BUFFERS];
    bufferResource_t*   indexBuffers[MAX_INDEX_BUFFERS];
    bufferResource_t*   uniformBuffers[MAX_UNIFORM_BUFFERS];
    int                 numVertexBuffers;
    int                 numIndexBuffers;
    int                 numUniformBuffers;
    
    // Textures
    textureResource_t*  textures[MAX_TEXTURE_RESOURCES];
    int                 numTextures;
    
    // Double-buffered dynamic resources for each frame
    struct {
        gpuResource_t*  dynamicVB;
        gpuResource_t*  dynamicIB;
        gpuResource_t*  dynamicUB;
        size_t          vbUsed;
        size_t          ibUsed;
        size_t          ubUsed;
    } frame[2];
    int                 currentFrame;
    
    // Memory allocators
#ifdef USE_VULKAN
    VkDeviceMemory      deviceLocalMemory;
    VkDeviceMemory      hostVisibleMemory;
    VkDeviceMemory      hostCachedMemory;
    VkDeviceSize        deviceLocalOffset;
    VkDeviceSize        hostVisibleOffset;
    VkDeviceSize        hostCachedOffset;
#endif
    
    // Synchronization
    void*               resourceMutex;
    
    // Statistics
    size_t              totalAllocated;
    size_t              totalUsed;
    int                 allocationCount;
} resourcePool_t;

// Frame-local allocation for temporary data
typedef struct frameAlloc_s {
    byte*               base;
    size_t              size;
    size_t              used;
    
    // Sub-allocators
    struct {
        void*           base;
        size_t          used;
        size_t          size;
    } vertexPool, indexPool, uniformPool, tempPool;
} frameAlloc_t;

// Global resource pool
extern resourcePool_t resourcePool;

// Resource management functions
void R_InitResourcePool(void);
void R_ShutdownResourcePool(void);
void R_BeginFrameResources(void);
void R_EndFrameResources(void);

// Buffer creation
gpuResource_t* R_CreateVertexBuffer(size_t size, gpuResourceUsage_t usage, const void* data);
gpuResource_t* R_CreateIndexBuffer(size_t size, gpuResourceUsage_t usage, const void* data);
gpuResource_t* R_CreateUniformBuffer(size_t size, gpuResourceUsage_t usage, const void* data);

// Texture creation
gpuResource_t* R_CreateTexture2D(int width, int height, VkFormat format, gpuResourceUsage_t usage);
gpuResource_t* R_CreateTextureCube(int size, VkFormat format, gpuResourceUsage_t usage);
gpuResource_t* R_CreateTextureArray(int width, int height, int layers, VkFormat format, gpuResourceUsage_t usage);

// Resource updates
void R_UpdateBuffer(gpuResource_t* resource, size_t offset, size_t size, const void* data);
void R_UpdateTexture(gpuResource_t* resource, int level, int layer, const void* data);
void* R_MapBuffer(gpuResource_t* resource, size_t offset, size_t size);
void R_UnmapBuffer(gpuResource_t* resource);

// Reference counting
void R_AddRefResource(gpuResource_t* resource);
void R_ReleaseResource(gpuResource_t* resource);

// Dynamic allocation
void* R_AllocVertexBuffer(size_t size);
void* R_AllocIndexBuffer(size_t size);
void* R_AllocUniformBuffer(size_t size);

// Frame allocation (temporary, freed at end of frame)
void* Frame_Alloc(size_t size);
void* Frame_AllocVertices(size_t size);
void* Frame_AllocIndices(size_t size);
void* Frame_AllocUniforms(size_t size);
void Frame_Clear(void);

// Cache line alignment for SIMD
#define CACHE_LINE_SIZE 64

typedef struct renderConstants_s {
    mat4_t          mvpMatrix;
    vec4_t          color;
    vec4_t          fogColor;
    vec2_t          fogParams;
    float           time;
    float           alphaTest;
} renderConstants_t;

// SIMD batch processing helpers
void R_ProcessVerticesSIMD(const vec4_t* input, vec4_t* output, int count, const mat4_t transform);
void R_CullSurfacesSIMD(drawSurf_t* surfs, int count, const vec4_t planes[6]);

// Statistics
void R_PrintResourceStats(void);
size_t R_GetResourceMemoryUsed(void);
int R_GetResourceCount(gpuResourceType_t type);

#endif // TR_RESOURCE_H