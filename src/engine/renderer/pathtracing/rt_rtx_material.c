/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

RTX Material System
Converts Quake3 shaders to PBR materials for ray tracing
===========================================================================
*/

#include "rt_rtx.h"
#include "../materials/tr_material.h"
#include "../vulkan/vk.h"
#include "../core/tr_local.h"
#include "../core/tr_common.h"

// External Vulkan instance
extern Vk_Instance vk;

// ============================================================================
// Material Cache Definition
// ============================================================================

// PBR Material structure matching shader layout
typedef struct {
    vec4_t albedo;              // Base color (RGB) + opacity (A)
    vec4_t specular;            // Specular color (RGB) + reflectance (A)
    vec4_t emission;            // Emissive color (RGB) + intensity (A)
    float roughness;            // Surface roughness [0-1]
    float metallic;             // Metallic value [0-1]
    float normalScale;          // Normal map intensity
    float occlusionStrength;    // Ambient occlusion strength
    uint32_t flags;             // Material flags (two-sided, alpha test, etc.)
    uint32_t albedoTexture;     // Texture indices
    uint32_t normalTexture;
    uint32_t metallicTexture;
    uint32_t roughnessTexture;
    uint32_t aoTexture;
    uint32_t emissiveTexture;
    uint32_t padding;           // Alignment to 16 bytes
} MaterialData;

#define MATERIAL_FLAG_TWO_SIDED        (1 << 0)
#define MATERIAL_FLAG_ALPHA_TEST       (1 << 1)
#define MATERIAL_FLAG_ALPHA_BLEND      (1 << 2)
#define MATERIAL_FLAG_EMISSIVE         (1 << 3)
#define MATERIAL_FLAG_WATER            (1 << 4)
#define MATERIAL_FLAG_GLASS            (1 << 5)
#define MATERIAL_FLAG_METAL            (1 << 6)
#define MATERIAL_FLAG_VERTEX_LIGHTING  (1 << 7)
#define MATERIAL_FLAG_NO_SHADOWS       (1 << 8)

// Internal material representation
typedef struct rtxMaterial_s {
    char name[MAX_QPATH];
    MaterialData data;
    shader_t *shader;
    qboolean converted;
} rtxMaterial_t;

// Material cache
typedef struct {
    rtxMaterial_t *materials;
    int numMaterials;
    int maxMaterials;
    qboolean dirty;
    
    // GPU buffer
    VkBuffer buffer;
    VkDeviceMemory memory;
    size_t bufferSize;
} rtxMaterialCache_t;

static rtxMaterialCache_t materialCache;

// ============================================================================
// Material Presets
// ============================================================================

static const MaterialData defaultMaterial = {
    .albedo = { 0.5f, 0.5f, 0.5f, 1.0f },
    .specular = { 0.04f, 0.04f, 0.04f, 1.0f },
    .emission = { 0.0f, 0.0f, 0.0f, 0.0f },
    .roughness = 0.5f,
    .metallic = 0.0f,
    .normalScale = 1.0f,
    .occlusionStrength = 1.0f,
    .flags = 0,
    .albedoTexture = 0,
    .normalTexture = 0,
    .metallicTexture = 0,
    .roughnessTexture = 0,
    .aoTexture = 0,
    .emissiveTexture = 0
};

static const MaterialData metalMaterial = {
    .albedo = { 0.7f, 0.7f, 0.7f, 1.0f },
    .specular = { 1.0f, 1.0f, 1.0f, 1.0f },
    .emission = { 0.0f, 0.0f, 0.0f, 0.0f },
    .roughness = 0.1f,
    .metallic = 1.0f,
    .normalScale = 1.0f,
    .occlusionStrength = 1.0f,
    .flags = 0
};

