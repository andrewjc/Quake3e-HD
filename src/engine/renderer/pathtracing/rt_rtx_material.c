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
    uint32_t albedoTexture;     // Texture indices (must match GLSL Material struct order)
    uint32_t normalTexture;
    uint32_t roughnessTexture;
    uint32_t metallicTexture;
    uint32_t emissionTexture;
    uint32_t occlusionTexture;
    uint32_t lightmapTexture;
    uint32_t flags;             // Material flags (two-sided, alpha test, etc.)
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

// Texture registry for RTX descriptor array (binding 12)
#define RTX_MAX_TEXTURES 256
static image_t *rtxTextureImages[RTX_MAX_TEXTURES];
static uint32_t rtxTextureCount = 0;

static uint32_t RTX_RegisterTexture(image_t *image) {
    if (!image || !image->view) {
        return 0; // fallback to slot 0 (default/white)
    }

    // Return existing slot if already registered
    for (uint32_t i = 0; i < rtxTextureCount; ++i) {
        if (rtxTextureImages[i] == image) {
            return i;
        }
    }

    // Append new slot if space
    if (rtxTextureCount >= RTX_MAX_TEXTURES) {
        ri.Printf(PRINT_WARNING, "RTX: Texture registry full (%d); using fallback\n", RTX_MAX_TEXTURES);
        return 0;
    }

    uint32_t slot = rtxTextureCount++;
    rtxTextureImages[slot] = image;
    return slot;
}

uint32_t RTX_GetRegisteredTextureCount(void) {
    return rtxTextureCount;
}

void RTX_FillTextureDescriptorInfos(VkDescriptorImageInfo *outInfos, uint32_t maxInfos,
                                    VkSampler sampler, VkImageView fallbackView) {
    if (!outInfos || maxInfos == 0) {
        return;
    }

    for (uint32_t i = 0; i < maxInfos; ++i) {
        VkImageView view = fallbackView;
        if (i < rtxTextureCount) {
            image_t *img = rtxTextureImages[i];
            if (img && img->view) {
                view = img->view;
            }
        }
        outInfos[i].sampler = sampler;
        outInfos[i].imageView = view;
        outInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

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
    .albedoTexture = 0,
    .normalTexture = 0,
    .roughnessTexture = 0,
    .metallicTexture = 0,
    .emissionTexture = 0,
    .occlusionTexture = 0,
    .lightmapTexture = 0,
    .flags = 0
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
            uint32_t texIndex = RTX_RegisterTexture(image);
            // Try to identify texture type from name
            if (strstr(name, "_n") || strstr(name, "_normal") || strstr(name, "_nrm")) {
                // Normal map
                data->normalTexture = texIndex;
            } else if (strstr(name, "_s") || strstr(name, "_spec") || strstr(name, "_metallic")) {
                // Metallic/Specular map
                data->metallicTexture = texIndex;
            } else if (strstr(name, "_r") || strstr(name, "_rough")) {
                // Roughness map
                data->roughnessTexture = texIndex;
            } else if (strstr(name, "_ao") || strstr(name, "_occlusion")) {
                // Ambient occlusion
                data->occlusionTexture = texIndex;
            } else if (strstr(name, "_e") || strstr(name, "_emit") || strstr(name, "_glow")) {
                // Emissive map
                data->emissionTexture = texIndex;
                data->flags |= MATERIAL_FLAG_EMISSIVE;
            } else if (!data->albedoTexture) {
                // Assume it's an albedo texture if not already set
                data->albedoTexture = texIndex;
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

    if (firstStage && firstStage->bundle[0].image[0]) {
        data->albedoTexture = RTX_RegisterTexture(firstStage->bundle[0].image[0]);
    }

    for (int i = 0; i < numStages; i++) {
        shaderStage_t *stage = shader->stages[i];
        if (!stage) {
            continue;
        }

        RTX_AnalyzeStageForPBR(stage, material);

        if (i > 0 && stage->bundle[0].image[0]) {
            image_t *image = stage->bundle[0].image[0];
            const char *name = image->imgName;
            if (name) {
                uint32_t texIndex = RTX_RegisterTexture(image);
                if (strstr(name, "normal") || strstr(name, "_n")) {
                    data->normalTexture = texIndex;
                } else if (strstr(name, "specular") || strstr(name, "metallic") || strstr(name, "_s")) {
                    data->metallicTexture = texIndex;
                } else if (strstr(name, "roughness") || strstr(name, "_r")) {
                    data->roughnessTexture = texIndex;
                } else if (strstr(name, "emission") || strstr(name, "glow") || strstr(name, "_e")) {
                    data->emissionTexture = texIndex;
                    data->flags |= MATERIAL_FLAG_EMISSIVE;
                } else if (strstr(name, "occlusion") || strstr(name, "_ao")) {
                    data->occlusionTexture = texIndex;
                }
            }
        }
    }

    if (data->albedo[0] == 0 && data->albedo[1] == 0 && data->albedo[2] == 0) {
        VectorSet(data->albedo, 1.0f, 1.0f, 1.0f);
    }
}

#if 0
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
                    data->emissionTexture = (uint32_t)(uintptr_t)image->descriptor;
                    data->flags |= MATERIAL_FLAG_EMISSIVE;
                } else if (strstr(name, "occlusion") || strstr(name, "_ao")) {
                    data->occlusionTexture = (uint32_t)(uintptr_t)image->descriptor;
            }
        }
    }

    // Temporarily disable hardware texture sampling until RTX texture descriptors
    // are fully populated. This prevents the ray tracing shaders from indexing
    // descriptors that have not been bound yet, which was causing device loss.
    data->albedoTexture = 0;
    data->normalTexture = 0;
    data->metallicTexture = 0;
    data->roughnessTexture = 0;
    data->occlusionTexture = 0;
    data->emissionTexture = 0;
}
    
    // Default base color if not set
    if (data->albedo[0] == 0 && data->albedo[1] == 0 && data->albedo[2] == 0) {
        VectorSet(data->albedo, 1.0f, 1.0f, 1.0f);
    }
}

