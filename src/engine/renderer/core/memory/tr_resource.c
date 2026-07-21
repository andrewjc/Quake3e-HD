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
// tr_resource.c - GPU resource management implementation

#include "../tr_local.h"
#include "tr_resource.h"

// Global resource pool
resourcePool_t resourcePool;

// Frame allocators (double-buffered)
static frameAlloc_t frameAlloc[2];
static int currentFrameAlloc;

// External sync functions
extern void* Sys_CreateMutex(void);
extern void Sys_DestroyMutex(void* mutex);
extern void Sys_LockMutex(void* mutex);
extern void Sys_UnlockMutex(void* mutex);
extern int Sys_AtomicAdd(volatile int* value, int add);

// CVars for resource management
static cvar_t* r_dynamicBufferSize;
static cvar_t* r_frameAllocSize;
static cvar_t* r_resourceDebug;

/*
================
R_InitResourcePool

Initialize the resource management system
================
*/
void R_InitResourcePool(void) {
    int i;

    // Idempotent init: if a previous pool is still live (init called without a
    // paired shutdown — which happens on this build's per-map renderer
    // restart), release it first. Memset'ing over the handles below would
    // otherwise orphan the per-frame dynamic vertex/index/uniform buffers and
    // their allocations (VUID-vkDestroyDevice-device-05137).
    if (resourcePool.resourceMutex) {
        R_ShutdownResourcePool();
    }

    Com_Memset(&resourcePool, 0, sizeof(resourcePool));

    // Register CVars
    r_dynamicBufferSize = ri.Cvar_Get("r_dynamicBufferSize", "16", CVAR_ARCHIVE | CVAR_LATCH);
    r_frameAllocSize = ri.Cvar_Get("r_frameAllocSize", "32", CVAR_ARCHIVE | CVAR_LATCH);
    r_resourceDebug = ri.Cvar_Get("r_resourceDebug", "0", CVAR_CHEAT);
    
    // Create synchronization mutex
    resourcePool.resourceMutex = Sys_CreateMutex();
    if (!resourcePool.resourceMutex) {
        ri.Error(ERR_FATAL, "Failed to create resource mutex");
    }
    
    // Allocate dynamic buffers for each frame
    size_t dynamicSize = r_dynamicBufferSize->integer * 1024 * 1024;
    
    for (i = 0; i < 2; i++) {
#ifdef USE_VULKAN
        // Create dynamic vertex buffer
        resourcePool.frame[i].dynamicVB = R_CreateVertexBuffer(
            dynamicSize / 3, 
            RESOURCE_USAGE_DYNAMIC | RESOURCE_USAGE_PERSISTENT,
            NULL
        );
        
        // Create dynamic index buffer
        resourcePool.frame[i].dynamicIB = R_CreateIndexBuffer(
            dynamicSize / 3,
            RESOURCE_USAGE_DYNAMIC | RESOURCE_USAGE_PERSISTENT,
            NULL
        );
        
        // Create dynamic uniform buffer
        resourcePool.frame[i].dynamicUB = R_CreateUniformBuffer(
            dynamicSize / 3,
            RESOURCE_USAGE_DYNAMIC | RESOURCE_USAGE_PERSISTENT,
            NULL
        );
#endif
        
        resourcePool.frame[i].vbUsed = 0;
        resourcePool.frame[i].ibUsed = 0;
        resourcePool.frame[i].ubUsed = 0;
    }
    
    // Initialize frame allocators
    size_t frameSize = r_frameAllocSize->integer * 1024 * 1024;
    
    for (i = 0; i < 2; i++) {
        frameAlloc[i].size = frameSize;
        frameAlloc[i].base = (byte*)ri.Hunk_Alloc(frameSize, h_low);
        frameAlloc[i].used = 0;
        
        // Setup sub-allocators
        size_t quarterSize = frameSize / 4;
        
        frameAlloc[i].vertexPool.base = frameAlloc[i].base;
        frameAlloc[i].vertexPool.size = quarterSize;
        frameAlloc[i].vertexPool.used = 0;
        
        frameAlloc[i].indexPool.base = frameAlloc[i].base + quarterSize;
        frameAlloc[i].indexPool.size = quarterSize;
        frameAlloc[i].indexPool.used = 0;
        
        frameAlloc[i].uniformPool.base = frameAlloc[i].base + quarterSize * 2;
        frameAlloc[i].uniformPool.size = quarterSize;
        frameAlloc[i].uniformPool.used = 0;
        
        frameAlloc[i].tempPool.base = frameAlloc[i].base + quarterSize * 3;
        frameAlloc[i].tempPool.size = quarterSize;
        frameAlloc[i].tempPool.used = 0;
    }
    
    currentFrameAlloc = 0;
    resourcePool.currentFrame = 0;
    
    ri.Printf(PRINT_ALL, "Resource pool initialized:\n");
    ri.Printf(PRINT_ALL, "  Dynamic buffer size: %d MB per frame\n", r_dynamicBufferSize->integer);
    ri.Printf(PRINT_ALL, "  Frame allocator size: %d MB per frame\n", r_frameAllocSize->integer);
}

