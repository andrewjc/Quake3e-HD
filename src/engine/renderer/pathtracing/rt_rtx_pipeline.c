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
#include "rt_volumefx.h"
#include "../core/tr_local.h"
#include "../core/tr_common_utils.h"
#include "../vulkan/vk.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>


// External RTX state
extern rtxState_t rtx;
extern cvar_t *r_rtx_surface_debug;
extern cvar_t *r_rtx_debug;
extern cvar_t *r_rtx_debugBlend;
extern VkBuffer RTX_GetMaterialBuffer(void);
extern pathTracer_t rt;

// Dummy fallback buffer to keep descriptors valid when real buffers are not ready
static VkBuffer rtxDummyBuffer = VK_NULL_HANDLE;
static VkDeviceMemory rtxDummyMemory = VK_NULL_HANDLE;

static void RTX_CreateDummyBuffer(void) {
    if (rtxDummyBuffer != VK_NULL_HANDLE) {
        return;
    }

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateBuffer(vk.device, &bufferInfo, NULL, &rtxDummyBuffer) != VK_SUCCESS) {
        rtxDummyBuffer = VK_NULL_HANDLE;
        return;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, rtxDummyBuffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &allocFlags,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };

    if (vkAllocateMemory(vk.device, &allocInfo, NULL, &rtxDummyMemory) != VK_SUCCESS) {
        vkDestroyBuffer(vk.device, rtxDummyBuffer, NULL);
        rtxDummyBuffer = VK_NULL_HANDLE;
        return;
    }

    vkBindBufferMemory(vk.device, rtxDummyBuffer, rtxDummyMemory, 0);
}

// Pipeline management structures
typedef struct {
    VkShaderModule raygenShader;
    VkShaderModule missShader;
    VkShaderModule shadowMissShader;
    VkShaderModule closestHitShader;
    VkShaderModule rayQueryShader;
} rtxShaders_t;

typedef struct {
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    VkPipeline rayQueryPipeline;
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

typedef struct {
    float origin[4];
    float direction[4];
    uint32_t occluded;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
} rtxShadowQueryGpu_t;

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
    uint32_t surfaceDebugMode;
    uint32_t _padSurfaceDebug[3];
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
    vec4_t lightGridOrigin;      // xyz = origin, w unused
    vec4_t lightGridCellSize;    // xyz = cell size, w unused
    vec4_t lightGridInvCellSize; // xyz = inverse cell size, w unused
    uint32_t lightGridDims[4];   // x, y, z, cellCount
    uint32_t lightGridCounts[4]; // directionalCount, offsetCount, indexCount, reserved
    vec4_t skyAmbient;           // rgb = ambient color, a = intensity
    vec4_t volumetricParams;     // x = density/unit, y = anisotropy g, z = max march dist, w = enable
    vec4_t featureParams;        // x = caustics enable, y = caustic intensity, z = in-scatter scale, w = reflections
    vec4_t shadowParams;         // x = soft shadows, y = point light radius scale, z = sun angular radius, w = spare
} RenderSettingsUBO;

// Debug options
typedef struct {
    uint32_t noTextures;
    uint32_t debugMode;
    float    debugOverlayBlend;
    uint32_t debugFlags;
} DebugSettingsUBO;

// Volumetric weapon effects (std140: the count cell pads to 16 bytes, each
// instance is three vec4s)
typedef struct {
    uint32_t count;
    uint32_t _padCount[3];
    rtVolumeFxGpu_t fx[RT_MAX_VOLUME_FX];
} VolumeFXUBO;


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
    float _padSkyColor[2];  // std140: vec4 below aligns to a 16-byte boundary
    float skyColor[4];      // rgb = map sky average color, a = brightness
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

// Global pipeline state
static struct {
    rtxShaders_t shaders;
    rtxPipelineInfo_t pipeline;
    rtxSBT_t sbt;
    
    // Descriptor resources
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSet;
    qboolean descriptorSetReady;
    
    // Uniform buffers
    VkBuffer cameraUBO;
    VkDeviceMemory cameraUBOMemory;
    VkBuffer renderSettingsUBO;
    VkDeviceMemory renderSettingsUBOMemory;
    VkBuffer environmentUBO;
    VkDeviceMemory environmentUBOMemory;
    VkBuffer debugSettingsUBO;
    VkDeviceMemory debugSettingsUBOMemory;
    VkBuffer volumeFXUBO;
    VkDeviceMemory volumeFXUBOMemory;

    // Storage buffers
    VkBuffer instanceDataBuffer;
    VkDeviceMemory instanceDataBufferMemory;
    VkBuffer triangleMaterialBuffer;
    VkDeviceMemory triangleMaterialBufferMemory;
    uint32_t triangleMaterialCount;
    uint32_t triangleMaterialCapacity;
    VkBuffer rayQueryBuffer;
    VkDeviceMemory rayQueryBufferMemory;
    rtxShadowQueryGpu_t *rayQueryMapped;
    uint32_t rayQueryCapacity;
    
    // Texture arrays
    VkSampler textureSampler;
    uint32_t textureCount;
    VkImageView *textureViews;
    uint32_t activeInstances;
    
    // RT properties
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties;
} rtxPipeline;

// Function pointers for RT pipeline
static PFN_vkGetRayTracingShaderGroupHandlesKHR qvkGetRayTracingShaderGroupHandlesKHR;
static PFN_vkCmdTraceRaysKHR qvkCmdTraceRaysKHR;

static qboolean RTX_CreateRayQueryPipeline(VkDevice device);
static void RTX_DestroyRayQueryPipeline(void);
static void RTX_DestroyRayQueryBuffer(void);
static qboolean RTX_EnsureRayQueryCapacity(uint32_t count);

static void RTX_DestroyDescriptorSetLayoutSafe(VkDescriptorSetLayout *layout) {
    if (!layout || *layout == VK_NULL_HANDLE) {
        return;
    }

    if (qvkDestroyDescriptorSetLayout) {
        vkDestroyDescriptorSetLayout(vk.device, *layout, NULL);
    } else {
        ri.Printf(PRINT_WARNING, "RTX_ShutdownPipeline: qvkDestroyDescriptorSetLayout missing, skipping destroy\n");
    }

    *layout = VK_NULL_HANDLE;
}

/*
================
RTX_LoadShaderModule

Load compiled SPIR-V shader from disk
================
*/
static VkShaderModule RTX_LoadShaderModule(VkDevice device, const char *filename) {
    byte *shaderCode;
    int fileSize;
    VkShaderModule module = VK_NULL_HANDLE;
    
    // Attempt to load from RTX and compute shader directories
    const char *searchPaths[] = {
        "shaders/rtx/%s",
        "shaders/compute/%s"
    };
    char fullPath[MAX_QPATH];
    shaderCode = NULL;
    fileSize = 0;

    for (int i = 0; i < ARRAY_LEN(searchPaths); i++) {
        Com_sprintf(fullPath, sizeof(fullPath), searchPaths[i], filename);
        ri.Printf(PRINT_ALL, "RTX: Attempting to load shader: %s\n", fullPath);
        fileSize = ri.FS_ReadFile(fullPath, (void **)&shaderCode);
        if (fileSize > 0 && shaderCode) {
            ri.Printf(PRINT_ALL, "RTX: Successfully read %d bytes from %s\n", fileSize, filename);
            break;
        }
        if (shaderCode) {
            ri.FS_FreeFile(shaderCode);
            shaderCode = NULL;
        }
    }

    if (fileSize <= 0 || !shaderCode) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to open shader file: %s\n", filename);
        return VK_NULL_HANDLE;
    }
    
    // Create shader module
    VkShaderModuleCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fileSize,
        .pCode = (const uint32_t *)shaderCode
    };
    
    VkResult result = vkCreateShaderModule(device, &createInfo, NULL, &module);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: vkCreateShaderModule failed for %s (VkResult: %d, codeSize: %d)\n",
                  filename, result, fileSize);
        module = VK_NULL_HANDLE;
    } else {
        ri.Printf(PRINT_ALL, "RTX: Successfully created shader module for %s (%d bytes, handle=%p)\n",
                  filename, fileSize, (void*)module);
    }
    
    ri.FS_FreeFile(shaderCode);
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
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT
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
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT
        },
        // Binding 7: Render settings UBO
        {
            .binding = 7,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT
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
            .stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT
        },
        // Binding 10: Instance data buffer
        {
            .binding = 10,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT
        },
        // Binding 11: Material buffer
        {
            .binding = 11,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT
        },
        // Binding 12: Texture array
        {
            .binding = 12,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = RTX_MAX_TEXTURES,
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        },
        // Binding 13: Volumetric weapon effects UBO
        {
            .binding = 13,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 14: Light buffer
        {
            .binding = 14,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT
        },
        // Binding 15: Direct light contribution image
        {
            .binding = 15,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 16: Indirect light contribution image
        {
            .binding = 16,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 17: Media image (volumetric in-scatter rgb + transmittance
        // a along the primary ray; the composite applies it to raster pixels)
        {
            .binding = 17,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        {
            .binding = 18,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding = 19,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding = 20,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT |
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 21: Light grid offsets
        {
            .binding = 21,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR
        },
        // Binding 22: Light grid indices
        {
            .binding = 22,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                          VK_SHADER_STAGE_RAYGEN_BIT_KHR
        }
    };
    
    // All bindings are update-after-bind: the single descriptor set stays
    // referenced by in-flight frame command buffers, and resource rebinds
    // (TLAS rebuild, resize, texture registration) update it after a queue
    // drain. Without these flags such updates violate
    // VUID-vkUpdateDescriptorSets-None-03047.
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = ARRAY_LEN(bindings)
    };

    VkDescriptorBindingFlags flags[ARRAY_LEN(bindings)] = {0};
    for (uint32_t i = 0; i < ARRAY_LEN(bindings); ++i) {
        // Uniform buffers stay flagless: NVIDIA lacks uniform-buffer
        // update-after-bind support, and those bindings are written once at
        // init and never rebound afterwards.
        if (bindings[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                       VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        }
        if (bindings[i].binding == 12 || bindings[i].binding == 20) {
            flags[i] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }
    }
    bindingFlags.pBindingFlags = flags;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bindingFlags,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
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
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RTX_MAX_TEXTURES + 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 }
    };
    
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT |
                 VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
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
    // TRANSFER_DST is required because per-frame uniform updates are recorded
    // into the frame command buffer via vkCmdUpdateBuffer so they execute in
    // order with the ray dispatch instead of racing frames in flight.
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(CameraUBO),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
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
    
    // Debug settings UBO
    bufferInfo.size = sizeof(DebugSettingsUBO);
    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.debugSettingsUBO) != VK_SUCCESS) {
        return qfalse;
    }

    vkGetBufferMemoryRequirements(device, rtxPipeline.debugSettingsUBO, &memReqs);
    allocInfo.allocationSize = memReqs.size;

    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.debugSettingsUBOMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.debugSettingsUBO, NULL);
        return qfalse;
    }

    vkBindBufferMemory(device, rtxPipeline.debugSettingsUBO, rtxPipeline.debugSettingsUBOMemory, 0);

    // Volumetric weapon effects UBO
    bufferInfo.size = sizeof(VolumeFXUBO);
    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.volumeFXUBO) != VK_SUCCESS) {
        return qfalse;
    }

    vkGetBufferMemoryRequirements(device, rtxPipeline.volumeFXUBO, &memReqs);
    allocInfo.allocationSize = memReqs.size;

    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.volumeFXUBOMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.volumeFXUBO, NULL);
        return qfalse;
    }

    vkBindBufferMemory(device, rtxPipeline.volumeFXUBO, rtxPipeline.volumeFXUBOMemory, 0);

    return qtrue;
}

