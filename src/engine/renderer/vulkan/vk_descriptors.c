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
#include "vk.h"
#include "vk_shader.h"

/*
================================================================================
Vulkan Descriptor Set Management

This file implements efficient descriptor set management for texture binding
and uniform buffers.
================================================================================
*/

// Helper functions to get Vulkan resources from images
static inline VkSampler VK_GetTextureSampler(image_t *image) {
    // For now, return a default sampler
    // TODO: Implement proper sampler selection based on image flags
    extern VkSampler vk_default_sampler;  // Assume this exists in vk.c
    return vk_default_sampler;
}

static inline VkImageView VK_GetTextureView(image_t *image) {
    if (image) {
        return image->view;
    }
    return VK_NULL_HANDLE;
}

#define MAX_DESCRIPTOR_SETS 1024
#define MAX_TEXTURE_DESCRIPTORS 512
#define MAX_UNIFORM_DESCRIPTORS 64
#define DESCRIPTOR_POOL_SIZE 1100

typedef struct vkDescriptorCache_s {
    VkDescriptorSet     set;
    uint32_t            hash;
    int                 frameUsed;
    qboolean            inUse;
} vkDescriptorCache_t;

typedef struct vkDescriptorManager_s {
    VkDescriptorPool            pool;
    VkDescriptorSetLayout       textureLayout;
    VkDescriptorSetLayout       uniformLayout;
    VkDescriptorSetLayout       combinedLayout;
    
    // Descriptor set cache
    vkDescriptorCache_t         textureCache[MAX_TEXTURE_DESCRIPTORS];
    int                         numTextureDescriptors;
    
    vkDescriptorCache_t         uniformCache[MAX_UNIFORM_DESCRIPTORS];
    int                         numUniformDescriptors;
    
    // Statistics
    int                         allocations;
    int                         cacheHits;
    int                         cacheMisses;
} vkDescriptorManager_t;

static vkDescriptorManager_t descriptorManager;

// Texture binding slots
enum {
    TEXTURE_SLOT_DIFFUSE = 0,
    TEXTURE_SLOT_LIGHTMAP = 1,
    TEXTURE_SLOT_NORMAL = 2,
    TEXTURE_SLOT_SPECULAR = 3,
    TEXTURE_SLOT_GLOW = 4,
    TEXTURE_SLOT_DETAIL = 5,
    TEXTURE_SLOT_ENVIRONMENT = 6,
    MAX_TEXTURE_SLOTS = 7
};

/*
================
VK_InitDescriptorManager

Initialize the descriptor set manager
================
*/
void VK_InitDescriptorManager(void) {
    VkDescriptorPoolSize poolSizes[3];
    VkDescriptorPoolCreateInfo poolInfo;
    VkDescriptorSetLayoutBinding textureBindings[MAX_TEXTURE_SLOTS];
    VkDescriptorSetLayoutBinding uniformBinding;
    VkDescriptorSetLayoutCreateInfo layoutInfo;
    VkResult result;
    int i;
    
    ri.Printf(PRINT_ALL, "Initializing descriptor manager...\n");
    
    Com_Memset(&descriptorManager, 0, sizeof(descriptorManager));
    
    // Create descriptor pool
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = MAX_TEXTURE_DESCRIPTORS * MAX_TEXTURE_SLOTS;
    
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = MAX_UNIFORM_DESCRIPTORS;
    
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[2].descriptorCount = MAX_UNIFORM_DESCRIPTORS;
    
    Com_Memset(&poolInfo, 0, sizeof(poolInfo));
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = DESCRIPTOR_POOL_SIZE;
    poolInfo.poolSizeCount = ARRAY_LEN(poolSizes);
    poolInfo.pPoolSizes = poolSizes;
    
    result = vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &descriptorManager.pool);
    if (result != VK_SUCCESS) {
        ri.Error(ERR_FATAL, "Failed to create descriptor pool: %d", result);
    }
    
    // Create texture descriptor set layout
    for (i = 0; i < MAX_TEXTURE_SLOTS; i++) {
        Com_Memset(&textureBindings[i], 0, sizeof(textureBindings[i]));
        textureBindings[i].binding = i;
        textureBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        textureBindings[i].descriptorCount = 1;
        textureBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    
    Com_Memset(&layoutInfo, 0, sizeof(layoutInfo));
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = MAX_TEXTURE_SLOTS;
    layoutInfo.pBindings = textureBindings;
    
    result = vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, 
                                        &descriptorManager.textureLayout);
    if (result != VK_SUCCESS) {
        ri.Error(ERR_FATAL, "Failed to create texture descriptor set layout: %d", result);
    }
    
    // Create uniform buffer descriptor set layout
    Com_Memset(&uniformBinding, 0, sizeof(uniformBinding));
    uniformBinding.binding = 0;
    uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uniformBinding.descriptorCount = 1;
    uniformBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    
    Com_Memset(&layoutInfo, 0, sizeof(layoutInfo));
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uniformBinding;
    
    result = vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL,
                                        &descriptorManager.uniformLayout);
    if (result != VK_SUCCESS) {
        ri.Error(ERR_FATAL, "Failed to create uniform descriptor set layout: %d", result);
    }
    
    // Store layouts for pipeline creation
    // NOTE: The descriptor layouts are already stored in vk.set_layout_sampler, 
    // vk.set_layout_uniform, vk.set_layout_storage
    // No need to duplicate them here
    
    ri.Printf(PRINT_ALL, "Descriptor manager initialized\n");
}

