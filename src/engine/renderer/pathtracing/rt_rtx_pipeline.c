/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

RTX Ray Tracing Pipeline Management
Handles RT pipeline creation, shader binding table, and descriptor sets
===========================================================================
*/
// Force rebuild

#include "rt_rtx.h"
#include "rt_pathtracer.h"
#include "../core/tr_local.h"
#include "../vulkan/vk.h"
#include <stdio.h>

// External RTX state
extern rtxState_t rtx;

// Pipeline management structures
typedef struct {
    VkShaderModule raygenShader;
    VkShaderModule missShader;
    VkShaderModule shadowMissShader;
    VkShaderModule closestHitShader;
} rtxShaders_t;

typedef struct {
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
} rtxPipelineInfo_t;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress deviceAddress;
    VkStridedDeviceAddressRegionKHR raygenRegion;
    VkStridedDeviceAddressRegionKHR missRegion;
    VkStridedDeviceAddressRegionKHR hitRegion;
    VkStridedDeviceAddressRegionKHR callableRegion;
    uint32_t handleSize;
    uint32_t handleSizeAligned;
    uint32_t groupCount;
} rtxSBT_t;

// Uniform buffer structures
typedef struct {
    float viewInverse[16];     // mat4
    float projInverse[16];     // mat4
    vec3_t position;
    float time;
    vec3_t forward;
    float fov;
    vec3_t right;
    float nearPlane;
    vec3_t up;
    float farPlane;
    vec2_t jitter;
    vec2_t previousJitter;
    float previousViewProjection[16];  // mat4
    uint32_t frameCount;
    uint32_t enablePathTracing;
    uint32_t maxBounces;
    uint32_t samplesPerPixel;
} CameraUBO;

typedef struct {
    uint32_t enableShadows;
    uint32_t enableReflections;
    uint32_t enableGI;
    uint32_t enableAO;
    float shadowBias;
    float reflectionRoughnessCutoff;
    float giIntensity;
    float aoRadius;
    uint32_t debugMode;
    uint32_t enableDenoiser;
    uint32_t enableDLSS;
    uint32_t enableMotionBlur;
} RenderSettingsUBO;

typedef struct {
    vec3_t sunDirection;
    float sunIntensity;
    vec3_t sunColor;
    float skyIntensity;
    vec3_t fogColor;
    float fogDensity;
    float fogStart;
    float fogEnd;
    uint32_t useEnvironmentMap;
    uint32_t useProceduralSky;
    float time;
    float cloudCoverage;
} EnvironmentUBO;

// Material data for PBR
typedef struct {
    vec4_t albedo;
    vec4_t specular;
    vec4_t emission;
    float roughness;
    float metallic;
    float normalScale;
    float occlusionStrength;
    uint32_t albedoTexture;
    uint32_t normalTexture;
    uint32_t roughnessTexture;
    uint32_t metallicTexture;
    uint32_t emissionTexture;
    uint32_t occlusionTexture;
    uint32_t lightmapTexture;
    uint32_t flags;
} MaterialData;

// Light data
typedef struct {
    vec4_t position;    // w = type (0=directional, 1=point, 2=spot)
    vec4_t direction;   // w = inner cone angle for spot
    vec4_t color;       // w = intensity
    vec4_t attenuation; // x=constant, y=linear, z=quadratic, w=outer cone angle
} LightData;

// Global pipeline state
static struct {
    rtxShaders_t shaders;
    rtxPipelineInfo_t pipeline;
    rtxSBT_t sbt;
    
    // Descriptor resources
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSet;
    
    // Uniform buffers
    VkBuffer cameraUBO;
    VkDeviceMemory cameraUBOMemory;
    VkBuffer renderSettingsUBO;
    VkDeviceMemory renderSettingsUBOMemory;
    VkBuffer environmentUBO;
    VkDeviceMemory environmentUBOMemory;
    
    // Storage buffers
    VkBuffer materialBuffer;
    VkDeviceMemory materialBufferMemory;
    VkBuffer lightBuffer;
    VkDeviceMemory lightBufferMemory;
    VkBuffer instanceDataBuffer;
    VkDeviceMemory instanceDataBufferMemory;
    
    // Texture arrays
    VkSampler textureSampler;
    uint32_t textureCount;
    VkImageView *textureViews;
    uint32_t lightmapCount;
    VkImageView *lightmapViews;
    
    // RT properties
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties;
} rtxPipeline;