/*
================
RTX_CreateStorageBuffers

Create storage buffers for materials, lights, and instance data
================
*/
static qboolean RTX_CreateStorageBuffers(VkDevice device, VkPhysicalDevice physicalDevice) {
    (void)physicalDevice;
    VkDeviceSize instanceBufferSize = sizeof(rtxInstanceGpuData_t) * RTX_MAX_INSTANCES;
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = instanceBufferSize,
        // Device address is required when queried for SBT/instance builds.
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateBuffer(device, &bufferInfo, NULL, &rtxPipeline.instanceDataBuffer) != VK_SUCCESS) {
        return qfalse;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Created instance data buffer %p (%llu bytes)\n",
                  (void*)rtxPipeline.instanceDataBuffer,
                  (unsigned long long)instanceBufferSize);
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, rtxPipeline.instanceDataBuffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &allocFlags,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    if (vkAllocateMemory(device, &allocInfo, NULL, &rtxPipeline.instanceDataBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, rtxPipeline.instanceDataBuffer, NULL);
        rtxPipeline.instanceDataBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    vkBindBufferMemory(device, rtxPipeline.instanceDataBuffer, rtxPipeline.instanceDataBufferMemory, 0);

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Bound instance data memory %p\n",
                  (void*)rtxPipeline.instanceDataBufferMemory);
    }

    void *mapped = NULL;
    if (vkMapMemory(device, rtxPipeline.instanceDataBufferMemory, 0, instanceBufferSize, 0, &mapped) == VK_SUCCESS) {
        Com_Memset(mapped, 0, (size_t)instanceBufferSize);
        vkUnmapMemory(device, rtxPipeline.instanceDataBufferMemory);
    }

    rtxPipeline.activeInstances = 0;

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
    PFN_vkCreateRayTracingPipelinesKHR createPipelines = (PFN_vkCreateRayTracingPipelinesKHR)
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR");
    qvkGetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR)
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR");
    qvkCmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)
        vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR");
    
    if (!createPipelines || !qvkGetRayTracingShaderGroupHandlesKHR || !qvkCmdTraceRaysKHR) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to load RT pipeline function pointers\n");
        return qfalse;
    }
    vk_register_ray_tracing_pipeline_dispatch(createPipelines);
    
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
    
    // Create pipeline layout (shared between RT and compute pipelines)
    VkPushConstantRange pushRange = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(uint32_t)
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &rtxPipeline.pipeline.descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange
    };
    
    result = vkCreatePipelineLayout(device, &pipelineLayoutInfo, NULL, 
                                    &rtxPipeline.pipeline.pipelineLayout);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create pipeline layout (result: %d)\n", result);
        return qfalse;
    }
    
    // Create ray tracing pipeline. Bounces are iterated in raygen (depth 1),
    // but closest-hit traces shadow rays, so the pipeline needs recursion
    // depth 2; one extra level of headroom avoids driver-side accounting
    // faults observed when hit shaders of secondary rays trace.
    int reqRecursion = 3;
    if (rtxPipeline.rtProperties.maxRayRecursionDepth > 0 && reqRecursion > (int)rtxPipeline.rtProperties.maxRayRecursionDepth)
        reqRecursion = (int)rtxPipeline.rtProperties.maxRayRecursionDepth;
    VkRayTracingPipelineCreateInfoKHR pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
        .stageCount = ARRAY_LEN(shaderStages),
        .pStages = shaderStages,
        .groupCount = ARRAY_LEN(shaderGroups),
        .pGroups = shaderGroups,
        .maxPipelineRayRecursionDepth = (uint32_t)reqRecursion,
        .layout = rtxPipeline.pipeline.pipelineLayout
    };
    
    ri.Printf(PRINT_ALL, "RTX: Creating ray tracing pipeline with %d stages, %d groups, max recursion %d\n",
              pipelineInfo.stageCount, pipelineInfo.groupCount, pipelineInfo.maxPipelineRayRecursionDepth);

    result = qvkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
                                             &pipelineInfo, NULL, &rtxPipeline.pipeline.pipeline);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: vkCreateRayTracingPipelinesKHR failed with VkResult %d\n", result);
        // Add more detailed error info
        switch(result) {
            case VK_ERROR_OUT_OF_HOST_MEMORY:
                ri.Printf(PRINT_WARNING, "RTX: Out of host memory\n");
                break;
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:
                ri.Printf(PRINT_WARNING, "RTX: Out of device memory\n");
                break;
            case VK_ERROR_INVALID_SHADER_NV:
                ri.Printf(PRINT_WARNING, "RTX: Invalid shader\n");
                break;
            default:
                ri.Printf(PRINT_WARNING, "RTX: Unknown error\n");
                break;
        }
        return qfalse;
    }

    ri.Printf(PRINT_ALL, "RTX: Ray tracing pipeline created successfully (handle=%p)\n",
              (void*)rtxPipeline.pipeline.pipeline);

    if (!RTX_CreateRayQueryPipeline(device)) {
        ri.Printf(PRINT_WARNING, "RTX: Ray query compute pipeline not available; CPU fallback will be used\n");
    }

    return qtrue;
}

static qboolean RTX_CreateRayQueryPipeline(VkDevice device) {
    if (!(rtx.features & RTX_FEATURE_RAY_QUERY)) {
        return qtrue;
    }

    if (rtxPipeline.pipeline.rayQueryPipeline != VK_NULL_HANDLE) {
        return qtrue;
    }

    if (!rtxPipeline.shaders.rayQueryShader) {
        rtxPipeline.shaders.rayQueryShader = RTX_LoadShaderModule(device, "shadow_queries.spv");
        if (!rtxPipeline.shaders.rayQueryShader) {
            ri.Printf(PRINT_WARNING, "RTX: Failed to load shadow query shader module\n");
            return qfalse;
        }
    }

    VkPipelineShaderStageCreateInfo stageInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = rtxPipeline.shaders.rayQueryShader,
        .pName = "main"
    };

    VkComputePipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stageInfo,
        .layout = rtxPipeline.pipeline.pipelineLayout
    };

    VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                               NULL, &rtxPipeline.pipeline.rayQueryPipeline);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create ray query compute pipeline (result: %d)\n", result);
        if (rtxPipeline.pipeline.rayQueryPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, rtxPipeline.pipeline.rayQueryPipeline, NULL);
            rtxPipeline.pipeline.rayQueryPipeline = VK_NULL_HANDLE;
        }
        return qfalse;
    }

    return qtrue;
}

static void RTX_DestroyRayQueryPipeline(void) {
    if (rtxPipeline.pipeline.rayQueryPipeline) {
        vkDestroyPipeline(vk.device, rtxPipeline.pipeline.rayQueryPipeline, NULL);
        rtxPipeline.pipeline.rayQueryPipeline = VK_NULL_HANDLE;
    }
    if (rtxPipeline.shaders.rayQueryShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.rayQueryShader, NULL);
        rtxPipeline.shaders.rayQueryShader = VK_NULL_HANDLE;
    }
}

static void RTX_DestroyRayQueryBuffer(void) {
    if (rtxPipeline.rayQueryMapped && rtxPipeline.rayQueryBufferMemory) {
        vkUnmapMemory(vk.device, rtxPipeline.rayQueryBufferMemory);
        rtxPipeline.rayQueryMapped = NULL;
    }

    if (rtxPipeline.rayQueryBuffer) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Destroying ray query buffer %p\n",
                      (void*)rtxPipeline.rayQueryBuffer);
        }
        vkDestroyBuffer(vk.device, rtxPipeline.rayQueryBuffer, NULL);
        rtxPipeline.rayQueryBuffer = VK_NULL_HANDLE;
    }
    if (rtxPipeline.rayQueryBufferMemory) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Freeing ray query memory %p\n",
                      (void*)rtxPipeline.rayQueryBufferMemory);
        }
        vkFreeMemory(vk.device, rtxPipeline.rayQueryBufferMemory, NULL);
        rtxPipeline.rayQueryBufferMemory = VK_NULL_HANDLE;
    }
    rtxPipeline.rayQueryCapacity = 0;
}