/*
================
VK_ShutdownDescriptorManager

Shutdown the descriptor set manager
================
*/
void VK_ShutdownDescriptorManager(void) {
    // Free all cached descriptor sets
    if (descriptorManager.pool != VK_NULL_HANDLE) {
        vkResetDescriptorPool(vk.device, descriptorManager.pool, 0);
    }
    
    // Destroy layouts
    if (descriptorManager.textureLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk.device, descriptorManager.textureLayout, NULL);
        descriptorManager.textureLayout = VK_NULL_HANDLE;
    }
    
    if (descriptorManager.uniformLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk.device, descriptorManager.uniformLayout, NULL);
        descriptorManager.uniformLayout = VK_NULL_HANDLE;
    }
    
    if (descriptorManager.combinedLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk.device, descriptorManager.combinedLayout, NULL);
        descriptorManager.combinedLayout = VK_NULL_HANDLE;
    }
    
    // Destroy pool
    if (descriptorManager.pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk.device, descriptorManager.pool, NULL);
        descriptorManager.pool = VK_NULL_HANDLE;
    }
    
    Com_Memset(&descriptorManager, 0, sizeof(descriptorManager));
}

/*
================
VK_HashStageTextures

Generate hash for texture combination
================
*/
static uint32_t VK_HashStageTextures(const materialStage_t *stage) {
    uint32_t hash = 0;
    int i, j;
    
    // Hash primary texture bundle
    for (i = 0; i < NUM_TEXTURE_BUNDLES; i++) {
        for (j = 0; j < MAX_IMAGE_ANIMATIONS; j++) {
            if (stage->bundle[i].image[j]) {
                hash = hash * 31 + (uint32_t)(intptr_t)stage->bundle[i].image[j];
            }
        }
    }
    
    // Hash additional textures
    if (stage->normalMap) {
        hash = hash * 31 + (uint32_t)(intptr_t)stage->normalMap;
    }
    if (stage->specularMap) {
        hash = hash * 31 + (uint32_t)(intptr_t)stage->specularMap;
    }
    if (stage->glowMap) {
        hash = hash * 31 + (uint32_t)(intptr_t)stage->glowMap;
    }
    
    return hash;
}

/*
================
VK_AllocateDescriptorSet

Allocate a new descriptor set
================
*/
static VkDescriptorSet VK_AllocateDescriptorSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo allocInfo;
    VkDescriptorSet set;
    VkResult result;
    
    Com_Memset(&allocInfo, 0, sizeof(allocInfo));
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorManager.pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;
    
    result = vkAllocateDescriptorSets(vk.device, &allocInfo, &set);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "Failed to allocate descriptor set: %d\n", result);
        return VK_NULL_HANDLE;
    }
    
    descriptorManager.allocations++;
    return set;
}