// Function pointers for RT pipeline
static PFN_vkCreateRayTracingPipelinesKHR qvkCreateRayTracingPipelinesKHR;
static PFN_vkGetRayTracingShaderGroupHandlesKHR qvkGetRayTracingShaderGroupHandlesKHR;
static PFN_vkCmdTraceRaysKHR qvkCmdTraceRaysKHR;

/*
================
RTX_LoadShaderModule

Load compiled SPIR-V shader from disk
================
*/
static VkShaderModule RTX_LoadShaderModule(VkDevice device, const char *filename) {
    FILE *file;
    size_t fileSize;
    uint32_t *shaderCode;
    VkShaderModule module = VK_NULL_HANDLE;
    
    // Build full path
    char fullPath[MAX_QPATH];
    Com_sprintf(fullPath, sizeof(fullPath), "baseq3/shaders/rtx/%s", filename);
    
    // Open SPIR-V file
    file = fopen(fullPath, "rb");
    if (!file) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to open shader file: %s\n", fullPath);
        return VK_NULL_HANDLE;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Allocate and read shader code
    shaderCode = Z_Malloc(fileSize);
    if (fread(shaderCode, 1, fileSize, file) != fileSize) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to read shader file: %s\n", fullPath);
        Z_Free(shaderCode);
        fclose(file);
        return VK_NULL_HANDLE;
    }
    fclose(file);
    
    // Create shader module
    VkShaderModuleCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fileSize,
        .pCode = shaderCode
    };
    
    VkResult result = vkCreateShaderModule(device, &createInfo, NULL, &module);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create shader module from %s (result: %d)\n", 
                  filename, result);
        module = VK_NULL_HANDLE;
    } else {
        ri.Printf(PRINT_ALL, "RTX: Loaded shader module: %s (%zu bytes)\n", filename, fileSize);
    }
    
    Z_Free(shaderCode);
    return module;
}

/*
================
RTX_CreateDescriptorSetLayout

Create descriptor set layout for RT resources
================
*/
static qboolean RTX_CreateDescriptorSetLayout(VkDevice device) {
    VkDescriptorSetLayoutBinding bindings[] = {
        // Binding 0: TLAS
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        },
        // Binding 1: Output color image
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 2: Albedo image
        {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 3: Normal image
        {
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 4: Motion vector image
        {
            .binding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 5: Depth image
        {
            .binding = 5,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 6: Camera UBO
        {
            .binding = 6,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        },
        // Binding 7: Render settings UBO
        {
            .binding = 7,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        },
        // Binding 8: Environment map
        {
            .binding = 8,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR
        },
        // Binding 9: Environment data UBO
        {
            .binding = 9,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR
        },
        // Binding 10: Instance data buffer
        {
            .binding = 10,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        },
        // Binding 11: Material buffer
        {
            .binding = 11,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        },
        // Binding 12: Texture array
        {
            .binding = 12,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 256,  // Max textures
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        },
        // Binding 13: Lightmap array
        {
            .binding = 13,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 64,   // Max lightmaps
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        },
        // Binding 14: Light buffer
        {
            .binding = 14,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        }
    };
    
    // Enable variable descriptor counts for texture arrays
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = ARRAY_LEN(bindings)
    };
    
    VkDescriptorBindingFlags flags[ARRAY_LEN(bindings)] = {0};
    // Only allow partially bound for texture arrays, not variable count
    // since they're not the highest binding number
    flags[12] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    flags[13] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    bindingFlags.pBindingFlags = flags;
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bindingFlags,
        .bindingCount = ARRAY_LEN(bindings),
        .pBindings = bindings
    };
    
    VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, NULL, 
                                                  &rtxPipeline.pipeline.descriptorSetLayout);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create descriptor set layout (result: %d)\n", result);
        return qfalse;
    }
    
    return qtrue;
}

/*
================
RTX_CreateDescriptorPool

Create descriptor pool for RT resources
================
*/
static qboolean RTX_CreateDescriptorPool(VkDevice device) {
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 321 }, // 256 + 64 + 1
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 }
    };
    
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = ARRAY_LEN(poolSizes),
        .pPoolSizes = poolSizes
    };
    
    VkResult result = vkCreateDescriptorPool(device, &poolInfo, NULL, &rtxPipeline.descriptorPool);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create descriptor pool (result: %d)\n", result);
        return qfalse;
    }
    
    return qtrue;
}