static qboolean RTX_EnsureRayQueryCapacity(uint32_t count) {
    if (!(rtx.features & RTX_FEATURE_RAY_QUERY)) {
        return qfalse;
    }

    if (count == 0) {
        return qtrue;
    }

    if (rtxPipeline.rayQueryCapacity >= count && rtxPipeline.rayQueryBuffer != VK_NULL_HANDLE) {
        return qtrue;
    }

    uint32_t newCapacity = rtxPipeline.rayQueryCapacity ? rtxPipeline.rayQueryCapacity : 64;
    while (newCapacity < count) {
        newCapacity = (newCapacity < UINT32_MAX / 2) ? newCapacity * 2 : count;
        if (newCapacity >= count) {
            break;
        }
    }

    RTX_DestroyRayQueryBuffer();

    VkDeviceSize bufferSize = sizeof(rtxShadowQueryGpu_t) * (VkDeviceSize)newCapacity;

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bufferSize,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateBuffer(vk.device, &bufferInfo, NULL, &rtxPipeline.rayQueryBuffer) != VK_SUCCESS) {
        return qfalse;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Created ray query buffer %p (%llu bytes)\n",
                  (void*)rtxPipeline.rayQueryBuffer,
                  (unsigned long long)bufferSize);
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, rtxPipeline.rayQueryBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    if (vkAllocateMemory(vk.device, &allocInfo, NULL, &rtxPipeline.rayQueryBufferMemory) != VK_SUCCESS) {
        vkDestroyBuffer(vk.device, rtxPipeline.rayQueryBuffer, NULL);
        rtxPipeline.rayQueryBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    vkBindBufferMemory(vk.device, rtxPipeline.rayQueryBuffer, rtxPipeline.rayQueryBufferMemory, 0);

    if (vkMapMemory(vk.device, rtxPipeline.rayQueryBufferMemory, 0, VK_WHOLE_SIZE, 0,
                    (void**)&rtxPipeline.rayQueryMapped) != VK_SUCCESS) {
        RTX_DestroyRayQueryBuffer();
        return qfalse;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Ray query memory %p mapped (capacity=%u)\n",
                  (void*)rtxPipeline.rayQueryBufferMemory,
                  rtxPipeline.rayQueryCapacity);
    }

    rtxPipeline.rayQueryCapacity = newCapacity;

    VkDescriptorBufferInfo bufferDesc = {
        .buffer = rtxPipeline.rayQueryBuffer,
        .offset = 0,
        .range = bufferSize
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = rtxPipeline.descriptorSet,
        .dstBinding = 19,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &bufferDesc
    };

    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

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
    // Ensure handleSizeAligned is at least baseAlignment to maintain alignment for all regions
    rtxPipeline.sbt.handleSizeAligned = (handleSize + handleAlignment - 1) & ~(handleAlignment - 1);
    if (rtxPipeline.sbt.handleSizeAligned < baseAlignment) {
        rtxPipeline.sbt.handleSizeAligned = baseAlignment;
    }
    rtxPipeline.sbt.groupCount = 4; // raygen, miss, shadow miss, hit
    
    ri.Printf(PRINT_ALL, "RTX: SBT Alignment - handleSize: %u, handleAlignment: %u, baseAlignment: %u, handleSizeAligned: %u\n",
              handleSize, handleAlignment, baseAlignment, rtxPipeline.sbt.handleSizeAligned);
    
    // Calculate SBT buffer size
    uint32_t sbtSize = rtxPipeline.sbt.groupCount * rtxPipeline.sbt.handleSizeAligned;
    sbtSize = (sbtSize + baseAlignment - 1) & ~(baseAlignment - 1);

    // Add guard regions before/after to catch stray writes and give the driver
    // some slack if it oversteps the recorded size.  Align the guard to the
    // SBT alignment rules so device addresses stay valid.
    VkDeviceSize guardSize = 4096;
    VkDeviceSize alignMask = (VkDeviceSize)baseAlignment - 1;
    guardSize = (guardSize + alignMask) & ~alignMask;

    VkDeviceSize totalSize = sbtSize + guardSize * 2;
    
    // Create SBT buffer
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = totalSize,
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
    
    // Ensure allocation is large enough to allow alignment
    VkDeviceSize alignedSize = memReqs.size + baseAlignment;
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &memoryAllocateFlagsInfo,
        // memReqs.size already reflects totalSize; pad slightly for safety
        .allocationSize = alignedSize,
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
    result = vkMapMemory(device, rtxPipeline.sbt.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to map SBT memory (result: %d)\n", result);
        Z_Free(shaderHandles);
        return qfalse;
    }
    
    // Copy shader handles with proper alignment
    uint8_t *pData = (uint8_t*)mapped + guardSize;
    for (uint32_t i = 0; i < rtxPipeline.sbt.groupCount; i++) {
        Com_Memcpy(pData + i * rtxPipeline.sbt.handleSizeAligned, 
            shaderHandles + i * handleSize, handleSize);
    }
    
    vkUnmapMemory(device, rtxPipeline.sbt.memory);
    Z_Free(shaderHandles);
    
    // Get the buffer's device address
    VkDeviceAddress rawAddress = RTX_GetBufferDeviceAddressVK(rtxPipeline.sbt.buffer);
    if (!rawAddress) rawAddress = RTX_GetBufferDeviceAddress(rtxPipeline.sbt.buffer);
    
    // Since the buffer is already sized and aligned properly, use the raw address with guard offset
    rtxPipeline.sbt.deviceAddress = rawAddress + guardSize;
    
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
    
    ri.Printf(PRINT_ALL, "RTX: SBT Addresses - base: 0x%llx, raygen: 0x%llx, miss: 0x%llx, hit: 0x%llx\n",
              (unsigned long long)rtxPipeline.sbt.deviceAddress,
              (unsigned long long)rtxPipeline.sbt.raygenRegion.deviceAddress,
              (unsigned long long)rtxPipeline.sbt.missRegion.deviceAddress,
              (unsigned long long)rtxPipeline.sbt.hitRegion.deviceAddress);
    ri.Printf(PRINT_ALL, "RTX: Miss address %% 64 = %llu\n", 
              (unsigned long long)(rtxPipeline.sbt.missRegion.deviceAddress % 64));
    
    rtxPipeline.sbt.callableRegion = (VkStridedDeviceAddressRegionKHR){0};

    ri.Printf(PRINT_ALL,
              "RTX: SBT buffer total=%llu bytes guard=%llu base=0x%llx raygen=0x%llx\n",
              (unsigned long long)totalSize,
              (unsigned long long)guardSize,
              (unsigned long long)rtxPipeline.sbt.deviceAddress,
              (unsigned long long)rtxPipeline.sbt.raygenRegion.deviceAddress);
    
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
    // Check if already initialized
    if (rtxPipeline.pipeline.pipeline != VK_NULL_HANDLE) {
        ri.Printf(PRINT_ALL, "RTX: Pipeline already initialized, skipping re-initialization\n");
        return qtrue;
    }

    ri.Printf(PRINT_ALL, "RTX: Initializing RT pipeline for the first time\n");

    if (!vk.device || !vk.physical_device) {
        ri.Printf(PRINT_WARNING, "RTX: Vulkan device not initialized\n");
        return qfalse;
    }

    // Create descriptor pool
    ri.Printf(PRINT_ALL, "RTX: Creating descriptor pool...\n");
    if (!RTX_CreateDescriptorPool(vk.device)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create descriptor pool\n");
        return qfalse;
    }
    ri.Printf(PRINT_ALL, "RTX: Descriptor pool created successfully\n");

    // Create RT pipeline
    ri.Printf(PRINT_ALL, "RTX: Creating RT pipeline...\n");
    if (!RTX_CreateRTPipeline(vk.device, vk.physical_device)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create RT pipeline\n");
        return qfalse;
    }
    ri.Printf(PRINT_ALL, "RTX: RT pipeline created successfully (handle=%p)\n", (void*)rtxPipeline.pipeline.pipeline);

    // Create shader binding table
    ri.Printf(PRINT_ALL, "RTX: Creating shader binding table...\n");
    if (!RTX_CreateShaderBindingTable(vk.device, vk.physical_device)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create shader binding table\n");
        return qfalse;
    }
    ri.Printf(PRINT_ALL, "RTX: Shader binding table created successfully\n");

    // Allocate descriptor sets
    ri.Printf(PRINT_ALL, "RTX: Allocating descriptor sets...\n");
    if (!RTX_AllocateDescriptorSets(vk.device)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to allocate descriptor sets\n");
        return qfalse;
    }
    ri.Printf(PRINT_ALL, "RTX: Descriptor sets allocated successfully\n");

    // Create uniform buffers
    ri.Printf(PRINT_ALL, "RTX: Creating uniform buffers...\n");
    if (!RTX_CreateUniformBuffers(vk.device, vk.physical_device)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create uniform buffers\n");
        return qfalse;
    }
    ri.Printf(PRINT_ALL, "RTX: Uniform buffers created successfully\n");

    // Create storage buffers
    ri.Printf(PRINT_ALL, "RTX: Creating storage buffers...\n");
    if (!RTX_CreateStorageBuffers(vk.device, vk.physical_device)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create storage buffers\n");
        return qfalse;
    }
    ri.Printf(PRINT_ALL, "RTX: Storage buffers created successfully\n");

    // Create texture sampler
    ri.Printf(PRINT_ALL, "RTX: Creating texture sampler...\n");
    if (!RTX_CreateTextureSampler(vk.device)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create texture sampler\n");
        return qfalse;
    }
    ri.Printf(PRINT_ALL, "RTX: Texture sampler created successfully\n");
    
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

    RTX_DestroyRayQueryPipeline();
    RTX_DestroyRayQueryBuffer();
    
    // Destroy shader modules
    if (rtxPipeline.shaders.raygenShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.raygenShader, NULL);
        rtxPipeline.shaders.raygenShader = VK_NULL_HANDLE;
    }
    if (rtxPipeline.shaders.missShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.missShader, NULL);
        rtxPipeline.shaders.missShader = VK_NULL_HANDLE;
    }
    if (rtxPipeline.shaders.shadowMissShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.shadowMissShader, NULL);
        rtxPipeline.shaders.shadowMissShader = VK_NULL_HANDLE;
    }
    if (rtxPipeline.shaders.closestHitShader) {
        vkDestroyShaderModule(vk.device, rtxPipeline.shaders.closestHitShader, NULL);
        rtxPipeline.shaders.closestHitShader = VK_NULL_HANDLE;
    }
    
    // Destroy pipeline
    if (rtxPipeline.pipeline.pipeline) {
        vkDestroyPipeline(vk.device, rtxPipeline.pipeline.pipeline, NULL);
        rtxPipeline.pipeline.pipeline = VK_NULL_HANDLE;
    }
    if (rtxPipeline.pipeline.pipelineLayout) {
        vkDestroyPipelineLayout(vk.device, rtxPipeline.pipeline.pipelineLayout, NULL);
        rtxPipeline.pipeline.pipelineLayout = VK_NULL_HANDLE;
    }
    RTX_DestroyDescriptorSetLayoutSafe(&rtxPipeline.pipeline.descriptorSetLayout);
    
    // Destroy SBT
    if (rtxPipeline.sbt.buffer) {
        vkDestroyBuffer(vk.device, rtxPipeline.sbt.buffer, NULL);
        rtxPipeline.sbt.buffer = VK_NULL_HANDLE;
    }
    if (rtxPipeline.sbt.memory) {
        vkFreeMemory(vk.device, rtxPipeline.sbt.memory, NULL);
        rtxPipeline.sbt.memory = VK_NULL_HANDLE;
    }
    
    // Destroy descriptor pool
    if (rtxPipeline.descriptorPool) {
        vkDestroyDescriptorPool(vk.device, rtxPipeline.descriptorPool, NULL);
        rtxPipeline.descriptorPool = VK_NULL_HANDLE;
    }
    if (rtxDummyBuffer) {
        vkDestroyBuffer(vk.device, rtxDummyBuffer, NULL);
        rtxDummyBuffer = VK_NULL_HANDLE;
    }
    if (rtxDummyMemory) {
        vkFreeMemory(vk.device, rtxDummyMemory, NULL);
        rtxDummyMemory = VK_NULL_HANDLE;
    }
    
    // Destroy uniform buffers
    if (rtxPipeline.cameraUBO) {
        vkDestroyBuffer(vk.device, rtxPipeline.cameraUBO, NULL);
        vkFreeMemory(vk.device, rtxPipeline.cameraUBOMemory, NULL);
        rtxPipeline.cameraUBO = VK_NULL_HANDLE;
        rtxPipeline.cameraUBOMemory = VK_NULL_HANDLE;
    }
    if (rtxPipeline.renderSettingsUBO) {
        vkDestroyBuffer(vk.device, rtxPipeline.renderSettingsUBO, NULL);
        vkFreeMemory(vk.device, rtxPipeline.renderSettingsUBOMemory, NULL);
        rtxPipeline.renderSettingsUBO = VK_NULL_HANDLE;
        rtxPipeline.renderSettingsUBOMemory = VK_NULL_HANDLE;
    }
    if (rtxPipeline.environmentUBO) {
        vkDestroyBuffer(vk.device, rtxPipeline.environmentUBO, NULL);
        vkFreeMemory(vk.device, rtxPipeline.environmentUBOMemory, NULL);
        rtxPipeline.environmentUBO = VK_NULL_HANDLE;
        rtxPipeline.environmentUBOMemory = VK_NULL_HANDLE;
    }
    // debugSettingsUBO (binding 18) and volumeFXUBO (binding 13) were omitted
    // here, so the following Com_Memset orphaned them — a buffer+allocation
    // leaked on every RTX reinitialisation (VUID-vkDestroyDevice-device-05137).
    if (rtxPipeline.debugSettingsUBO) {
        vkDestroyBuffer(vk.device, rtxPipeline.debugSettingsUBO, NULL);
        vkFreeMemory(vk.device, rtxPipeline.debugSettingsUBOMemory, NULL);
        rtxPipeline.debugSettingsUBO = VK_NULL_HANDLE;
        rtxPipeline.debugSettingsUBOMemory = VK_NULL_HANDLE;
    }
    if (rtxPipeline.volumeFXUBO) {
        vkDestroyBuffer(vk.device, rtxPipeline.volumeFXUBO, NULL);
        vkFreeMemory(vk.device, rtxPipeline.volumeFXUBOMemory, NULL);
        rtxPipeline.volumeFXUBO = VK_NULL_HANDLE;
        rtxPipeline.volumeFXUBOMemory = VK_NULL_HANDLE;
    }

    // Destroy storage buffers
    if (rtxPipeline.instanceDataBuffer) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Destroying instance data buffer %p\n",
                      (void*)rtxPipeline.instanceDataBuffer);
        }
        vkDestroyBuffer(vk.device, rtxPipeline.instanceDataBuffer, NULL);
        vkFreeMemory(vk.device, rtxPipeline.instanceDataBufferMemory, NULL);
        rtxPipeline.instanceDataBuffer = VK_NULL_HANDLE;
        rtxPipeline.instanceDataBufferMemory = VK_NULL_HANDLE;
    }
    if (rtxPipeline.triangleMaterialBuffer) {
        vkDestroyBuffer(vk.device, rtxPipeline.triangleMaterialBuffer, NULL);
        rtxPipeline.triangleMaterialBuffer = VK_NULL_HANDLE;
    }
    if (rtxPipeline.triangleMaterialBufferMemory) {
        vkFreeMemory(vk.device, rtxPipeline.triangleMaterialBufferMemory, NULL);
        rtxPipeline.triangleMaterialBufferMemory = VK_NULL_HANDLE;
    }
    rtxPipeline.triangleMaterialCount = 0;
    rtxPipeline.triangleMaterialCapacity = 0;
    // (rayQueryBuffer is released above by RTX_DestroyRayQueryBuffer().)

    // Destroy sampler
    if (rtxPipeline.textureSampler) {
        vkDestroySampler(vk.device, rtxPipeline.textureSampler, NULL);
        rtxPipeline.textureSampler = VK_NULL_HANDLE;
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

VkPipeline RTX_GetRayQueryPipelineHandle(void) {
    return rtxPipeline.pipeline.rayQueryPipeline;
}

VkBuffer RTX_RayQueryGetBuffer(void) {
    return rtxPipeline.rayQueryBuffer;
}

VkDeviceSize RTX_RayQueryRecordSize(void) {
    return sizeof(rtxShadowQueryGpu_t);
}

qboolean RTX_RayQueryUpload(const rtShadowQuery_t *queries, int count) {
    if (!queries || count <= 0) {
        return qfalse;
    }

    if (!RTX_EnsureRayQueryCapacity((uint32_t)count)) {
        return qfalse;
    }

    if (!rtxPipeline.rayQueryMapped) {
        return qfalse;
    }

    rtxShadowQueryGpu_t *dst = rtxPipeline.rayQueryMapped;
    for (int i = 0; i < count; i++) {
        dst[i].origin[0] = queries[i].origin[0];
        dst[i].origin[1] = queries[i].origin[1];
        dst[i].origin[2] = queries[i].origin[2];
        dst[i].origin[3] = 1.0f;

        dst[i].direction[0] = queries[i].direction[0];
        dst[i].direction[1] = queries[i].direction[1];
        dst[i].direction[2] = queries[i].direction[2];
        dst[i].direction[3] = queries[i].maxDistance;

        dst[i].occluded = 0;
        dst[i].pad0 = 0;
        dst[i].pad1 = 0;
        dst[i].pad2 = 0;
    }

    return qtrue;
}

void RTX_RayQueryDownload(rtShadowQuery_t *queries, int count) {
    if (!queries || count <= 0 || !rtxPipeline.rayQueryMapped) {
        return;
    }

    rtxShadowQueryGpu_t *src = rtxPipeline.rayQueryMapped;
    for (int i = 0; i < count; i++) {
        queries[i].occluded = src[i].occluded ? qtrue : qfalse;
    }
}

/*
================
RTX_PrepareFrameData

Update per-frame UBOs and GPU buffers (materials, lights, instance data)
================
*/
// Saved 3D scene viewParms — captured during RB_DrawSurfs, used at end-of-frame
static viewParms_t  rtxSavedViewParms;
static trRefdef_t   rtxSavedRefdef;
static qboolean     rtxHasValidViewParms = qfalse;

qboolean RTX_HasValidViewParms(void) { return rtxHasValidViewParms; }

void RTX_ResetViewParms(void) { rtxHasValidViewParms = qfalse; }

// Camera planes captured by the most recent RTX_PrepareFrameData; consumed
// by the depth-aware composite pass to linearize the raster depth buffer.
static float rtxLastZNear = 4.0f;
static float rtxLastZFar = 4096.0f;

void RTX_GetLastCameraPlanes(float *zNear, float *zFar)
{
    if (zNear) {
        *zNear = rtxLastZNear;
    }
    if (zFar) {
        *zFar = rtxLastZFar;
    }
}

// World->clip matrices for temporal reprojection: the current frame's
// matrix is rotated into the "previous" slot at the start of the next
// frame's RTX_PrepareFrameData.
static float rtxCurrViewProj[16];
static float rtxPrevViewProj[16];
static qboolean rtxCurrViewProjValid = qfalse;
static qboolean rtxPrevViewProjValid = qfalse;

// Column-major 4x4 multiply: out = a * b
static void RTX_MatrixMultiply4x4(const float a[16], const float b[16], float out[16])
{
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

/*
================
RTX_GetPrevViewProjection

Returns the previous frame's view-projection matrix for temporal
reprojection. Returns qfalse (history must be reset) until two frames of
camera data exist.
================
*/
qboolean RTX_GetPrevViewProjection(float outMatrix[16])
{
    if (!rtxPrevViewProjValid) {
        return qfalse;
    }
    Com_Memcpy(outMatrix, rtxPrevViewProj, sizeof(rtxPrevViewProj));
    return qtrue;
}

void RTX_SaveViewParms(void)
{
    // Called from RB_DrawSurfs after backEnd.viewParms is set from the 3D scene
    // Only save the main 3D world view, ignoring portals, weapons, and 2D UI.
    
    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_ALL, "RTX_SaveViewParms: pass=%d rdflags=0x%x fov=%.1f pos=(%.1f,%.1f,%.1f) saved=%d\n",
            tr.frameSceneNum, backEnd.refdef.rdflags, backEnd.viewParms.fovX,
            backEnd.viewParms.or.origin[0], backEnd.viewParms.or.origin[1], backEnd.viewParms.or.origin[2],
            rtxHasValidViewParms);
    }

    // If we already saved a valid view this frame, don't overwrite it with later passes (like weapons/UI).
    if (rtxHasValidViewParms) {
        return;
    }

    if (!(backEnd.refdef.rdflags & RDF_NOWORLDMODEL) && 
        backEnd.viewParms.portalView == PV_NONE &&
        backEnd.viewParms.viewportWidth > 0 &&
        backEnd.viewParms.fovX > 0.0f) {
        rtxSavedViewParms = backEnd.viewParms;
        rtxSavedRefdef = backEnd.refdef;
        rtxHasValidViewParms = qtrue;
    }
}