static const MaterialData glassMaterial = {
    .albedo = { 0.1f, 0.1f, 0.1f, 0.1f },
    .specular = { 0.04f, 0.04f, 0.04f, 1.0f },
    .emission = { 0.0f, 0.0f, 0.0f, 0.0f },
    .roughness = 0.0f,
    .metallic = 0.0f,
    .normalScale = 1.0f,
    .occlusionStrength = 1.0f,
    .flags = MATERIAL_FLAG_ALPHA_BLEND | MATERIAL_FLAG_GLASS
};

static const MaterialData waterMaterial = {
    .albedo = { 0.1f, 0.3f, 0.4f, 0.7f },
    .specular = { 0.04f, 0.04f, 0.04f, 1.0f },
    .emission = { 0.0f, 0.0f, 0.0f, 0.0f },
    .roughness = 0.0f,
    .metallic = 0.0f,
    .normalScale = 0.5f,
    .occlusionStrength = 1.0f,
    .flags = MATERIAL_FLAG_ALPHA_BLEND | MATERIAL_FLAG_WATER
};

// ============================================================================
// Material Analysis Functions
// ============================================================================

static void RTX_AnalyzeStageForPBR(shaderStage_t *stage, rtxMaterial_t *material) {
    if (!stage || !material) {
        return;
    }
    
    MaterialData *data = &material->data;
    
    // Check for texture
    if (stage->bundle[0].image[0]) {
        image_t *image = stage->bundle[0].image[0];
        
        // Get texture name for identification
        const char *name = image->imgName;
        if (name) {
            // Try to identify texture type from name
            if (strstr(name, "_n") || strstr(name, "_normal") || strstr(name, "_nrm")) {
                // Normal map
                data->normalTexture = (uint32_t)(uintptr_t)image->descriptor;
            } else if (strstr(name, "_s") || strstr(name, "_spec") || strstr(name, "_metallic")) {
                // Metallic/Specular map
                data->metallicTexture = (uint32_t)(uintptr_t)image->descriptor;
            } else if (strstr(name, "_r") || strstr(name, "_rough")) {
                // Roughness map
                data->roughnessTexture = (uint32_t)(uintptr_t)image->descriptor;
            } else if (strstr(name, "_ao") || strstr(name, "_occlusion")) {
                // Ambient occlusion
                data->aoTexture = (uint32_t)(uintptr_t)image->descriptor;
            } else if (strstr(name, "_e") || strstr(name, "_emit") || strstr(name, "_glow")) {
                // Emissive map
                data->emissiveTexture = (uint32_t)(uintptr_t)image->descriptor;
                data->flags |= MATERIAL_FLAG_EMISSIVE;
            } else if (!data->albedoTexture) {
                // Assume it's an albedo texture if not already set
                data->albedoTexture = (uint32_t)(uintptr_t)image->descriptor;
            }
        }
    }
    
    // Check for RGB generator types
    if (stage->bundle[0].rgbGen == CGEN_LIGHTING_DIFFUSE) {
        data->flags |= MATERIAL_FLAG_VERTEX_LIGHTING;
    } else if (stage->bundle[0].rgbGen == CGEN_CONST) {
        // Use constant color
        data->albedo[0] = stage->bundle[0].constantColor.rgba[0] / 255.0f;
        data->albedo[1] = stage->bundle[0].constantColor.rgba[1] / 255.0f;
        data->albedo[2] = stage->bundle[0].constantColor.rgba[2] / 255.0f;
    }
    
    // Check alpha settings
    if (stage->bundle[0].alphaGen == AGEN_CONST) {
        data->albedo[3] = stage->bundle[0].constantColor.rgba[3] / 255.0f;
    }
    
    // Check blend modes
    if (stage->stateBits & GLS_SRCBLEND_BITS) {
        unsigned int srcBlend = stage->stateBits & GLS_SRCBLEND_BITS;
        unsigned int dstBlend = stage->stateBits & GLS_DSTBLEND_BITS;
        
        if (srcBlend == GLS_SRCBLEND_SRC_ALPHA && 
            dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA) {
            data->flags |= MATERIAL_FLAG_ALPHA_BLEND;
        }
    }
    
    // Check for alpha test
    if (stage->stateBits & GLS_ATEST_BITS) {
        data->flags |= MATERIAL_FLAG_ALPHA_TEST;
    }
}