/*
================
RTX_AllocateDescriptorSets

Allocate descriptor sets from pool
================
*/
static qboolean RTX_AllocateDescriptorSets(VkDevice device) {
    // No variable descriptor count needed since we're not using 
    // VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
    
    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = NULL,
        .descriptorPool = rtxPipeline.descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &rtxPipeline.pipeline.descriptorSetLayout
    };
    
    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &rtxPipeline.descriptorSet);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to allocate descriptor sets (result: %d)\n", result);
        return qfalse;
    }
    
    return qtrue;
}

/*
================
RTX_CreateUniformBuffers

Create uniform buffers for camera, settings, and environment
================
*/
static qboolean RTX_CreateUniformBuffers(VkDevice device, VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    
    // Camera UBO
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(CameraUBO),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.cameraUBO) != VK_SUCCESS) {
        return qfalse;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, rtxPipeline.cameraUBO, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    
    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.cameraUBOMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.cameraUBO, NULL);
        return qfalse;
    }
    
    vkBindBufferMemory(device, rtxPipeline.cameraUBO, rtxPipeline.cameraUBOMemory, 0);
    
    // Render settings UBO
    bufferInfo.size = sizeof(RenderSettingsUBO);
    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.renderSettingsUBO) != VK_SUCCESS) {
        return qfalse;
    }
    
    vkGetBufferMemoryRequirements(device, rtxPipeline.renderSettingsUBO, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    
    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.renderSettingsUBOMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.renderSettingsUBO, NULL);
        return qfalse;
    }
    
    vkBindBufferMemory(device, rtxPipeline.renderSettingsUBO, rtxPipeline.renderSettingsUBOMemory, 0);
    
    // Environment UBO
    bufferInfo.size = sizeof(EnvironmentUBO);
    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.environmentUBO) != VK_SUCCESS) {
        return qfalse;
    }
    
    vkGetBufferMemoryRequirements(device, rtxPipeline.environmentUBO, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    
    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.environmentUBOMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.environmentUBO, NULL);
        return qfalse;
    }
    
    vkBindBufferMemory(device, rtxPipeline.environmentUBO, rtxPipeline.environmentUBOMemory, 0);
    
    return qtrue;
}

/*
================
RTX_CreateStorageBuffers

Create storage buffers for materials, lights, and instance data
================
*/
static qboolean RTX_CreateStorageBuffers(VkDevice device, VkPhysicalDevice physicalDevice) {
    // Material buffer (max 4096 materials)
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(MaterialData) * 4096,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.materialBuffer) != VK_SUCCESS) {
        return qfalse;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, rtxPipeline.materialBuffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.materialBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.materialBuffer, NULL);
        return qfalse;
    }
    
    vkBindBufferMemory(device, rtxPipeline.materialBuffer, rtxPipeline.materialBufferMemory, 0);
    
    // Light buffer (max 256 lights)
    bufferInfo.size = sizeof(uint32_t) + sizeof(LightData) * 256;
    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.lightBuffer) != VK_SUCCESS) {
        return qfalse;
    }
    
    vkGetBufferMemoryRequirements(device, rtxPipeline.lightBuffer, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    
    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.lightBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.lightBuffer, NULL);
        return qfalse;
    }
    
    vkBindBufferMemory(device, rtxPipeline.lightBuffer, rtxPipeline.lightBufferMemory, 0);
    
    // Instance data buffer
    bufferInfo.size = sizeof(uint64_t) * 8 * RTX_MAX_INSTANCES;  // Instance data structure size
    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.instanceDataBuffer) != VK_SUCCESS) {
        return qfalse;
    }
    
    vkGetBufferMemoryRequirements(device, rtxPipeline.instanceDataBuffer, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    
    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.instanceDataBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.instanceDataBuffer, NULL);
        return qfalse;
    }
    
    vkBindBufferMemory(device, rtxPipeline.instanceDataBuffer, rtxPipeline.instanceDataBufferMemory, 0);
    
    return qtrue;
}

/*
================
RTX_CreateTextureSampler

Create sampler for texture arrays
================
*/
static qboolean RTX_CreateTextureSampler(VkDevice device) {
    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = 16.0f,
        .compareEnable = VK_FALSE,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE
    };
    
    VkResult result = vkCreateSampler(device, &samplerInfo, NULL, &rtxPipeline.textureSampler);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create texture sampler (result: %d)\n", result);
        return qfalse;
    }
    
    return qtrue;
}