void RTX_PrepareFrameData(VkCommandBuffer cmd)
{
    if (!vk.device || cmd == VK_NULL_HANDLE) return;

    // All per-frame GPU data is recorded into the frame command buffer with
    // vkCmdUpdateBuffer so the transfers execute in submission order with the
    // ray dispatch that consumes them. Host-mapped writes are not safe here:
    // the previous frame may still be reading these buffers on the GPU.

    // Make prior shader reads of the uniform/light buffers visible to the
    // transfer stage before overwriting them.
    {
        VkMemoryBarrier preBarrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
        };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &preBarrier, 0, NULL, 0, NULL);
    }

    // 1) Update CameraUBO
    if (rtxPipeline.cameraUBO) {
        CameraUBO cam = {0};

        // Use saved viewParms from the 3D rendering pass (backEnd.viewParms
        // may be zero/stale by the time end-of-frame RTX dispatch runs).
        const viewParms_t *vp;
        const trRefdef_t *rd;
        if (rtxHasValidViewParms) {
            vp = &rtxSavedViewParms;
            rd = &rtxSavedRefdef;
            rtxHasValidViewParms = qfalse; // Consume it for this frame
        } else {
            vp = &backEnd.viewParms;
            rd = &backEnd.refdef;
        }

        // Build view inverse (camera → world) from the model-view matrix.
        // vp->world.modelMatrix is the world→camera transform (column-major),
        // built by R_RotateForViewer (vp->or.modelMatrix is NOT populated).
        if (!MatrixInverse(vp->world.modelMatrix, cam.viewInverse)) {
            // Singular matrix — build identity so rays at least have finite dirs
            Com_Memset(cam.viewInverse, 0, sizeof(cam.viewInverse));
            cam.viewInverse[0] = cam.viewInverse[5] = cam.viewInverse[10] = cam.viewInverse[15] = 1.0f;
            if (r_rtx_debug && r_rtx_debug->integer >= 1) {
                ri.Printf(PRINT_WARNING, "RTX: Singular modelMatrix encountered!\n");
            }
        }

        // Build projection inverse from the engine projection matrix.
        if (!MatrixInverse(vp->projectionMatrix, cam.projInverse)) {
            Com_Memset(cam.projInverse, 0, sizeof(cam.projInverse));
            cam.projInverse[0] = cam.projInverse[5] = cam.projInverse[10] = cam.projInverse[15] = 1.0f;
        }

        VectorCopy(vp->or.origin, cam.position);
        VectorCopy(vp->or.axis[0], cam.forward);
        VectorCopy(vp->or.axis[1], cam.right);
        VectorCopy(vp->or.axis[2], cam.up);
        cam.nearPlane = vp->zNear;
        cam.farPlane = vp->zFar;
        rtxLastZNear = vp->zNear;
        rtxLastZFar = vp->zFar;

        // Rotate last frame's world->clip matrix into the "previous" slot
        // (consumed by temporal reprojection later this frame), then record
        // the current one.
        if (rtxCurrViewProjValid) {
            Com_Memcpy(rtxPrevViewProj, rtxCurrViewProj, sizeof(rtxPrevViewProj));
            rtxPrevViewProjValid = qtrue;
        }
        RTX_MatrixMultiply4x4(vp->projectionMatrix, vp->world.modelMatrix, rtxCurrViewProj);
        rtxCurrViewProjValid = qtrue;
        cam.fov = rd->fov_x;
        cam.frameCount = tr.frameCount;
        cam.enablePathTracing = 1;
        cam.maxBounces = (uint32_t)RTX_GetEffectiveBounceCount();
        int samplesPerPixel = rt.samplesPerPixel > 0 ? rt.samplesPerPixel : 1;
        cam.samplesPerPixel = (uint32_t)samplesPerPixel;
        int debugModeInt = (r_rtx_debug) ? r_rtx_debug->integer : 0;
        if (debugModeInt == 0 && r_rtx_surface_debug) {
            debugModeInt = r_rtx_surface_debug->integer;
        }
        if (debugModeInt < 0) {
            debugModeInt = 0;
        }
        if (debugModeInt > 8) {
            debugModeInt = 8;
        }
        cam.surfaceDebugMode = (uint32_t)debugModeInt;

        vkCmdUpdateBuffer(cmd, rtxPipeline.cameraUBO, 0, sizeof(cam), &cam);
    }

    // 2) Render settings
    if (rtxPipeline.renderSettingsUBO) {
        RenderSettingsUBO rs = {0};
        rs.enableShadows = 1;
        rs.enableReflections = 1;
        rs.enableGI = 1;
        // enableAO is unused by the shader as an AO flag; it is repurposed as
        // the ReSTIR/RIS enable (render_plan Phase 7). See raygen.rgen.
        rs.enableAO = (rt_restir && rt_restir->integer) ? 1 : 0;
        rs.shadowBias = 0.001f;
        rs.reflectionRoughnessCutoff = 0.9f;
        rs.giIntensity = 1.0f;
        rs.aoRadius = 0.5f;
        int debugModeInt = (r_rtx_debug) ? r_rtx_debug->integer : 0;
        if (debugModeInt == 0 && r_rtx_surface_debug) {
            debugModeInt = r_rtx_surface_debug->integer;
        }
        if (debugModeInt < 0) {
            debugModeInt = 0;
        }
        if (debugModeInt > 8) {
            debugModeInt = 8;
        }
        rs.debugMode = (uint32_t)debugModeInt;
        rs.enableDenoiser = (rtx_denoise && rtx_denoise->integer) ? 1 : 0;
        rs.enableDLSS = (rtx_dlss && rtx_dlss->integer) ? 1 : 0;
        rs.enableMotionBlur = 0;

        for (int axis = 0; axis < 3; ++axis) {
            rs.lightGridOrigin[axis] = rt.lightGrid.origin[axis];
            float cellSize = rt.lightGrid.cellSize[axis];
            if (cellSize <= 0.0f || !isfinite(cellSize)) {
                cellSize = 1.0f;
            }
            float invCellSize = rt.lightGrid.invCellSize[axis];
            if (invCellSize <= 0.0f || !isfinite(invCellSize)) {
                invCellSize = 1.0f / cellSize;
            }
            rs.lightGridCellSize[axis] = cellSize;
            rs.lightGridInvCellSize[axis] = invCellSize;
        }
        rs.lightGridOrigin[3] = 0.0f;
        rs.lightGridCellSize[3] = 0.0f;
        rs.lightGridInvCellSize[3] = 0.0f;

        rs.lightGridDims[0] = (uint32_t)rt.lightGrid.dims[0];
        rs.lightGridDims[1] = (uint32_t)rt.lightGrid.dims[1];
        rs.lightGridDims[2] = (uint32_t)rt.lightGrid.dims[2];
        rs.lightGridDims[3] = (uint32_t)rt.lightGrid.cellCount;

        rs.lightGridCounts[0] = rt.lightGrid.directionalCount;
        rs.lightGridCounts[1] = rt.lightGrid.offsetCount;
        rs.lightGridCounts[2] = rt.lightGrid.indexCount;
        // Where the dynamic (non-grid) lights start in the scene light array
        rs.lightGridCounts[3] = (uint32_t)MAX(rt.staticSceneLightCount, 0);
        rs.skyAmbient[0] = rt.skyAmbientColor[0];
        rs.skyAmbient[1] = rt.skyAmbientColor[1];
        rs.skyAmbient[2] = rt.skyAmbientColor[2];
        rs.skyAmbient[3] = rt.skyAmbientIntensity;

        // Volumetric fog: cvar 1.0 ≈ optical depth 0.25 over the full march
        rs.volumetricParams[0] = (rt_volumetricDensity ? rt_volumetricDensity->value : 1.0f) * 0.0001f;
        rs.volumetricParams[1] = 0.45f;   // Henyey-Greenstein anisotropy
        rs.volumetricParams[2] = 2500.0f; // max march distance
        rs.volumetricParams[3] = (rt_volumetric && rt_volumetric->integer) ? 1.0f : 0.0f;

        rs.featureParams[0] = (rt_caustics && rt_caustics->integer) ? 1.0f : 0.0f;
        rs.featureParams[1] = 1.1f;       // caustic ridge intensity
        // In-scatter radiance scale: sun/effect light carried by the fog.
        // Cvar 1.0 keeps a full-length unoccluded march well below sky
        // luminance so open skies stay dark instead of washing to white.
        rs.featureParams[2] = (rt_volumetricScatter ? rt_volumetricScatter->value : 1.0f) * 0.06f;
        // Reflections toggle (rt_reflections): splits the GI bounce into
        // diffuse + specular lobes when on.
        rs.featureParams[3] = (rt_reflections && rt_reflections->integer) ? 1.0f : 0.0f;

        // Soft shadows: sample each light's area so shadow edges gain a
        // distance-widening penumbra (contact hardening). y = point-light
        // physical size as a fraction of its influence radius; z = the sun's
        // angular radius in radians (~0.5deg default).
        rs.shadowParams[0] = (rt_softShadows && rt_softShadows->integer) ? 1.0f : 0.0f;
        rs.shadowParams[1] = rt_softShadowScale ? rt_softShadowScale->value : 0.08f;
        rs.shadowParams[2] = (rt_sunSoftness ? rt_sunSoftness->value : 0.5f) * (3.14159265f / 180.0f);
        // w = refraction enable: reflective/refractive water & glass surfaces.
        rs.shadowParams[3] = (rt_refraction && rt_refraction->integer) ? 1.0f : 0.0f;

        vkCmdUpdateBuffer(cmd, rtxPipeline.renderSettingsUBO, 0, sizeof(rs), &rs);
    }

    // 3) Environment
    if (rtxPipeline.environmentUBO) {
        EnvironmentUBO env = {0};
        // Prefer the skylight the light system inferred from the map's sky
        // surfaces so the traced sky matches the map's art direction; fall
        // back to the legacy q3map_sun values.
        vec3_t sunDir;
        vec3_t sunColor;
        float sunIntensity = 0.0f;
        qboolean haveSceneSun = qfalse;

        for (int i = 0; i < rt.numSceneLights; ++i) {
            const rtSceneLight_t *sl = &rt.sceneLights[i];
            if (sl->type == RT_LIGHT_TYPE_DIRECTIONAL) {
                // Scene light direction is the travel direction of the light;
                // the environment wants the direction toward the sun.
                VectorNegate(sl->direction, sunDir);
                VectorCopy(sl->color, sunColor);
                sunIntensity = sl->intensity;
                haveSceneSun = qtrue;
                break;
            }
        }

        if (!haveSceneSun) {
            VectorCopy(tr.sunDirection, sunDir);
            sunIntensity = VectorNormalize2(tr.sunLight, sunColor);
        }
        if (VectorNormalize(sunDir) <= 0.0f) {
            VectorSet(sunDir, 0.0f, 0.0f, 1.0f);
        }
        if (sunIntensity <= 0.0f) {
            VectorSet(sunColor, 1.0f, 0.98f, 0.95f);
            sunIntensity = 3.0f;
        }

        env.sunDirection[0] = sunDir[0];
        env.sunDirection[1] = sunDir[1];
        env.sunDirection[2] = sunDir[2];
        env.sunIntensity = sunIntensity;
        VectorCopy(sunColor, env.sunColor);
        env.skyIntensity = 1.0f;
        VectorSet(env.fogColor, 0.5f, 0.6f, 0.7f);
        env.fogDensity = 0.0f;
        env.fogStart = 0.0f;
        env.fogEnd = 0.0f;
        env.useEnvironmentMap = 0;
        env.useProceduralSky = 1;
        env.time = ri.Milliseconds() * 0.001f;
        env.cloudCoverage = rt_cloudCoverage ? Com_Clamp(0.0f, 1.0f, rt_cloudCoverage->value) : 0.0f;

        // Average sky color sampled from the map's sky shader; drives the
        // traced sky background and bounce tint. The color is hue-only
        // (normalized to max channel 1.0), so the alpha channel sets the
        // backdrop luminance: 0.18 lands around 0.28 sRGB after exposure
        // and ACES — a dark, moody sky that cloud highlights and the sun
        // disc still read against.
        if (rt.skyDomeColor[0] > 0.0f || rt.skyDomeColor[1] > 0.0f || rt.skyDomeColor[2] > 0.0f) {
            // Full-chroma average of the map's sky art (the lighting uses
            // the desaturated skyAmbientColor instead)
            env.skyColor[0] = rt.skyDomeColor[0];
            env.skyColor[1] = rt.skyDomeColor[1];
            env.skyColor[2] = rt.skyDomeColor[2];
        } else if (rt.skyAmbientColor[0] > 0.0f || rt.skyAmbientColor[1] > 0.0f || rt.skyAmbientColor[2] > 0.0f) {
            env.skyColor[0] = rt.skyAmbientColor[0];
            env.skyColor[1] = rt.skyAmbientColor[1];
            env.skyColor[2] = rt.skyAmbientColor[2];
        } else {
            env.skyColor[0] = 0.55f;
            env.skyColor[1] = 0.65f;
            env.skyColor[2] = 0.85f;
        }
        env.skyColor[3] = 0.18f;

        vkCmdUpdateBuffer(cmd, rtxPipeline.environmentUBO, 0, sizeof(env), &env);
    }

    if (rtxPipeline.debugSettingsUBO) {
        DebugSettingsUBO debugData = {0};
        debugData.noTextures = (r_rtx_debug && r_rtx_debug->integer == 2) ? 1u : 0u;
        debugData.debugMode = (r_rtx_debug) ? (uint32_t)MAX(r_rtx_debug->integer, 0) : 0u;
        float overlayBlend = (r_rtx_debugBlend) ? r_rtx_debugBlend->value : 0.0f;
        if (!isfinite(overlayBlend)) {
            overlayBlend = 0.0f;
        }
        overlayBlend = Com_Clamp(0.0f, 1.0f, overlayBlend);
        debugData.debugOverlayBlend = overlayBlend;
        // Bit 0 = parallax-occlusion enable (written every frame, so rt_parallax
        // toggles POM live; the _h height textures themselves are loaded/unloaded
        // at map load per the same cvar).
        debugData.debugFlags = (rt_parallax && rt_parallax->integer) ? 1u : 0u;

        vkCmdUpdateBuffer(cmd, rtxPipeline.debugSettingsUBO, 0, sizeof(debugData), &debugData);
    }

    if (rtxPipeline.volumeFXUBO) {
        VolumeFXUBO vfx;
        Com_Memset(&vfx, 0, sizeof(vfx));
        vfx.count = (uint32_t)RT_VolumeFX_FillGpu(vfx.fx, RT_MAX_VOLUME_FX);
        vkCmdUpdateBuffer(cmd, rtxPipeline.volumeFXUBO, 0, sizeof(vfx), &vfx);
    }

    // 4) Upload material buffer if dirty (rare: world load / new shader registration)
    RTX_BuildMaterialBuffer();
    RTX_UploadMaterialBuffer(vk.device, cmd, VK_NULL_HANDLE);

    // 5) Record pending scene light data into the frame command buffer
    RT_RecordSceneLightUpload(cmd);

    // Make the transfer writes visible to ray tracing and compute reads.
    {
        VkMemoryBarrier postBarrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT
        };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &postBarrier, 0, NULL, 0, NULL);
    }
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
    VkWriteDescriptorSet writes[27];
    uint32_t writeCount = 0;
    // Uniform-buffer bindings lack UPDATE_AFTER_BIND (unsupported on NVIDIA),
    // so they must be written before the set is bound and cannot be written
    // once-globally: the descriptor set is REALLOCATED whenever the pipeline is
    // recreated (map change / resize), and each new set starts with its UBO
    // bindings uninitialised. Track the set the UBOs were last written to and
    // rewrite them whenever the handle changes; otherwise the freshly allocated
    // set traces with never-updated descriptors (VUID-vkCmdTraceRaysKHR-08114,
    // which only "worked" because NVIDIA reuses the pool's descriptor storage).
    static VkDescriptorSet uboWrittenSet = VK_NULL_HANDLE;
    qboolean writeUniformBindings = qfalse;

    if (colorImage == VK_NULL_HANDLE) {
        ri.Printf(PRINT_WARNING, "RTX: Descriptor update skipped (color image view unavailable)\n");
        return;
    }
    if (albedoImage == VK_NULL_HANDLE) {
        albedoImage = colorImage;
    }
    if (normalImage == VK_NULL_HANDLE) {
        normalImage = colorImage;
    }
    if (motionImage == VK_NULL_HANDLE) {
        motionImage = colorImage;
    }
    if (depthImage == VK_NULL_HANDLE) {
        depthImage = colorImage;
    }

    // Get lighting contribution image views
    VkImageView directLightView = NULL;
    VkImageView indirectLightView = NULL;
    RTX_GetLightingContributionViews(&directLightView, &indirectLightView);

    // Use color image as fallback if lighting buffers aren't created yet
    if (!directLightView) directLightView = colorImage;
    if (!indirectLightView) indirectLightView = colorImage;

    VkImageView mediaView = RTX_GetMediaImageView();
    if (!mediaView) mediaView = colorImage;

    // Ensure GPU-side buffers exist before computing the binding signature.
    RT_UpdateSceneLightBuffer();
    RT_UpdateLightGridBuffers();
    RTX_CreateDummyBuffer();

    // Rewriting the descriptor set is only legal while no submitted command
    // buffer still references it, so skip the update unless a bound resource
    // actually changed (world load, resize, TLAS rebuild, texture upload).
    {
        typedef struct {
            uint64_t descriptorSet;
            uint64_t tlas;
            uint64_t views[9];
            uint64_t buffers[7];
            uint64_t texViewHash;
            uint64_t triMatCount;
        } rtxDescriptorKey_t;
        static rtxDescriptorKey_t cachedKey;
        static qboolean cachedKeyValid = qfalse;
        rtxDescriptorKey_t key;

        Com_Memset(&key, 0, sizeof(key));
        // Include the set handle so a reallocated set is never treated as
        // "unchanged" and skipped by the early-out below.
        key.descriptorSet = (uint64_t)rtxPipeline.descriptorSet;
        key.tlas = (uint64_t)tlas;
        key.views[0] = (uint64_t)colorImage;
        key.views[1] = (uint64_t)albedoImage;
        key.views[2] = (uint64_t)normalImage;
        key.views[3] = (uint64_t)motionImage;
        key.views[4] = (uint64_t)depthImage;
        key.views[5] = (uint64_t)directLightView;
        key.views[6] = (uint64_t)indirectLightView;
        key.views[7] = (uint64_t)((tr.whiteImage && tr.whiteImage->view) ? tr.whiteImage->view : VK_NULL_HANDLE);
        key.views[8] = (uint64_t)mediaView;
        key.buffers[0] = (uint64_t)rtxPipeline.instanceDataBuffer;
        key.buffers[1] = (uint64_t)RTX_GetMaterialBuffer();
        key.buffers[2] = (uint64_t)RT_GetSceneLightBuffer();
        key.buffers[3] = (uint64_t)RT_GetLightGridOffsetBuffer();
        key.buffers[4] = (uint64_t)RT_GetLightGridIndexBuffer();
        key.buffers[5] = (uint64_t)rtxPipeline.triangleMaterialBuffer;
        key.buffers[6] = (uint64_t)rtxPipeline.rayQueryBuffer;
        key.triMatCount = (uint64_t)rtxPipeline.triangleMaterialCount;

        if (rtxPipeline.textureSampler) {
            static VkDescriptorImageInfo keyTexInfos[RTX_MAX_TEXTURES];
            VkImageView keyFallback = (tr.whiteImage && tr.whiteImage->view) ? tr.whiteImage->view : colorImage;
            RTX_FillTextureDescriptorInfos(keyTexInfos, ARRAY_LEN(keyTexInfos),
                                           rtxPipeline.textureSampler, keyFallback);
            uint64_t hash = 1469598103934665603ULL;
            for (uint32_t i = 0; i < ARRAY_LEN(keyTexInfos); ++i) {
                hash ^= (uint64_t)keyTexInfos[i].imageView;
                hash *= 1099511628211ULL;
            }
            key.texViewHash = hash;
        }

        if (cachedKeyValid && rtxPipeline.descriptorSetReady &&
            memcmp(&key, &cachedKey, sizeof(key)) == 0) {
            return;
        }

        // Bindings changed: drain the queue so no in-flight frame still reads
        // the descriptor set, then rewrite it below. The UBO bindings are
        // rewritten whenever the descriptor set itself changed (first build or
        // a pipeline recreation), independent of the resource-change key.
        vkQueueWaitIdle(vk.queue);
        writeUniformBindings = (uboWrittenSet != rtxPipeline.descriptorSet);
        cachedKey = key;
        cachedKeyValid = qtrue;
    }

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
    
    // Uniform buffers - Camera (6) and RenderSettings (7)
    VkDescriptorBufferInfo bufferInfos[2] = {
        { .buffer = rtxPipeline.cameraUBO, .offset = 0, .range = sizeof(CameraUBO) },
        { .buffer = rtxPipeline.renderSettingsUBO, .offset = 0, .range = sizeof(RenderSettingsUBO) }
    };

    if (writeUniformBindings) {
        for (uint32_t i = 0; i < 2; i++) {
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
    }
    
    // Binding 8: Environment map (use default image as placeholder)
    VkSampler envSampler = rtxPipeline.textureSampler;
    if (envSampler == VK_NULL_HANDLE) {
        ri.Printf(PRINT_WARNING, "RTX: Environment sampler unavailable for descriptor update\n");
        return;
    }

    VkImageView envView = VK_NULL_HANDLE;
    if (tr.whiteImage && tr.whiteImage->view) {
        envView = tr.whiteImage->view;
    } else if (tr.defaultImage && tr.defaultImage->view) {
        envView = tr.defaultImage->view;
    }

    VkDescriptorImageInfo envImageInfo = {
        .sampler = envSampler,
        .imageView = envView,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    if (envImageInfo.imageView == VK_NULL_HANDLE) {
        envImageInfo.imageView = colorImage ? colorImage : directLightView;
        envImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    if (envImageInfo.imageView != VK_NULL_HANDLE) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 8,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &envImageInfo
        };
    } else {
        static qboolean loggedEnvWarning = qfalse;
        if (!loggedEnvWarning) {
            ri.Printf(PRINT_WARNING, "RTX: Skipping environment descriptor update (no valid image view)\n");
            loggedEnvWarning = qtrue;
        }
    }

    // Texture array binding (12) – populate all slots with registered or fallback textures
    if (rtxPipeline.textureSampler) {
        VkImageView fallbackView = VK_NULL_HANDLE;
        if (tr.whiteImage && tr.whiteImage->view) {
            fallbackView = tr.whiteImage->view;
        } else if (tr.defaultImage && tr.defaultImage->view) {
            fallbackView = tr.defaultImage->view;
        } else {
            // As a last resort, reuse the color image so descriptor is non-null
            fallbackView = colorImage;
        }

        static VkDescriptorImageInfo texInfos[RTX_MAX_TEXTURES];
        RTX_FillTextureDescriptorInfos(texInfos, ARRAY_LEN(texInfos),
                                       rtxPipeline.textureSampler, fallbackView);

        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 12,
            .dstArrayElement = 0,
            .descriptorCount = ARRAY_LEN(texInfos),
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = texInfos
        };
    }
    
    // Environment UBO at binding 9
    VkDescriptorBufferInfo envBufferInfo = {
        .buffer = rtxPipeline.environmentUBO,
        .offset = 0,
        .range = sizeof(EnvironmentUBO)
    };

    if (writeUniformBindings) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 9,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &envBufferInfo
        };
    }
    
    // Storage buffers. Ranges use VK_WHOLE_SIZE so per-frame content changes
    // (e.g. varying light counts) never require a descriptor rewrite; shaders
    // read the embedded counts instead.
    VkBuffer matBuf = RTX_GetMaterialBuffer();
    VkBuffer lightBuf = RT_GetSceneLightBuffer();
    VkBuffer lightGridOffsetBuf = RT_GetLightGridOffsetBuffer();
    VkBuffer lightGridIndexBuf = RT_GetLightGridIndexBuffer();
    if (matBuf == VK_NULL_HANDLE) {
        matBuf = rtxDummyBuffer;
    }
    if (lightBuf == VK_NULL_HANDLE) {
        lightBuf = rtxDummyBuffer;
    }
    VkDescriptorBufferInfo storageBufferInfos[5] = {
        { .buffer = rtxPipeline.instanceDataBuffer ? rtxPipeline.instanceDataBuffer : rtxDummyBuffer, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = matBuf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = lightBuf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = lightGridOffsetBuf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = lightGridIndexBuf, .offset = 0, .range = VK_WHOLE_SIZE }
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

    VkDescriptorBufferInfo offsetInfo = storageBufferInfos[3];
    if (offsetInfo.buffer == VK_NULL_HANDLE) {
        offsetInfo.buffer = rtxPipeline.instanceDataBuffer;
        offsetInfo.range = VK_WHOLE_SIZE;
    } else if (offsetInfo.range == 0) {
        offsetInfo.range = VK_WHOLE_SIZE;
    }
    if (offsetInfo.buffer != VK_NULL_HANDLE) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 21,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &offsetInfo
        };
    } else if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Light grid offset descriptor skipped (no fallback buffer available)\n");
    }

    VkDescriptorBufferInfo indexInfo = storageBufferInfos[4];
    if (indexInfo.buffer == VK_NULL_HANDLE) {
        indexInfo.buffer = rtxPipeline.instanceDataBuffer;
        indexInfo.range = VK_WHOLE_SIZE;
    } else if (indexInfo.range == 0) {
        indexInfo.range = VK_WHOLE_SIZE;
    }
    if (indexInfo.buffer != VK_NULL_HANDLE) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 22,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &indexInfo
        };
    } else if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Light grid index descriptor skipped (no fallback buffer available)\n");
    }

    VkDescriptorBufferInfo triangleMaterialInfo = {
        .buffer = (rtxPipeline.triangleMaterialBuffer && rtxPipeline.triangleMaterialCount > 0)
                  ? rtxPipeline.triangleMaterialBuffer
                  : rtxDummyBuffer,
        .offset = 0,
        .range = (rtxPipeline.triangleMaterialBuffer && rtxPipeline.triangleMaterialCount > 0)
                 ? sizeof(uint32_t) * rtxPipeline.triangleMaterialCount
                 : VK_WHOLE_SIZE
    };

    writes[writeCount++] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = rtxPipeline.descriptorSet,
        .dstBinding = 20,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &triangleMaterialInfo
    };

    // Add lighting contribution images (bindings 15, 16)
    VkDescriptorImageInfo lightingImageInfos[2] = {
        { .imageView = directLightView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = indirectLightView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL }
    };

    for (uint32_t i = 0; i < 2; i++) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 15 + i,  // Bindings 15, 16
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &lightingImageInfos[i]
        };
    }

    // Debug settings UBO (binding 18)
    VkDescriptorBufferInfo debugBufferInfo = {
        .buffer = rtxPipeline.debugSettingsUBO,
        .offset = 0,
        .range = sizeof(DebugSettingsUBO)
    };

    if (writeUniformBindings) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 18,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &debugBufferInfo
        };
    }

    // Volumetric weapon effects UBO (binding 13)
    VkDescriptorBufferInfo volumeFXBufferInfo = {
        .buffer = rtxPipeline.volumeFXUBO,
        .offset = 0,
        .range = sizeof(VolumeFXUBO)
    };

    if (writeUniformBindings && rtxPipeline.volumeFXUBO) {
        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 13,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &volumeFXBufferInfo
        };
    }

    // Media image (binding 17): volumetric in-scatter + transmittance
    VkDescriptorImageInfo mediaImageInfo = {
        .imageView = mediaView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL
    };

    writes[writeCount++] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = rtxPipeline.descriptorSet,
        .dstBinding = 17,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &mediaImageInfo
    };

    if (rtxPipeline.rayQueryBuffer) {
        VkDescriptorBufferInfo queryBufferInfo = {
            .buffer = rtxPipeline.rayQueryBuffer,
            .offset = 0,
            .range = sizeof(rtxShadowQueryGpu_t) * rtxPipeline.rayQueryCapacity
        };

        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 19,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &queryBufferInfo
        };
    }
    
    vkUpdateDescriptorSets(vk.device, writeCount, writes, 0, NULL);
    rtxPipeline.descriptorSetReady = qtrue;

    // Only now that the writes actually landed (the function has several early
    // returns before this point) is it safe to record which set holds the UBO
    // bindings, so a bailed-out update is retried rather than assumed done.
    if (writeUniformBindings) {
        uboWrittenSet = rtxPipeline.descriptorSet;
    }
}