static void RTX_AnalyzeShaderStages(shader_t *shader, rtxMaterial_t *material) {
    if (!shader || !material) {
        return;
    }
    
    // Count actual stages
    int numStages = 0;
    for (int i = 0; i < MAX_SHADER_STAGES; i++) {
        if (shader->stages[i]) {
            numStages++;
        } else {
            break;
        }
    }
    
    if (numStages == 0) {
        return;
    }
    
    MaterialData *data = &material->data;
    shaderStage_t *firstStage = shader->stages[0];
    
    // Set base albedo texture from first stage
    if (firstStage && firstStage->bundle[0].image[0]) {
        data->albedoTexture = (uint32_t)(uintptr_t)firstStage->bundle[0].image[0]->descriptor;
    }
    
    // Analyze all stages
    for (int i = 0; i < numStages; i++) {
        shaderStage_t *stage = shader->stages[i];
        if (!stage) continue;
        
        RTX_AnalyzeStageForPBR(stage, material);
        
        // Special multi-texture handling
        if (i > 0 && stage->bundle[0].image[0]) {
            image_t *image = stage->bundle[0].image[0];
            const char *name = image->imgName;
            
            if (name) {
                // Identify texture purpose from name patterns
                if (strstr(name, "normal") || strstr(name, "_n")) {
                    data->normalTexture = (uint32_t)(uintptr_t)image->descriptor;
                } else if (strstr(name, "specular") || strstr(name, "metallic") || strstr(name, "_s")) {
                    data->metallicTexture = (uint32_t)(uintptr_t)image->descriptor;
                } else if (strstr(name, "roughness") || strstr(name, "_r")) {
                    data->roughnessTexture = (uint32_t)(uintptr_t)image->descriptor;
                } else if (strstr(name, "emission") || strstr(name, "glow") || strstr(name, "_e")) {
                    data->emissiveTexture = (uint32_t)(uintptr_t)image->descriptor;
                    data->flags |= MATERIAL_FLAG_EMISSIVE;
                } else if (strstr(name, "occlusion") || strstr(name, "_ao")) {
                    data->aoTexture = (uint32_t)(uintptr_t)image->descriptor;
                }
            }
        }
    }
    
    // Default base color if not set
    if (data->albedo[0] == 0 && data->albedo[1] == 0 && data->albedo[2] == 0) {
        VectorSet(data->albedo, 1.0f, 1.0f, 1.0f);
    }
}

static void RTX_IdentifyMaterialType(shader_t *shader, rtxMaterial_t *material) {
    if (!shader || !material) {
        return;
    }
    
    const char *name = shader->name;
    MaterialData *data = &material->data;
    
    // Identify special surface types
    if (shader->surfaceFlags & SURF_METALSTEPS || strstr(name, "metal")) {
        // Metal surface
        data->metallic = 0.9f;
        data->roughness = 0.2f;
        data->flags |= MATERIAL_FLAG_METAL;
    } else if (shader->surfaceFlags & SURF_SLICK || strstr(name, "ice")) {
        // Ice/slick surface
        data->roughness = 0.05f;
        data->metallic = 0.0f;
    } else if (strstr(name, "glass") || strstr(name, "window")) {
        // Glass surface
        *data = glassMaterial;
        data->flags |= MATERIAL_FLAG_GLASS | MATERIAL_FLAG_ALPHA_BLEND;
    } else if (strstr(name, "water") || strstr(name, "liquid")) {
        // Water surface
        *data = waterMaterial;
        data->flags |= MATERIAL_FLAG_WATER | MATERIAL_FLAG_ALPHA_BLEND;
    } else if (strstr(name, "lava")) {
        // Lava surface
        data->emission[0] = 5.0f;
        data->emission[1] = 2.0f;
        data->emission[2] = 0.5f;
        data->emission[3] = 10.0f; // intensity
        data->flags |= MATERIAL_FLAG_EMISSIVE;
    } else if (strstr(name, "light") || strstr(name, "lamp")) {
        // Light emitting surface
        data->emission[0] = 1.0f;
        data->emission[1] = 1.0f;
        data->emission[2] = 0.9f;
        data->emission[3] = 5.0f;
        data->flags |= MATERIAL_FLAG_EMISSIVE;
    }
    
    // Check shader properties
    if (shader->cullType == CT_TWO_SIDED) {
        data->flags |= MATERIAL_FLAG_TWO_SIDED;
    }
    
    // Sky shaders don't cast shadows
    if (shader->isSky) {
        data->flags |= MATERIAL_FLAG_NO_SHADOWS;
        data->emission[0] = 1.0f;
        data->emission[1] = 1.0f;
        data->emission[2] = 1.0f;
        data->emission[3] = 1.0f;
        data->flags |= MATERIAL_FLAG_EMISSIVE;
    }
}