/*
================
RTX_CreateRTPipeline

Create ray tracing pipeline with all shader stages
================
*/
qboolean RTX_CreateRTPipeline(VkDevice device, VkPhysicalDevice physicalDevice) {
    VkResult result;
    
    // Load RT extension function pointers
    qvkCreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR)
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR");
    qvkGetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR)
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR");
    qvkCmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)
        vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR");
    
    if (!qvkCreateRayTracingPipelinesKHR || !qvkGetRayTracingShaderGroupHandlesKHR || !qvkCmdTraceRaysKHR) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to load RT pipeline function pointers\n");
        return qfalse;
    }
    
    // Get RT pipeline properties
    rtxPipeline.rtProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &rtxPipeline.rtProperties
    };
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
    
    // Load shader modules
    rtxPipeline.shaders.raygenShader = RTX_LoadShaderModule(device, "raygen.spv");
    rtxPipeline.shaders.missShader = RTX_LoadShaderModule(device, "miss.spv");
    rtxPipeline.shaders.shadowMissShader = RTX_LoadShaderModule(device, "shadow.spv");
    rtxPipeline.shaders.closestHitShader = RTX_LoadShaderModule(device, "closesthit.spv");
    
    if (!rtxPipeline.shaders.raygenShader || !rtxPipeline.shaders.missShader ||
        !rtxPipeline.shaders.shadowMissShader || !rtxPipeline.shaders.closestHitShader) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to load one or more shader modules\n");
        return qfalse;
    }
    
    // Create shader stages
    VkPipelineShaderStageCreateInfo shaderStages[] = {
        // Ray generation
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            .module = rtxPipeline.shaders.raygenShader,
            .pName = "main"
        },
        // Miss
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
            .module = rtxPipeline.shaders.missShader,
            .pName = "main"
        },
        // Shadow miss
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
            .module = rtxPipeline.shaders.shadowMissShader,
            .pName = "main"
        },
        // Closest hit
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            .module = rtxPipeline.shaders.closestHitShader,
            .pName = "main"
        }
    };
    
    // Create shader groups
    VkRayTracingShaderGroupCreateInfoKHR shaderGroups[] = {
        // Group 0: Ray generation
        {
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
            .generalShader = 0,
            .closestHitShader = VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR
        },
        // Group 1: Miss
        {
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
            .generalShader = 1,
            .closestHitShader = VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR
        },
        // Group 2: Shadow miss
        {
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
            .generalShader = 2,
            .closestHitShader = VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR
        },
        // Group 3: Hit
        {
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
            .generalShader = VK_SHADER_UNUSED_KHR,
            .closestHitShader = 3,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR
        }
    };
    
    // Create descriptor set layout
    if (!RTX_CreateDescriptorSetLayout(device)) {
        return qfalse;
    }
    
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &rtxPipeline.pipeline.descriptorSetLayout,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = NULL
    };
    
    result = vkCreatePipelineLayout(device, &pipelineLayoutInfo, NULL, 
                                    &rtxPipeline.pipeline.pipelineLayout);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create pipeline layout (result: %d)\n", result);
        return qfalse;
    }
    
    // Create ray tracing pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
        .stageCount = ARRAY_LEN(shaderStages),
        .pStages = shaderStages,
        .groupCount = ARRAY_LEN(shaderGroups),
        .pGroups = shaderGroups,
        .maxPipelineRayRecursionDepth = rtx_gi_bounces ? rtx_gi_bounces->integer : 2,
        .layout = rtxPipeline.pipeline.pipelineLayout
    };
    
    result = qvkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, 
                                             &pipelineInfo, NULL, &rtxPipeline.pipeline.pipeline);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create ray tracing pipeline (result: %d)\n", result);
        return qfalse;
    }
    
    ri.Printf(PRINT_ALL, "RTX: Ray tracing pipeline created successfully\n");
    return qtrue;
}