void RTX_UpdateInstanceDataBuffer(const rtxInstanceGpuData_t *instances, int count) {
    if (!rtxPipeline.instanceDataBufferMemory) {
        return;
    }

    int clampedCount = count;
    if (clampedCount < 0) {
        clampedCount = 0;
    } else if (clampedCount > RTX_MAX_INSTANCES) {
        ri.Printf(PRINT_WARNING, "RTX: Instance data count %d exceeds capacity %d, clamping\n",
                  clampedCount, RTX_MAX_INSTANCES);
        clampedCount = RTX_MAX_INSTANCES;
    }

    VkDeviceSize totalSize = sizeof(rtxInstanceGpuData_t) * RTX_MAX_INSTANCES;
    VkDeviceSize copySize = sizeof(rtxInstanceGpuData_t) * clampedCount;

    void *mapped = NULL;
    if (vkMapMemory(vk.device, rtxPipeline.instanceDataBufferMemory, 0, totalSize, 0, &mapped) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to map instance data buffer memory\n");
        return;
    }

    uint8_t *dst = (uint8_t *)mapped;
    if (copySize > 0 && instances) {
        Com_Memcpy(dst, instances, (size_t)copySize);
    }
    if (copySize < totalSize) {
        Com_Memset(dst + copySize, 0, (size_t)(totalSize - copySize));
    }

    vkUnmapMemory(vk.device, rtxPipeline.instanceDataBufferMemory);
    rtxPipeline.activeInstances = (uint32_t)clampedCount;
}