/*
================
VK_GetTextureDescriptorSet

Get or create texture descriptor set for stage
================
*/
VkDescriptorSet VK_GetTextureDescriptorSet(const materialStage_t *stage) {
    uint32_t hash;
    VkDescriptorSet set;
    VkDescriptorImageInfo imageInfos[MAX_TEXTURE_SLOTS];
    VkWriteDescriptorSet writes[MAX_TEXTURE_SLOTS];
    int numWrites = 0;
    int i;
    
    // Generate hash for this texture combination
    hash = VK_HashStageTextures(stage);
    
    // Look for existing cached set
    for (i = 0; i < descriptorManager.numTextureDescriptors; i++) {
        if (descriptorManager.textureCache[i].hash == hash && 
            descriptorManager.textureCache[i].inUse) {
            descriptorManager.cacheHits++;
            descriptorManager.textureCache[i].frameUsed = tr.frameCount;
            return descriptorManager.textureCache[i].set;
        }
    }
    
    descriptorManager.cacheMisses++;
    
    // Allocate new descriptor set
    set = VK_AllocateDescriptorSet(descriptorManager.textureLayout);
    if (set == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    
    // Setup diffuse texture
    if (stage->bundle[0].image[0]) {
        Com_Memset(&imageInfos[numWrites], 0, sizeof(imageInfos[numWrites]));
        imageInfos[numWrites].sampler = VK_GetTextureSampler(stage->bundle[0].image[0]);
        imageInfos[numWrites].imageView = VK_GetTextureView(stage->bundle[0].image[0]);
        imageInfos[numWrites].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        
        Com_Memset(&writes[numWrites], 0, sizeof(writes[numWrites]));
        writes[numWrites].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[numWrites].dstSet = set;
        writes[numWrites].dstBinding = TEXTURE_SLOT_DIFFUSE;
        writes[numWrites].dstArrayElement = 0;
        writes[numWrites].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[numWrites].descriptorCount = 1;
        writes[numWrites].pImageInfo = &imageInfos[numWrites];
        numWrites++;
    }
    
    // Setup lightmap texture
    if (stage->bundle[1].image[0]) {
        Com_Memset(&imageInfos[numWrites], 0, sizeof(imageInfos[numWrites]));
        imageInfos[numWrites].sampler = VK_GetTextureSampler(stage->bundle[1].image[0]);
        imageInfos[numWrites].imageView = VK_GetTextureView(stage->bundle[1].image[0]);
        imageInfos[numWrites].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        
        Com_Memset(&writes[numWrites], 0, sizeof(writes[numWrites]));
        writes[numWrites].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[numWrites].dstSet = set;
        writes[numWrites].dstBinding = TEXTURE_SLOT_LIGHTMAP;
        writes[numWrites].dstArrayElement = 0;
        writes[numWrites].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[numWrites].descriptorCount = 1;
        writes[numWrites].pImageInfo = &imageInfos[numWrites];
        numWrites++;
    }
    
    // Setup normal map
    if (stage->normalMap) {
        Com_Memset(&imageInfos[numWrites], 0, sizeof(imageInfos[numWrites]));
        imageInfos[numWrites].sampler = VK_GetTextureSampler(stage->normalMap);
        imageInfos[numWrites].imageView = VK_GetTextureView(stage->normalMap);
        imageInfos[numWrites].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        
        Com_Memset(&writes[numWrites], 0, sizeof(writes[numWrites]));
        writes[numWrites].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[numWrites].dstSet = set;
        writes[numWrites].dstBinding = TEXTURE_SLOT_NORMAL;
        writes[numWrites].dstArrayElement = 0;
        writes[numWrites].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[numWrites].descriptorCount = 1;
        writes[numWrites].pImageInfo = &imageInfos[numWrites];
        numWrites++;
    }
    
    // Setup specular map
    if (stage->specularMap) {
        Com_Memset(&imageInfos[numWrites], 0, sizeof(imageInfos[numWrites]));
        imageInfos[numWrites].sampler = VK_GetTextureSampler(stage->specularMap);
        imageInfos[numWrites].imageView = VK_GetTextureView(stage->specularMap);
        imageInfos[numWrites].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        
        Com_Memset(&writes[numWrites], 0, sizeof(writes[numWrites]));
        writes[numWrites].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[numWrites].dstSet = set;
        writes[numWrites].dstBinding = TEXTURE_SLOT_SPECULAR;
        writes[numWrites].dstArrayElement = 0;
        writes[numWrites].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[numWrites].descriptorCount = 1;
        writes[numWrites].pImageInfo = &imageInfos[numWrites];
        numWrites++;
    }
    
    // Setup glow map
    if (stage->glowMap) {
        Com_Memset(&imageInfos[numWrites], 0, sizeof(imageInfos[numWrites]));
        imageInfos[numWrites].sampler = VK_GetTextureSampler(stage->glowMap);
        imageInfos[numWrites].imageView = VK_GetTextureView(stage->glowMap);
        imageInfos[numWrites].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        
        Com_Memset(&writes[numWrites], 0, sizeof(writes[numWrites]));
        writes[numWrites].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[numWrites].dstSet = set;
        writes[numWrites].dstBinding = TEXTURE_SLOT_GLOW;
        writes[numWrites].dstArrayElement = 0;
        writes[numWrites].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[numWrites].descriptorCount = 1;
        writes[numWrites].pImageInfo = &imageInfos[numWrites];
        numWrites++;
    }
    
    // Update descriptor set
    if (numWrites > 0) {
        vkUpdateDescriptorSets(vk.device, numWrites, writes, 0, NULL);
    }
    
    // Cache the descriptor set
    if (descriptorManager.numTextureDescriptors < MAX_TEXTURE_DESCRIPTORS) {
        vkDescriptorCache_t *cache = &descriptorManager.textureCache[descriptorManager.numTextureDescriptors++];
        cache->set = set;
        cache->hash = hash;
        cache->frameUsed = tr.frameCount;
        cache->inUse = qtrue;
    }
    
    return set;
}

/*
================
VK_BindTextureDescriptorSet

Bind texture descriptor set to command buffer
================
*/
void VK_BindTextureDescriptorSet(VkDescriptorSet set, VkPipelineLayout layout) {
    if (set == VK_NULL_HANDLE) {
        return;
    }
    
    vkCmdBindDescriptorSets(vk.cmd->command_buffer, 
                           VK_PIPELINE_BIND_POINT_GRAPHICS,
                           layout,
                           0,  // First set
                           1,  // Set count
                           &set,
                           0,  // Dynamic offset count
                           NULL);  // Dynamic offsets
}

/*
================
VK_CleanupDescriptorCache

Clean up unused descriptor sets
================
*/
void VK_CleanupDescriptorCache(void) {
    int i;
    int frameThreshold = tr.frameCount - 100;  // Keep sets used in last 100 frames
    
    // Clean texture descriptor cache
    for (i = 0; i < descriptorManager.numTextureDescriptors; i++) {
        if (descriptorManager.textureCache[i].inUse &&
            descriptorManager.textureCache[i].frameUsed < frameThreshold) {
            // Free the descriptor set
            vkFreeDescriptorSets(vk.device, descriptorManager.pool, 1, 
                               &descriptorManager.textureCache[i].set);
            descriptorManager.textureCache[i].inUse = qfalse;
        }
    }
    
    // Compact the cache
    int writeIndex = 0;
    for (i = 0; i < descriptorManager.numTextureDescriptors; i++) {
        if (descriptorManager.textureCache[i].inUse) {
            if (writeIndex != i) {
                descriptorManager.textureCache[writeIndex] = descriptorManager.textureCache[i];
            }
            writeIndex++;
        }
    }
    descriptorManager.numTextureDescriptors = writeIndex;
}

/*
================
VK_GetDescriptorStats

Get descriptor manager statistics
================
*/
void VK_GetDescriptorStats(int *allocations, int *cacheHits, int *cacheMisses) {
    if (allocations) {
        *allocations = descriptorManager.allocations;
    }
    if (cacheHits) {
        *cacheHits = descriptorManager.cacheHits;
    }
    if (cacheMisses) {
        *cacheMisses = descriptorManager.cacheMisses;
    }
}

/*
================
VK_ResetDescriptorStats

Reset descriptor manager statistics
================
*/
void VK_ResetDescriptorStats(void) {
    descriptorManager.allocations = 0;
    descriptorManager.cacheHits = 0;
    descriptorManager.cacheMisses = 0;
}