/*
================
R_ShutdownResourcePool

Cleanup resource management system
================
*/
void R_ShutdownResourcePool(void) {
    int i;
    
    // Release all dynamic buffers
    for (i = 0; i < 2; i++) {
        if (resourcePool.frame[i].dynamicVB) {
            R_ReleaseResource(resourcePool.frame[i].dynamicVB);
        }
        if (resourcePool.frame[i].dynamicIB) {
            R_ReleaseResource(resourcePool.frame[i].dynamicIB);
        }
        if (resourcePool.frame[i].dynamicUB) {
            R_ReleaseResource(resourcePool.frame[i].dynamicUB);
        }
    }
    
    // Release all pooled resources
    for (i = 0; i < resourcePool.numVertexBuffers; i++) {
        if (resourcePool.vertexBuffers[i]) {
            R_ReleaseResource(&resourcePool.vertexBuffers[i]->base);
        }
    }
    
    for (i = 0; i < resourcePool.numIndexBuffers; i++) {
        if (resourcePool.indexBuffers[i]) {
            R_ReleaseResource(&resourcePool.indexBuffers[i]->base);
        }
    }
    
    for (i = 0; i < resourcePool.numUniformBuffers; i++) {
        if (resourcePool.uniformBuffers[i]) {
            R_ReleaseResource(&resourcePool.uniformBuffers[i]->base);
        }
    }
    
    for (i = 0; i < resourcePool.numTextures; i++) {
        if (resourcePool.textures[i]) {
            R_ReleaseResource(&resourcePool.textures[i]->base);
        }
    }
    
    // Destroy mutex
    if (resourcePool.resourceMutex) {
        Sys_DestroyMutex(resourcePool.resourceMutex);
        resourcePool.resourceMutex = NULL;
    }
    
    Com_Memset(&resourcePool, 0, sizeof(resourcePool));
}

/*
================
R_BeginFrameResources

Prepare resources for a new frame
================
*/
void R_BeginFrameResources(void) {
    // Switch to next frame's resources
    resourcePool.currentFrame = (resourcePool.currentFrame + 1) & 1;
    currentFrameAlloc = (currentFrameAlloc + 1) & 1;
    
    // Reset dynamic buffer usage
    resourcePool.frame[resourcePool.currentFrame].vbUsed = 0;
    resourcePool.frame[resourcePool.currentFrame].ibUsed = 0;
    resourcePool.frame[resourcePool.currentFrame].ubUsed = 0;
    
    // Reset frame allocator
    frameAlloc_t* fa = &frameAlloc[currentFrameAlloc];
    fa->used = 0;
    fa->vertexPool.used = 0;
    fa->indexPool.used = 0;
    fa->uniformPool.used = 0;
    fa->tempPool.used = 0;
}

/*
================
R_EndFrameResources

Finalize resources for current frame
================
*/
void R_EndFrameResources(void) {
    // Flush any pending resource updates
#ifdef USE_VULKAN
    // Vulkan memory barriers will be handled by command buffer
#endif
    
    if (r_resourceDebug->integer) {
        static int frameCount = 0;
        if (++frameCount % 100 == 0) {
            R_PrintResourceStats();
        }
    }
}