#endif // legacy RTX_AnalyzeShaderStages (disabled)

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
    rtxTextureCount = 0;
    
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
        // Ensure no in-flight commands still reference the old material buffer
        vkQueueWaitIdle(vk.queue);
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
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        
        VK_CHECK(vkCreateBuffer(vk.device, &bufferInfo, NULL, &materialCache.buffer));
        
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(vk.device, materialCache.buffer, &memReqs);
        
        VkMemoryAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memReqs.size,
            .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
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
    VkBuffer targetBuffer = materialBuffer ? materialBuffer : materialCache.buffer;

    if (!targetBuffer || !materialCache.memory) {
        return;
    }

    // Material buffer is HOST_VISIBLE — write directly, no staging needed.
    // This avoids the use-after-free that occurred when a staging buffer was
    // destroyed before the command buffer referencing it was submitted.
    void *data = NULL;
    VkResult result = vkMapMemory(vk.device, materialCache.memory, 0, bufferSize, 0, &data);
    if (result != VK_SUCCESS || !data) {
        ri.Printf(PRINT_WARNING, "RTX_UploadMaterialBuffer: vkMapMemory failed (%d)\n", result);
        return;
    }
    
    MaterialData *materials = (MaterialData*)data;
    for (int i = 0; i < materialCache.numMaterials; i++) {
        materials[i] = materialCache.materials[i].data;
    }
    
    vkUnmapMemory(vk.device, materialCache.memory);
    
    materialCache.dirty = qfalse;
}

qboolean RTX_IsMaterialCacheDirty(void) {
    return materialCache.dirty;
}

int RTX_GetNumMaterials(void) {
    return materialCache.numMaterials;
}

qboolean RTX_GetShaderBaseColor(const shader_t *shader, vec3_t outColor) {
    if (!shader || !outColor) {
        return qfalse;
    }

    if (!shader->material) {
        return qfalse;
    }

    VectorCopy(shader->material->baseColor, outColor);
    return VectorLengthSquared(outColor) > 0.0f;
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

qboolean RTX_GetMaterialEmission(uint32_t materialIndex, vec3_t outColor, float *outIntensity) {
    if (materialIndex >= (uint32_t)materialCache.numMaterials) {
        return qfalse;
    }

    MaterialData *data = &materialCache.materials[materialIndex].data;

    vec3_t emissionColor = {
        data->emission[0],
        data->emission[1],
        data->emission[2]
    };
    float emissionIntensity = data->emission[3];

    vec3_t luminous;
    VectorScale(emissionColor, emissionIntensity, luminous);

    float magnitude = VectorLength(luminous);
    if (magnitude <= 0.0001f) {
        return qfalse;
    }

    if (outColor) {
        VectorCopy(luminous, outColor);
    }
    if (outIntensity) {
        *outIntensity = magnitude;
    }

    return qtrue;
}