// ============================================================================
// Public Material API
// ============================================================================

void RTX_InitMaterialCache(void) {
    Com_Memset(&materialCache, 0, sizeof(materialCache));
    
    materialCache.maxMaterials = 1024;
    materialCache.materials = ri.Hunk_Alloc(
        materialCache.maxMaterials * sizeof(rtxMaterial_t), h_low);
    
    if (!materialCache.materials) {
        ri.Error(ERR_FATAL, "Failed to allocate material cache");
    }
    
    // Add default material at index 0
    rtxMaterial_t *defaultMat = &materialCache.materials[0];
    strcpy(defaultMat->name, "*default");
    defaultMat->data = defaultMaterial;
    defaultMat->shader = NULL;
    defaultMat->converted = qtrue;
    materialCache.numMaterials = 1;
    
    ri.Printf(PRINT_ALL, "RTX: Material cache initialized with %d slots\n", 
              materialCache.maxMaterials);
}

void RTX_ShutdownMaterialCache(void) {
    if (materialCache.buffer) {
        vkDestroyBuffer(vk.device, materialCache.buffer, NULL);
        materialCache.buffer = VK_NULL_HANDLE;
    }
    
    if (materialCache.memory) {
        vkFreeMemory(vk.device, materialCache.memory, NULL);
        materialCache.memory = VK_NULL_HANDLE;
    }
    
    Com_Memset(&materialCache, 0, sizeof(materialCache));
}

static rtxMaterial_t* RTX_FindMaterial(shader_t *shader) {
    if (!shader) {
        return &materialCache.materials[0]; // Return default
    }
    
    // Search for existing material
    for (int i = 0; i < materialCache.numMaterials; i++) {
        if (materialCache.materials[i].shader == shader) {
            return &materialCache.materials[i];
        }
    }
    
    return NULL;
}

static rtxMaterial_t* RTX_ConvertShaderToMaterial(shader_t *shader) {
    if (!shader) {
        return &materialCache.materials[0];
    }
    
    // Check if already converted
    rtxMaterial_t *existing = RTX_FindMaterial(shader);
    if (existing) {
        return existing;
    }
    
    // Check if we have space
    if (materialCache.numMaterials >= materialCache.maxMaterials) {
        ri.Printf(PRINT_WARNING, "RTX: Material cache full, using default\n");
        return &materialCache.materials[0];
    }
    
    // Create new material
    rtxMaterial_t *material = &materialCache.materials[materialCache.numMaterials];
    Com_Memset(material, 0, sizeof(rtxMaterial_t));
    
    // Start with default values
    material->data = defaultMaterial;
    
    // Copy name
    Q_strncpyz(material->name, shader->name, sizeof(material->name));
    material->shader = shader;
    
    // Analyze shader for PBR properties
    RTX_AnalyzeShaderStages(shader, material);
    RTX_IdentifyMaterialType(shader, material);
    
    // Apply material overrides if available
    if (shader->material) {
        // The material system is available but we'll skip it for now
        // since material_t structure doesn't have PBR fields yet
    }
    
    material->converted = qtrue;
    materialCache.numMaterials++;
    materialCache.dirty = qtrue;
    
    return material;
}