/*
================
RTX_CreateShaderBindingTable

Create and populate shader binding table
================
*/
qboolean RTX_CreateShaderBindingTable(VkDevice device, VkPhysicalDevice physicalDevice) {
    VkResult result;
    
    // Calculate SBT properties
    uint32_t handleSize = rtxPipeline.rtProperties.shaderGroupHandleSize;
    uint32_t handleAlignment = rtxPipeline.rtProperties.shaderGroupHandleAlignment;
    uint32_t baseAlignment = rtxPipeline.rtProperties.shaderGroupBaseAlignment;
    
    rtxPipeline.sbt.handleSize = handleSize;
    rtxPipeline.sbt.handleSizeAligned = (handleSize + handleAlignment - 1) & ~(handleAlignment - 1);
    rtxPipeline.sbt.groupCount = 4; // raygen, miss, shadow miss, hit
    
    // Calculate SBT buffer size
    uint32_t sbtSize = rtxPipeline.sbt.groupCount * rtxPipeline.sbt.handleSizeAligned;
    sbtSize = (sbtSize + baseAlignment - 1) & ~(baseAlignment - 1);
    
    // Create SBT buffer
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sbtSize,
        .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | 
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    result = vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.sbt.buffer);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create SBT buffer (result: %d)\n", result);
        return qfalse;
    }
    
    // Allocate memory
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, rtxPipeline.sbt.buffer, &memReqs);
    
    VkMemoryAllocateFlagsInfo memoryAllocateFlagsInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &memoryAllocateFlagsInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    
    result = vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.sbt.memory);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to allocate SBT memory (result: %d)\n", result);
        vkDestroyBuffer(device, rtxPipeline.sbt.buffer, NULL);
        return qfalse;
    }
    
    vkBindBufferMemory(device, rtxPipeline.sbt.buffer, rtxPipeline.sbt.memory, 0);
    
    // Get shader group handles
    uint8_t *shaderHandles = Z_Malloc(handleSize * rtxPipeline.sbt.groupCount);
    result = qvkGetRayTracingShaderGroupHandlesKHR(device, rtxPipeline.pipeline.pipeline,
                                                   0, rtxPipeline.sbt.groupCount,
                                                   handleSize * rtxPipeline.sbt.groupCount,
                                                   shaderHandles);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to get shader group handles (result: %d)\n", result);
        Z_Free(shaderHandles);
        return qfalse;
    }
    
    // Map SBT memory and copy handles
    void *mapped;
    result = vkMapMemory(device, rtxPipeline.sbt.memory, 0, sbtSize, 0, &mapped);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to map SBT memory (result: %d)\n", result);
        Z_Free(shaderHandles);
        return qfalse;
    }
    
    uint8_t *pData = (uint8_t*)mapped;
    for (uint32_t i = 0; i < rtxPipeline.sbt.groupCount; i++) {
        Com_Memcpy(pData, shaderHandles + i * handleSize, handleSize);
        pData += rtxPipeline.sbt.handleSizeAligned;
    }
    
    vkUnmapMemory(device, rtxPipeline.sbt.memory);
    Z_Free(shaderHandles);
    
    // Get SBT device address
    VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = rtxPipeline.sbt.buffer
    };
    // Use the RTX_GetBufferDeviceAddress helper from rt_rtx_impl.c
    rtxPipeline.sbt.deviceAddress = RTX_GetBufferDeviceAddress(rtxPipeline.sbt.buffer);
    
    // Setup strided device address regions
    rtxPipeline.sbt.raygenRegion = (VkStridedDeviceAddressRegionKHR){
        .deviceAddress = rtxPipeline.sbt.deviceAddress,
        .stride = rtxPipeline.sbt.handleSizeAligned,
        .size = rtxPipeline.sbt.handleSizeAligned
    };
    
    rtxPipeline.sbt.missRegion = (VkStridedDeviceAddressRegionKHR){
        .deviceAddress = rtxPipeline.sbt.deviceAddress + rtxPipeline.sbt.handleSizeAligned,
        .stride = rtxPipeline.sbt.handleSizeAligned,
        .size = rtxPipeline.sbt.handleSizeAligned * 2  // 2 miss shaders
    };
    
    rtxPipeline.sbt.hitRegion = (VkStridedDeviceAddressRegionKHR){
        .deviceAddress = rtxPipeline.sbt.deviceAddress + rtxPipeline.sbt.handleSizeAligned * 3,
        .stride = rtxPipeline.sbt.handleSizeAligned,
        .size = rtxPipeline.sbt.handleSizeAligned
    };
    
    rtxPipeline.sbt.callableRegion = (VkStridedDeviceAddressRegionKHR){0};
    
    ri.Printf(PRINT_ALL, "RTX: Shader binding table created (size: %u bytes)\n", sbtSize);
    return qtrue;
}