void RTX_UploadTriangleMaterials(VkCommandBuffer cmd, const uint32_t *materials, uint32_t count) {
    if (!vk.device) {
        return;
    }

    if (count == 0 || !materials) {
        rtxPipeline.triangleMaterialCount = 0;
        return;
    }

    VkDeviceSize bufferSize = sizeof(uint32_t) * (VkDeviceSize)count;

    if (!rtxPipeline.triangleMaterialBuffer ||
        rtxPipeline.triangleMaterialCapacity < count) {
        VkBuffer oldBuffer = rtxPipeline.triangleMaterialBuffer;
        VkDeviceMemory oldMemory = rtxPipeline.triangleMaterialBufferMemory;

        VkBufferCreateInfo bufferInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bufferSize,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };

        VkBuffer newBuffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(vk.device, &bufferInfo, NULL, &newBuffer) != VK_SUCCESS) {
            rtxPipeline.triangleMaterialCount = 0;
            rtxPipeline.triangleMaterialCapacity = 0;
            return;
        }

        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Created triangle material buffer %p (%llu bytes)\n",
                      (void*)newBuffer,
                      (unsigned long long)bufferSize);
        }

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(vk.device, newBuffer, &memReqs);

        VkMemoryAllocateFlagsInfo allocFlags = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
        };

        // Pad host-visible triangle material buffer to tolerate small overruns.
        const VkDeviceSize pad = 1024 * 1024; // 1 MiB
        VkDeviceSize padAligned = (pad + memReqs.alignment - 1) & ~(memReqs.alignment - 1);

        VkMemoryAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &allocFlags,
            .allocationSize = memReqs.size + padAligned,
            .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        };

        VkDeviceMemory newMemory = VK_NULL_HANDLE;
        if (vkAllocateMemory(vk.device, &allocInfo, NULL, &newMemory) != VK_SUCCESS) {
            vkDestroyBuffer(vk.device, newBuffer, NULL);
            rtxPipeline.triangleMaterialCount = 0;
            rtxPipeline.triangleMaterialCapacity = 0;
            return;
        }

        vkBindBufferMemory(vk.device, newBuffer, newMemory, 0);

        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Triangle material memory %p bound\n",
                      (void*)newMemory);
            VkDeviceAddress addr = RTX_GetBufferDeviceAddressVK(newBuffer);
            if (addr) {
                ri.Printf(PRINT_DEVELOPER,
                          "RTX: Triangle material buffer addr=0x%llx size=%llu (count=%u)\n",
                          (unsigned long long)addr,
                          (unsigned long long)allocInfo.allocationSize,
                          count);
            }
        }

        rtxPipeline.triangleMaterialBuffer = newBuffer;
        rtxPipeline.triangleMaterialBufferMemory = newMemory;
        rtxPipeline.triangleMaterialCapacity = count;

        if (rtxPipeline.descriptorSet != VK_NULL_HANDLE) {
            VkDescriptorBufferInfo info = {
                .buffer = rtxPipeline.triangleMaterialBuffer,
                .offset = 0,
                .range = bufferSize
            };

            VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = rtxPipeline.descriptorSet,
                .dstBinding = 20,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &info
            };

            vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
        }

        if (oldBuffer != VK_NULL_HANDLE) {
            vkQueueWaitIdle(vk.queue);
            if (r_rtx_debug && r_rtx_debug->integer >= 2) {
                ri.Printf(PRINT_DEVELOPER,
                          "RTX: Destroying old triangle material buffer %p\n",
                          (void*)oldBuffer);
            }
            vkDestroyBuffer(vk.device, oldBuffer, NULL);
        }
        if (oldMemory != VK_NULL_HANDLE) {
            if (r_rtx_debug && r_rtx_debug->integer >= 2) {
                ri.Printf(PRINT_DEVELOPER,
                          "RTX: Freeing old triangle material memory %p\n",
                          (void*)oldMemory);
            }
            vkFreeMemory(vk.device, oldMemory, NULL);
        }
    }

    if (!cmd) {
        return;
    }

    void *mapped = NULL;
    if (vkMapMemory(vk.device, rtxPipeline.triangleMaterialBufferMemory, 0, bufferSize, 0, &mapped) != VK_SUCCESS) {
        return;
    }

    Com_Memcpy(mapped, materials, bufferSize);
    vkUnmapMemory(vk.device, rtxPipeline.triangleMaterialBufferMemory);

    if (cmd != VK_NULL_HANDLE) {
        VkBufferMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = rtxPipeline.triangleMaterialBuffer,
            .offset = 0,
            .size = bufferSize
        };

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, NULL, 1, &barrier, 0, NULL);
    }

    rtxPipeline.triangleMaterialCount = count;
}