void RTX_BuildMaterialBuffer(void) {
    if (materialCache.numMaterials == 0) {
        return;
    }
    
    size_t bufferSize = materialCache.numMaterials * sizeof(MaterialData);
    
    // Destroy old buffer if size changed
    if (materialCache.buffer && materialCache.bufferSize != bufferSize) {
        vkDestroyBuffer(vk.device, materialCache.buffer, NULL);
        vkFreeMemory(vk.device, materialCache.memory, NULL);
        materialCache.buffer = VK_NULL_HANDLE;
        materialCache.memory = VK_NULL_HANDLE;
    }
    
    // Create buffer if needed
    if (!materialCache.buffer) {
        VkBufferCreateInfo bufferInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bufferSize,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | 
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        
        VK_CHECK(vkCreateBuffer(vk.device, &bufferInfo, NULL, &materialCache.buffer));
        
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(vk.device, materialCache.buffer, &memReqs);
        
        VkMemoryAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memReqs.size,
            .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        };
        
        VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &materialCache.memory));
        VK_CHECK(vkBindBufferMemory(vk.device, materialCache.buffer, materialCache.memory, 0));
        
        materialCache.bufferSize = bufferSize;
    }
    
    materialCache.dirty = qtrue;
}

void RTX_UploadMaterialBuffer(VkDevice device, VkCommandBuffer commandBuffer,
                              VkBuffer materialBuffer) {
    if (!materialCache.dirty || materialCache.numMaterials == 0) {
        return;
    }
    
    size_t bufferSize = materialCache.numMaterials * sizeof(MaterialData);
    
    // Create staging buffer
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bufferSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    VkBuffer stagingBuffer;
    VK_CHECK(vkCreateBuffer(vk.device, &bufferInfo, NULL, &stagingBuffer));
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, stagingBuffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    
    VkDeviceMemory stagingMemory;
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &stagingMemory));
    VK_CHECK(vkBindBufferMemory(vk.device, stagingBuffer, stagingMemory, 0));
    
    // Copy material data to staging buffer
    void *data;
    VK_CHECK(vkMapMemory(vk.device, stagingMemory, 0, bufferSize, 0, &data));
    
    MaterialData *materials = (MaterialData*)data;
    for (int i = 0; i < materialCache.numMaterials; i++) {
        materials[i] = materialCache.materials[i].data;
    }
    
    vkUnmapMemory(vk.device, stagingMemory);
    
    // Copy from staging to device buffer
    VkBufferCopy copyRegion = {
        .srcOffset = 0,
        .dstOffset = 0,
        .size = bufferSize
    };
    
    vkCmdCopyBuffer(commandBuffer, stagingBuffer, materialBuffer ? materialBuffer : materialCache.buffer, 1, &copyRegion);
    
    // Memory barrier
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    };
    
    vkCmdPipelineBarrier(commandBuffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                        0, 1, &barrier, 0, NULL, 0, NULL);
    
    // Cleanup staging buffer
    vkDestroyBuffer(vk.device, stagingBuffer, NULL);
    vkFreeMemory(vk.device, stagingMemory, NULL);
    
    materialCache.dirty = qfalse;
}

qboolean RTX_IsMaterialCacheDirty(void) {
    return materialCache.dirty;
}

int RTX_GetNumMaterials(void) {
    return materialCache.numMaterials;
}

int RTX_GetMaterialIndex(shader_t *shader) {
    if (!shader) {
        return 0; // Default material
    }
    
    // Convert shader if needed
    rtxMaterial_t *material = RTX_ConvertShaderToMaterial(shader);
    
    // Return index
    return material - materialCache.materials;
}

VkBuffer RTX_GetMaterialBuffer(void) {
    return materialCache.buffer;
}