/*
================
RTX_InitializePipeline

Initialize entire RT pipeline system
================
*/
qboolean RTX_InitializePipeline(void) {
    if (!vk.device || !vk.physical_device) {
        ri.Printf(PRINT_WARNING, "RTX: Vulkan device not initialized\n");
        return qfalse;
    }
    
    // Create descriptor pool
    if (!RTX_CreateDescriptorPool(vk.device)) {
        return qfalse;
    }
    
    // Create RT pipeline
    if (!RTX_CreateRTPipeline(vk.device, vk.physical_device)) {
        return qfalse;
    }
    
    // Create shader binding table
    if (!RTX_CreateShaderBindingTable(vk.device, vk.physical_device)) {
        return qfalse;
    }
    
    // Allocate descriptor sets
    if (!RTX_AllocateDescriptorSets(vk.device)) {
        return qfalse;
    }
    
    // Create uniform buffers
    if (!RTX_CreateUniformBuffers(vk.device, vk.physical_device)) {
        return qfalse;
    }
    
    // Create storage buffers
    if (!RTX_CreateStorageBuffers(vk.device, vk.physical_device)) {
        return qfalse;
    }
    
    // Create texture sampler
    if (!RTX_CreateTextureSampler(vk.device)) {
        return qfalse;
    }
    
    ri.Printf(PRINT_ALL, "RTX: Pipeline system initialized successfully\n");
    return qtrue;
}

/*
================
RTX_ShutdownPipeline

Cleanup RT pipeline resources
================
*/
void RTX_ShutdownPipeline(void) {
    if (!vk.device) {
        return;
    }
    
    vkDeviceWaitIdle(vk.device);
    
    // Destroy shader modules
    if (rtxPipeline.shaders.raygenShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.raygenShader, NULL);
    }
    if (rtxPipeline.shaders.missShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.missShader, NULL);
    }
    if (rtxPipeline.shaders.shadowMissShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.shadowMissShader, NULL);
    }
    if (rtxPipeline.shaders.closestHitShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.closestHitShader, NULL);
    }
    
    // Destroy pipeline
    if (rtxPipeline.pipeline.pipeline) {
        vkDestroyPipeline(vk.device, rtxPipeline.pipeline.pipeline, NULL);
    }
    if (rtxPipeline.pipeline.pipelineLayout) {
        vkDestroyPipelineLayout(vk.device, rtxPipeline.pipeline.pipelineLayout, NULL);
    }
    if (rtxPipeline.pipeline.descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(vk.device, rtxPipeline.pipeline.descriptorSetLayout, NULL);
    }
    
    // Destroy SBT
    if (rtxPipeline.sbt.buffer) {
        vkDestroyBuffer(vk.device, rtxPipeline.sbt.buffer, NULL);
    }
    if (rtxPipeline.sbt.memory) {
        vkFreeMemory(vk.device, rtxPipeline.sbt.memory, NULL);
    }
    
    // Destroy descriptor pool
    if (rtxPipeline.descriptorPool) {
        vkDestroyDescriptorPool(vk.device, rtxPipeline.descriptorPool, NULL);
    }
    
    // Destroy uniform buffers
    if (rtxPipeline.cameraUBO) {
        vkDestroyBuffer(vk.device, rtxPipeline.cameraUBO, NULL);
        vkFreeMemory(vk.device, rtxPipeline.cameraUBOMemory, NULL);
    }
    if (rtxPipeline.renderSettingsUBO) {
        vkDestroyBuffer(vk.device, rtxPipeline.renderSettingsUBO, NULL);
        vkFreeMemory(vk.device, rtxPipeline.renderSettingsUBOMemory, NULL);
    }
    if (rtxPipeline.environmentUBO) {
        vkDestroyBuffer(vk.device, rtxPipeline.environmentUBO, NULL);
        vkFreeMemory(vk.device, rtxPipeline.environmentUBOMemory, NULL);
    }
    
    // Destroy storage buffers
    if (rtxPipeline.materialBuffer) {
        vkDestroyBuffer(vk.device, rtxPipeline.materialBuffer, NULL);
        vkFreeMemory(vk.device, rtxPipeline.materialBufferMemory, NULL);
    }
    if (rtxPipeline.lightBuffer) {
        vkDestroyBuffer(vk.device, rtxPipeline.lightBuffer, NULL);
        vkFreeMemory(vk.device, rtxPipeline.lightBufferMemory, NULL);
    }
    if (rtxPipeline.instanceDataBuffer) {
        vkDestroyBuffer(vk.device, rtxPipeline.instanceDataBuffer, NULL);
        vkFreeMemory(vk.device, rtxPipeline.instanceDataBufferMemory, NULL);
    }
    
    // Destroy sampler
    if (rtxPipeline.textureSampler) {
        vkDestroySampler(vk.device, rtxPipeline.textureSampler, NULL);
    }
    
    Com_Memset(&rtxPipeline, 0, sizeof(rtxPipeline));
    ri.Printf(PRINT_ALL, "RTX: Pipeline shutdown complete\n");
}