qboolean RTX_UpdateRayQueryDescriptors(VkAccelerationStructureKHR tlas) {
    if (!vk.device || rtxPipeline.descriptorSet == VK_NULL_HANDLE) {
        return qfalse;
    }

    if (!rtxPipeline.descriptorSetReady) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Ray query descriptors skipped (descriptor set not ready)\n");
        }
        return qfalse;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: UpdateRayQueryDescriptors set=%p tlas=%p materialBuf=%p triBuf=%p triCount=%u\n",
                  (void*)rtxPipeline.descriptorSet,
                  (void*)tlas,
                  (void*)RTX_GetMaterialBuffer(),
                  (void*)rtxPipeline.triangleMaterialBuffer,
                  rtxPipeline.triangleMaterialCount);
    }

    VkWriteDescriptorSet writes[5];
    uint32_t writeCount = 0;

    if (tlas != VK_NULL_HANDLE) {
        VkWriteDescriptorSetAccelerationStructureKHR asInfo = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1,
            .pAccelerationStructures = &tlas
        };

        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = &asInfo,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
        };
    }

    VkBuffer materialBuffer = RTX_GetMaterialBuffer();
    if (materialBuffer != VK_NULL_HANDLE) {
        VkDescriptorBufferInfo materialInfo = {
            .buffer = materialBuffer,
            .offset = 0,
            .range = VK_WHOLE_SIZE
        };

        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 11,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &materialInfo
        };
    }

    if (rtxPipeline.rayQueryBuffer != VK_NULL_HANDLE) {
        VkDeviceSize elementCount = (rtxPipeline.rayQueryCapacity > 0)
            ? (VkDeviceSize)rtxPipeline.rayQueryCapacity
            : 1;
        VkDescriptorBufferInfo queryInfo = {
            .buffer = rtxPipeline.rayQueryBuffer,
            .offset = 0,
            .range = sizeof(rtxShadowQueryGpu_t) * elementCount
        };

        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 19,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &queryInfo
        };
    } else if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: RayQuery descriptor update skipped query buffer (buffer unavailable)\n");
    }

    if (rtxPipeline.triangleMaterialBuffer && rtxPipeline.triangleMaterialCount > 0) {
        VkDescriptorBufferInfo triangleInfo = {
            .buffer = rtxPipeline.triangleMaterialBuffer,
            .offset = 0,
            .range = sizeof(uint32_t) * rtxPipeline.triangleMaterialCount
        };

        writes[writeCount++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = rtxPipeline.descriptorSet,
            .dstBinding = 20,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &triangleInfo
        };
    }

    if (writeCount == 0) {
        if (r_rtx_debug && r_rtx_debug->integer >= 3) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Ray query descriptor update skipped (no valid resources)\n");
        }
        return qfalse;
    }

    vkUpdateDescriptorSets(vk.device, writeCount, writes, 0, NULL);
    if (r_rtx_debug && r_rtx_debug->integer >= 3) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Ray query descriptors updated (%u writes) set=%p binding0=%p\n",
                  writeCount,
                  (void*)rtxPipeline.descriptorSet,
                  (void*)tlas);
    }
    return qtrue;
}

VkBuffer RTX_GetTriangleMaterialBuffer(void) {
    return rtxPipeline.triangleMaterialBuffer;
}

uint32_t RTX_GetTriangleMaterialCount(void) {
    return rtxPipeline.triangleMaterialCount;
}

// vk_find_memory_type is already defined in vk.c and declared in vk.h
// No need to redefine it here