/*
================
R_CreateVertexBuffer

Create a vertex buffer resource
================
*/
gpuResource_t* R_CreateVertexBuffer(size_t size, gpuResourceUsage_t usage, const void* data) {
    bufferResource_t* buffer;
    
    Sys_LockMutex(resourcePool.resourceMutex);
    
    // Check if we have room in the pool
    if (resourcePool.numVertexBuffers >= MAX_VERTEX_BUFFERS) {
        Sys_UnlockMutex(resourcePool.resourceMutex);
        ri.Printf(PRINT_WARNING, "R_CreateVertexBuffer: pool full\n");
        return NULL;
    }
    
    // Allocate resource structure
    buffer = (bufferResource_t*)Z_Malloc(sizeof(bufferResource_t));
    Com_Memset(buffer, 0, sizeof(bufferResource_t));
    
    buffer->base.type = RESOURCE_VERTEX_BUFFER;
    buffer->base.usage = usage;
    buffer->base.size = size;
    buffer->base.refCount = 1;
    buffer->base.lastFrameUsed = tr.frameCount;
    
#ifdef USE_VULKAN
    // Create Vulkan buffer
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    if (usage & RESOURCE_USAGE_DYNAMIC) {
        bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    
    VkResult result = vkCreateBuffer(vk.device, &bufferInfo, NULL, &buffer->base.vk.buffer);
    if (result != VK_SUCCESS) {
        Z_Free(buffer);
        Sys_UnlockMutex(resourcePool.resourceMutex);
        ri.Printf(PRINT_WARNING, "R_CreateVertexBuffer: vkCreateBuffer failed\n");
        return NULL;
    }
    
    // Allocate memory
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, buffer->base.vk.buffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            (usage & RESOURCE_USAGE_DYNAMIC) ? 
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    result = vkAllocateMemory(vk.device, &allocInfo, NULL, &buffer->base.memory);
    if (result != VK_SUCCESS) {
        vkDestroyBuffer(vk.device, buffer->base.vk.buffer, NULL);
        Z_Free(buffer);
        Sys_UnlockMutex(resourcePool.resourceMutex);
        ri.Printf(PRINT_WARNING, "R_CreateVertexBuffer: vkAllocateMemory failed\n");
        return NULL;
    }
    
    vkBindBufferMemory(vk.device, buffer->base.vk.buffer, buffer->base.memory, 0);
    
    // Map if persistent
    if (usage & RESOURCE_USAGE_PERSISTENT) {
        vkMapMemory(vk.device, buffer->base.memory, 0, size, 0, &buffer->base.mappedPtr);
    }
    
    // Upload initial data if provided
    if (data) {
        if (buffer->base.mappedPtr) {
            Com_Memcpy(buffer->base.mappedPtr, data, size);
        } else {
            // Use staging buffer for upload
            vk_upload_buffer_data(buffer->base.vk.buffer, 0, size, data);
        }
    }
#endif
    
    // Add to pool
    resourcePool.vertexBuffers[resourcePool.numVertexBuffers++] = buffer;
    resourcePool.totalAllocated += size;
    resourcePool.allocationCount++;
    
    Sys_UnlockMutex(resourcePool.resourceMutex);
    
    return &buffer->base;
}

/*
================
R_CreateIndexBuffer

Create an index buffer resource
================
*/
gpuResource_t* R_CreateIndexBuffer(size_t size, gpuResourceUsage_t usage, const void* data) {
    bufferResource_t* buffer;
    
    Sys_LockMutex(resourcePool.resourceMutex);
    
    if (resourcePool.numIndexBuffers >= MAX_INDEX_BUFFERS) {
        Sys_UnlockMutex(resourcePool.resourceMutex);
        ri.Printf(PRINT_WARNING, "R_CreateIndexBuffer: pool full\n");
        return NULL;
    }
    
    buffer = (bufferResource_t*)Z_Malloc(sizeof(bufferResource_t));
    Com_Memset(buffer, 0, sizeof(bufferResource_t));
    
    buffer->base.type = RESOURCE_INDEX_BUFFER;
    buffer->base.usage = usage;
    buffer->base.size = size;
    buffer->base.refCount = 1;
    buffer->base.lastFrameUsed = tr.frameCount;
    
#ifdef USE_VULKAN
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    VkResult result = vkCreateBuffer(vk.device, &bufferInfo, NULL, &buffer->base.vk.buffer);
    if (result != VK_SUCCESS) {
        Z_Free(buffer);
        Sys_UnlockMutex(resourcePool.resourceMutex);
        return NULL;
    }
    
    // Allocate and bind memory (similar to vertex buffer)
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, buffer->base.vk.buffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            (usage & RESOURCE_USAGE_DYNAMIC) ?
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    vkAllocateMemory(vk.device, &allocInfo, NULL, &buffer->base.memory);
    vkBindBufferMemory(vk.device, buffer->base.vk.buffer, buffer->base.memory, 0);
    
    if (usage & RESOURCE_USAGE_PERSISTENT) {
        vkMapMemory(vk.device, buffer->base.memory, 0, size, 0, &buffer->base.mappedPtr);
    }
    
    if (data) {
        if (buffer->base.mappedPtr) {
            Com_Memcpy(buffer->base.mappedPtr, data, size);
        } else {
            vk_upload_buffer_data(buffer->base.vk.buffer, 0, size, data);
        }
    }
#endif
    
    resourcePool.indexBuffers[resourcePool.numIndexBuffers++] = buffer;
    resourcePool.totalAllocated += size;
    resourcePool.allocationCount++;
    
    Sys_UnlockMutex(resourcePool.resourceMutex);
    
    return &buffer->base;
}

/*
================
R_CreateUniformBuffer

Create a uniform buffer resource
================
*/
gpuResource_t* R_CreateUniformBuffer(size_t size, gpuResourceUsage_t usage, const void* data) {
    bufferResource_t* buffer;
    
    Sys_LockMutex(resourcePool.resourceMutex);
    
    if (resourcePool.numUniformBuffers >= MAX_UNIFORM_BUFFERS) {
        Sys_UnlockMutex(resourcePool.resourceMutex);
        ri.Printf(PRINT_WARNING, "R_CreateUniformBuffer: pool full\n");
        return NULL;
    }
    
    buffer = (bufferResource_t*)Z_Malloc(sizeof(bufferResource_t));
    Com_Memset(buffer, 0, sizeof(bufferResource_t));
    
    buffer->base.type = RESOURCE_UNIFORM_BUFFER;
    buffer->base.usage = usage;
    buffer->base.size = size;
    buffer->base.refCount = 1;
    buffer->base.lastFrameUsed = tr.frameCount;
    
#ifdef USE_VULKAN
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    VkResult result = vkCreateBuffer(vk.device, &bufferInfo, NULL, &buffer->base.vk.buffer);
    if (result != VK_SUCCESS) {
        Z_Free(buffer);
        Sys_UnlockMutex(resourcePool.resourceMutex);
        return NULL;
    }
    
    // Uniforms are always host-visible for updates
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, buffer->base.vk.buffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    
    vkAllocateMemory(vk.device, &allocInfo, NULL, &buffer->base.memory);
    vkBindBufferMemory(vk.device, buffer->base.vk.buffer, buffer->base.memory, 0);
    
    // Always map uniform buffers
    vkMapMemory(vk.device, buffer->base.memory, 0, size, 0, &buffer->base.mappedPtr);
    
    if (data && buffer->base.mappedPtr) {
        Com_Memcpy(buffer->base.mappedPtr, data, size);
    }
#endif
    
    resourcePool.uniformBuffers[resourcePool.numUniformBuffers++] = buffer;
    resourcePool.totalAllocated += size;
    resourcePool.allocationCount++;
    
    Sys_UnlockMutex(resourcePool.resourceMutex);
    
    return &buffer->base;
}

/*
================
R_UpdateBuffer

Update buffer data
================
*/
void R_UpdateBuffer(gpuResource_t* resource, size_t offset, size_t size, const void* data) {
    if (!resource || !data) {
        return;
    }
    
    if (offset + size > resource->size) {
        ri.Printf(PRINT_WARNING, "R_UpdateBuffer: update exceeds buffer size\n");
        return;
    }
    
    resource->lastFrameUsed = tr.frameCount;
    
    if (resource->mappedPtr) {
        // Direct update through mapped pointer
        Com_Memcpy((byte*)resource->mappedPtr + offset, data, size);
        resource->dirty = qtrue;
    } else {
#ifdef USE_VULKAN
        // Use staging buffer
        vk_upload_buffer_data(resource->vk.buffer, offset, size, data);
#endif
    }
}

/*
================
R_AddRefResource

Increment resource reference count
================
*/
void R_AddRefResource(gpuResource_t* resource) {
    if (resource) {
        Sys_AtomicAdd(&resource->refCount, 1);
    }
}

/*
================
R_ReleaseResource

Decrement resource reference count and free if zero
================
*/
void R_ReleaseResource(gpuResource_t* resource) {
    if (!resource) {
        return;
    }
    
    int newCount = Sys_AtomicAdd(&resource->refCount, -1) - 1;
    
    if (newCount == 0) {
        Sys_LockMutex(resourcePool.resourceMutex);
        
#ifdef USE_VULKAN
        // Unmap memory if mapped
        if (resource->mappedPtr) {
            vkUnmapMemory(vk.device, resource->memory);
        }
        
        // Destroy Vulkan resources
        switch (resource->type) {
        case RESOURCE_VERTEX_BUFFER:
        case RESOURCE_INDEX_BUFFER:
        case RESOURCE_UNIFORM_BUFFER:
            vkDestroyBuffer(vk.device, resource->vk.buffer, NULL);
            break;
        case RESOURCE_TEXTURE:
            vkDestroyImageView(vk.device, resource->vk.imageView, NULL);
            vkDestroyImage(vk.device, resource->vk.image, NULL);
            break;
        default:
            break;
        }
        
        if (resource->memory) {
            vkFreeMemory(vk.device, resource->memory, NULL);
        }
#endif
        
        resourcePool.totalAllocated -= resource->size;
        resourcePool.allocationCount--;
        
        Sys_UnlockMutex(resourcePool.resourceMutex);
        
        Z_Free(resource);
    }
}

/*
================
R_AllocVertexBuffer

Allocate from dynamic vertex buffer
================
*/
void* R_AllocVertexBuffer(size_t size) {
    void* ptr;
    
    size = PAD(size, 16);  // Align to 16 bytes
    
    gpuResource_t* buffer = resourcePool.frame[resourcePool.currentFrame].dynamicVB;
    size_t* used = &resourcePool.frame[resourcePool.currentFrame].vbUsed;
    
    if (*used + size > buffer->size) {
        ri.Printf(PRINT_WARNING, "R_AllocVertexBuffer: overflow\n");
        return NULL;
    }
    
    ptr = (byte*)buffer->mappedPtr + *used;
    *used += size;
    
    return ptr;
}

/*
================
R_AllocIndexBuffer

Allocate from dynamic index buffer
================
*/
void* R_AllocIndexBuffer(size_t size) {
    void* ptr;
    
    size = PAD(size, 4);  // Align to 4 bytes
    
    gpuResource_t* buffer = resourcePool.frame[resourcePool.currentFrame].dynamicIB;
    size_t* used = &resourcePool.frame[resourcePool.currentFrame].ibUsed;
    
    if (*used + size > buffer->size) {
        ri.Printf(PRINT_WARNING, "R_AllocIndexBuffer: overflow\n");
        return NULL;
    }
    
    ptr = (byte*)buffer->mappedPtr + *used;
    *used += size;
    
    return ptr;
}

/*
================
Frame_Alloc

Allocate temporary memory for current frame
================
*/
static void* Resource_FrameAlloc(size_t size) {
    frameAlloc_t* fa = &frameAlloc[currentFrameAlloc];
    void* ptr;
    
    size = PAD(size, 16);
    
    if (fa->tempPool.used + size > fa->tempPool.size) {
        ri.Printf(PRINT_WARNING, "Frame_Alloc: overflow\n");
        return NULL;
    }
    
    ptr = (byte*)fa->tempPool.base + fa->tempPool.used;
    fa->tempPool.used += size;
    
    return ptr;
}

/*
================
Frame_Clear

Reset frame allocator
================
*/
static void Resource_FrameClear(void) {
    currentFrameAlloc = !currentFrameAlloc;
    frameAlloc_t* fa = &frameAlloc[currentFrameAlloc];
    
    fa->used = 0;
    fa->vertexPool.used = 0;
    fa->indexPool.used = 0;
    fa->uniformPool.used = 0;
    fa->tempPool.used = 0;
}

/*
================
R_PrintResourceStats

Print resource usage statistics
================
*/
void R_PrintResourceStats(void) {
    ri.Printf(PRINT_ALL, "Resource Statistics:\n");
    ri.Printf(PRINT_ALL, "  Total allocated: %zu MB\n", resourcePool.totalAllocated / (1024 * 1024));
    ri.Printf(PRINT_ALL, "  Allocations: %d\n", resourcePool.allocationCount);
    ri.Printf(PRINT_ALL, "  Vertex buffers: %d / %d\n", resourcePool.numVertexBuffers, MAX_VERTEX_BUFFERS);
    ri.Printf(PRINT_ALL, "  Index buffers: %d / %d\n", resourcePool.numIndexBuffers, MAX_INDEX_BUFFERS);
    ri.Printf(PRINT_ALL, "  Uniform buffers: %d / %d\n", resourcePool.numUniformBuffers, MAX_UNIFORM_BUFFERS);
    ri.Printf(PRINT_ALL, "  Textures: %d / %d\n", resourcePool.numTextures, MAX_TEXTURE_RESOURCES);
    
    for (int i = 0; i < 2; i++) {
        ri.Printf(PRINT_ALL, "  Frame %d dynamic usage:\n", i);
        ri.Printf(PRINT_ALL, "    Vertex: %zu KB\n", resourcePool.frame[i].vbUsed / 1024);
        ri.Printf(PRINT_ALL, "    Index: %zu KB\n", resourcePool.frame[i].ibUsed / 1024);
        ri.Printf(PRINT_ALL, "    Uniform: %zu KB\n", resourcePool.frame[i].ubUsed / 1024);
    }
}