/*
================
RTX_GetPipeline

Get current RT pipeline
================
*/
VkPipeline RTX_GetPipeline(void) {
    return rtxPipeline.pipeline.pipeline;
}

/*
================
RTX_GetPipelineLayout

Get current RT pipeline layout
================
*/
VkPipelineLayout RTX_GetPipelineLayout(void) {
    return rtxPipeline.pipeline.pipelineLayout;
}

/*
================
RTX_GetDescriptorSet

Get current RT descriptor set
================
*/
VkDescriptorSet RTX_GetDescriptorSet(void) {
    return rtxPipeline.descriptorSet;
}

/*
================
RTX_GetSBTRegions

Get shader binding table regions for ray dispatch
================
*/
void RTX_GetSBTRegions(VkStridedDeviceAddressRegionKHR *raygen,
                      VkStridedDeviceAddressRegionKHR *miss,
                      VkStridedDeviceAddressRegionKHR *hit,
                      VkStridedDeviceAddressRegionKHR *callable) {
    *raygen = rtxPipeline.sbt.raygenRegion;
    *miss = rtxPipeline.sbt.missRegion;
    *hit = rtxPipeline.sbt.hitRegion;
    *callable = rtxPipeline.sbt.callableRegion;
}

/*
================
RTX_UpdateDescriptorSets

Update descriptor set bindings
================
*/
void RTX_UpdateDescriptorSets(VkAccelerationStructureKHR tlas,
                             VkImageView colorImage, VkImageView albedoImage,
                             VkImageView normalImage, VkImageView motionImage,
                             VkImageView depthImage) {
    VkWriteDescriptorSet writes[15];
    uint32_t writeCount = 0;
    
    // TLAS binding
    VkWriteDescriptorSetAccelerationStructureKHR tlasInfo = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &tlas
    };
    
    writes[writeCount++] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = &tlasInfo,
        .dstSet = rtxPipeline.descriptorSet,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
    };
    
    // Storage images
    VkDescriptorImageInfo imageInfos[5] = {
        { .imageView = colorImage, .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = albedoImage, .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = normalImage, .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = motionImage, .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = depthImage, .imageLayout = VK_IMAGE_LAYOUT_GENERAL }
    };
    
    for (uint32_t i = 0; i < 5; i++) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 1 + i,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &imageInfos[i]
        };
    }
    
    // Uniform buffers
    VkDescriptorBufferInfo bufferInfos[3] = {
        { .buffer = rtxPipeline.cameraUBO, .offset = 0, .range = sizeof(CameraUBO) },
        { .buffer = rtxPipeline.renderSettingsUBO, .offset = 0, .range = sizeof(RenderSettingsUBO) },
        { .buffer = rtxPipeline.environmentUBO, .offset = 0, .range = sizeof(EnvironmentUBO) }
    };
    
    for (uint32_t i = 0; i < 3; i++) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 6 + i,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &bufferInfos[i]
        };
    }
    
    // Storage buffers
    VkDescriptorBufferInfo storageBufferInfos[3] = {
        { .buffer = rtxPipeline.instanceDataBuffer, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = rtxPipeline.materialBuffer, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = rtxPipeline.lightBuffer, .offset = 0, .range = VK_WHOLE_SIZE }
    };
    
    writes[writeCount++] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = rtxPipeline.descriptorSet,
        .dstBinding = 10,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &storageBufferInfos[0]
    };
    
    writes[writeCount++] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = rtxPipeline.descriptorSet,
        .dstBinding = 11,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &storageBufferInfos[1]
    };
    
    writes[writeCount++] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = rtxPipeline.descriptorSet,
        .dstBinding = 14,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &storageBufferInfos[2]
    };
    
    vkUpdateDescriptorSets(vk.device, writeCount, writes, 0, NULL);
}

// vk_find_memory_type is already defined in vk.c and declared in vk.h
// No need to redefine it here