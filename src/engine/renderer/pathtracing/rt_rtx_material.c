/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

RTX Material System
Converts Quake3 shaders to PBR materials for ray tracing
===========================================================================
*/

#include "rt_rtx.h"
#include "rt_pathtracer.h"
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
    uint32_t heightTexture;     // parallax-occlusion height map (0 = none)
    float    heightScale;       // parallax depth (UV-span fraction); must match GLSL Material
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

// Texture registry for RTX descriptor array (binding 12); capacity is
// RTX_MAX_TEXTURES from rt_rtx.h, shared with the descriptor layout.
static image_t *rtxTextureImages[RTX_MAX_TEXTURES];
static uint32_t rtxTextureCount = 0;

// Returns a 1-based texture handle for MaterialData: shaders treat 0 as
// "no texture" and sample textures[handle - 1], so slot N in the registry
// (descriptor array element N) is exposed as handle N+1.
static uint32_t RTX_RegisterTexture(image_t *image) {
    if (!image || !image->view) {
        return 0; // no texture
    }

    // Return existing handle if already registered
    for (uint32_t i = 0; i < rtxTextureCount; ++i) {
        if (rtxTextureImages[i] == image) {
            return i + 1;
        }
    }

    // Append new slot if space
    if (rtxTextureCount >= RTX_MAX_TEXTURES) {
        ri.Printf(PRINT_WARNING, "RTX: Texture registry full (%d); using fallback\n", RTX_MAX_TEXTURES);
        return 0;
    }

    uint32_t slot = rtxTextureCount++;
    rtxTextureImages[slot] = image;
    return slot + 1;
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

// PBR map kind derived from a texture's naming convention. Only exact
// basename suffixes count — substring tests misclassify ordinary Quake
// texture names (e.g. "base_support" contains "_s").
typedef enum {
    RTX_TEXKIND_ALBEDO,
    RTX_TEXKIND_NORMAL,
    RTX_TEXKIND_METALLIC,
    RTX_TEXKIND_ROUGHNESS,
    RTX_TEXKIND_OCCLUSION,
    RTX_TEXKIND_EMISSION
} rtxTextureKind_t;

static qboolean RTX_NameHasSuffix(const char *name, const char *suffix) {
    // Compare against the basename with any extension stripped
    const char *end = name + strlen(name);
    const char *dot = strrchr(name, '.');
    if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) {
        end = dot;
    }

    size_t suffixLen = strlen(suffix);
    size_t nameLen = (size_t)(end - name);
    if (nameLen < suffixLen) {
        return qfalse;
    }
    return Q_stricmpn(end - suffixLen, suffix, (int)suffixLen) == 0 ? qtrue : qfalse;
}

static rtxTextureKind_t RTX_ClassifyTextureName(const char *name) {
    if (RTX_NameHasSuffix(name, "_n") || RTX_NameHasSuffix(name, "_nrm") ||
        RTX_NameHasSuffix(name, "_normal") || RTX_NameHasSuffix(name, "_local")) {
        return RTX_TEXKIND_NORMAL;
    }
    if (RTX_NameHasSuffix(name, "_s") || RTX_NameHasSuffix(name, "_spec") ||
        RTX_NameHasSuffix(name, "_metallic") || RTX_NameHasSuffix(name, "_metal")) {
        return RTX_TEXKIND_METALLIC;
    }
    if (RTX_NameHasSuffix(name, "_r") || RTX_NameHasSuffix(name, "_rough") ||
        RTX_NameHasSuffix(name, "_roughness")) {
        return RTX_TEXKIND_ROUGHNESS;
    }
    if (RTX_NameHasSuffix(name, "_ao") || RTX_NameHasSuffix(name, "_occlusion")) {
        return RTX_TEXKIND_OCCLUSION;
    }
    if (RTX_NameHasSuffix(name, "_glow") || RTX_NameHasSuffix(name, "_emit") ||
        RTX_NameHasSuffix(name, "_emission") || RTX_NameHasSuffix(name, "_luma")) {
        return RTX_TEXKIND_EMISSION;
    }
    return RTX_TEXKIND_ALBEDO;
}

static void RTX_AnalyzeStageForPBR(shaderStage_t *stage, rtxMaterial_t *material) {
    if (!stage || !material) {
        return;
    }

    MaterialData *data = &material->data;

    // Lightmap stages carry the map's baked scene lighting, not surface
    // material data. The path tracer computes its own lighting, so a lightmap
    // stage must never become the albedo (that was giving light fixtures and
    // trims a lightmap texture — or none — instead of their real diffuse map).
    if (stage->bundle[0].isLightmap || stage->bundle[0].tcGen == TCGEN_LIGHTMAP) {
        return;
    }

    // Additive stages (blend dst = GL_ONE) are self-illuminated glow, not
    // surface colour: the "*.blend" glow maps on light fixtures, and beam /
    // flame / lava-crust effects. Their texture is the emission map. Routing it
    // here — instead of treating it as opaque albedo — is what stops these
    // surfaces rendering as flat white blobs; the glow texture then modulates
    // the emission so the fixture shows its real pattern and colour.
    unsigned int dstBlend = stage->stateBits & GLS_DSTBLEND_BITS;
    // Environment-mapped additive stages are chrome reflections, not glow -
    // exclude them so shiny trims are not mistaken for light emitters.
    qboolean isAdditive = (dstBlend == GLS_DSTBLEND_ONE) &&
                          stage->bundle[0].tcGen != TCGEN_ENVIRONMENT_MAPPED;

    // Check for texture
    if (stage->bundle[0].image[0]) {
        image_t *image = stage->bundle[0].image[0];

        // Get texture name for identification
        const char *name = image->imgName;
        if (name) {
            uint32_t texIndex = RTX_RegisterTexture(image);
            if (isAdditive) {
                if (!data->emissionTexture) {
                    data->emissionTexture = texIndex;
                }
                data->flags |= MATERIAL_FLAG_EMISSIVE;
            } else {
                switch (RTX_ClassifyTextureName(name)) {
                case RTX_TEXKIND_NORMAL:
                    data->normalTexture = texIndex;
                    break;
                case RTX_TEXKIND_METALLIC:
                    data->metallicTexture = texIndex;
                    break;
                case RTX_TEXKIND_ROUGHNESS:
                    data->roughnessTexture = texIndex;
                    break;
                case RTX_TEXKIND_OCCLUSION:
                    data->occlusionTexture = texIndex;
                    break;
                case RTX_TEXKIND_EMISSION:
                    if (!data->emissionTexture) {
                        data->emissionTexture = texIndex;
                    }
                    data->flags |= MATERIAL_FLAG_EMISSIVE;
                    break;
                default:
                    if (!data->albedoTexture) {
                        data->albedoTexture = texIndex;
                    }
                    break;
                }
            }
        }
    }

    // Check for RGB generator types. A constant colour only defines the surface
    // albedo on a non-additive (opaque) stage; on a glow stage it tints the
    // emission, which the glow texture already carries.
    if (stage->bundle[0].rgbGen == CGEN_LIGHTING_DIFFUSE) {
        data->flags |= MATERIAL_FLAG_VERTEX_LIGHTING;
    } else if (stage->bundle[0].rgbGen == CGEN_CONST && !isAdditive) {
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

    // Each stage classifies itself (lightmap skipped, additive glow -> emission,
    // otherwise diffuse/PBR by name suffix). The albedo comes from the first
    // real diffuse stage, not blindly from stage 0 (which is often a lightmap).
    for (int i = 0; i < numStages; i++) {
        shaderStage_t *stage = shader->stages[i];
        if (!stage) {
            continue;
        }

        RTX_AnalyzeStageForPBR(stage, material);
    }

    // A shader with no diffuse texture (e.g. all-lightmap or colour-only) keeps
    // the neutral default albedo. Do NOT force it to white: a texture-less
    // surface blown to (1,1,1) reads as a solid white panel once it is lit.
}

static void RTX_IdentifyMaterialType(shader_t *shader, rtxMaterial_t *material) {
    if (!shader || !material) {
        return;
    }
    
    const char *name = shader->name;
    MaterialData *data = &material->data;

    // Identify special surface types. Lava is tested FIRST (and by content flag,
    // not just name) so that textures/liquids/lava* is not caught by the generic
    // "liquid" water match below and turned into blue water.
    if ((shader->contentFlags & CONTENTS_LAVA) || strstr(name, "lava")) {
        // Lava glows with its own animated texture; the hit shader modulates
        // the emission colour by that map, so use the surface's own texture as
        // the emission source instead of a flat colour.
        data->emission[0] = 3.0f;
        data->emission[1] = 1.2f;
        data->emission[2] = 0.35f;
        data->emission[3] = 3.0f;
        if (!data->emissionTexture && data->albedoTexture) {
            data->emissionTexture = data->albedoTexture;
        }
        data->roughness = 0.7f;
        data->flags |= MATERIAL_FLAG_EMISSIVE;
    } else if (shader->surfaceFlags & SURF_METALSTEPS || strstr(name, "metal")) {
        // Metal surface
        data->metallic = 0.9f;
        data->roughness = 0.2f;
        data->flags |= MATERIAL_FLAG_METAL;
    } else if (shader->surfaceFlags & SURF_SLICK || strstr(name, "ice")) {
        // Ice/slick surface
        data->roughness = 0.05f;
        data->metallic = 0.0f;
    } else if (strstr(name, "glass")) {
        // Real glass only. "window" is deliberately NOT matched: most gothic /
        // idbase "window" textures are opaque decorative art (often self-lit
        // via an additive stage), and forcing them to clear glass dropped their
        // texture and rendered them as dark reflective panels. Non-destructive:
        // keep the analyzed albedo/emission and just add glass optics.
        data->roughness = 0.0f;
        data->metallic = 0.0f;
        data->flags |= MATERIAL_FLAG_GLASS | MATERIAL_FLAG_ALPHA_BLEND;
    } else if (strstr(name, "water") || strstr(name, "liquid")) {
        // Water / liquid: keep the surface's own texture (raygen's refractive
        // path supplies its own tint) and mark it for reflect + refract.
        data->roughness = 0.0f;
        data->flags |= MATERIAL_FLAG_WATER | MATERIAL_FLAG_ALPHA_BLEND;
    } else if (strstr(name, "light") || strstr(name, "lamp")) {
        // Named light fixture: emits its own colour. The glow/diffuse texture
        // (assigned during stage analysis) modulates the emission so the
        // fixture shows its real pattern instead of a solid white blob. If no
        // dedicated glow stage was found, the fixture's own diffuse map carries
        // the emission.
        //
        // The self-emission is kept modest and warm: at high intensity a plain
        // white light panel (e.g. base_light/light1) saturates to a flat white
        // rectangle that reads as a bug. The room illumination it casts comes
        // from the separately spawned static lights (auto-scaled to the target
        // average), so a lower visible glow does not darken the map.
        data->emission[0] = 1.0f;
        data->emission[1] = 0.9f;
        data->emission[2] = 0.72f;
        data->emission[3] = 2.0f;
        if (!data->emissionTexture && data->albedoTexture) {
            data->emissionTexture = data->albedoTexture;
        }
        data->flags |= MATERIAL_FLAG_EMISSIVE;
    }

    // A surface flagged emissive by stage analysis (an additive glow stage:
    // beam, flame, back-lit window) but with no emission colour assigned yet
    // gets a modest default, modulated by its glow texture, so it actually
    // glows instead of rendering as a dull grey panel.
    if ((data->flags & MATERIAL_FLAG_EMISSIVE) && data->emission[3] <= 0.0f) {
        data->emission[0] = 1.0f;
        data->emission[1] = 1.0f;
        data->emission[2] = 1.0f;
        data->emission[3] = 2.0f;
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

/*
================
RTX_LoadCompanionPBRMaps

Probe the filesystem for generated PBR maps that accompany the material's
albedo texture (tools/pbrgen.py emits them):
    <albedo>_n     tangent-space normal
    <albedo>_r     roughness
    <albedo>_metal metallic mask
    <albedo>_ao    ambient occlusion
Slots already claimed by explicit shader stages are left alone. When a map
is found, the corresponding scalar becomes a 1.0 multiplier so the texture
carries the value (the analyzer's name-based defaults would otherwise
scale it down or zero it out).
================
*/
static void RTX_LoadCompanionPBRMaps(rtxMaterial_t *material) {
    if (!rt_pbrMaps || !rt_pbrMaps->integer) {
        return;
    }

    MaterialData *data = &material->data;
    if (!data->albedoTexture || data->albedoTexture > rtxTextureCount) {
        return;
    }

    const image_t *albedo = rtxTextureImages[data->albedoTexture - 1];
    if (!albedo || !albedo->imgName[0]) {
        return;
    }

    char base[MAX_QPATH];
    COM_StripExtension(albedo->imgName, base, sizeof(base));

    if (!data->normalTexture) {
        image_t *img = R_FindImageFile(va("%s_n", base),
            IMGFLAG_MIPMAP | IMGFLAG_NOLIGHTSCALE | IMGFLAG_NORMALMAP);
        if (img) {
            data->normalTexture = RTX_RegisterTexture(img);
        }
    }

    if (!data->roughnessTexture) {
        image_t *img = R_FindImageFile(va("%s_r", base),
            IMGFLAG_MIPMAP | IMGFLAG_NOLIGHTSCALE);
        if (img) {
            data->roughnessTexture = RTX_RegisterTexture(img);
            data->roughness = 1.0f;
        }
    }

    if (!data->metallicTexture) {
        image_t *img = R_FindImageFile(va("%s_metal", base),
            IMGFLAG_MIPMAP | IMGFLAG_NOLIGHTSCALE);
        if (img) {
            data->metallicTexture = RTX_RegisterTexture(img);
            data->metallic = 1.0f;
        }
    }

    if (!data->occlusionTexture) {
        image_t *img = R_FindImageFile(va("%s_ao", base),
            IMGFLAG_MIPMAP | IMGFLAG_NOLIGHTSCALE);
        if (img) {
            data->occlusionTexture = RTX_RegisterTexture(img);
            data->occlusionStrength = 1.0f;
        }
    }

    // Parallax-occlusion height map (rt_parallax). Loaded here so the height
    // texture only registers when parallax is on; heightScale carries the
    // global depth into the closest-hit shader (mat.heightScale). Presence of
    // heightTexture is itself the per-material POM enable.
    if (rt_parallax && rt_parallax->integer && !data->heightTexture) {
        image_t *img = R_FindImageFile(va("%s_h", base),
            IMGFLAG_MIPMAP | IMGFLAG_NOLIGHTSCALE);
        if (img) {
            data->heightTexture = RTX_RegisterTexture(img);
            data->heightScale = (rt_parallaxScale && rt_parallaxScale->value > 0.0f)
                                ? rt_parallaxScale->value : 0.04f;
        }
    }
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
    RTX_LoadCompanionPBRMaps(material);
    
    // Apply material overrides if available
    if (shader->material) {
        // The material system is available but we'll skip it for now
        // since material_t structure doesn't have PBR fields yet
    }
    
    material->converted = qtrue;
    materialCache.numMaterials++;
    materialCache.dirty = qtrue;

    // Diagnostic (rt_debugMaterials 1): list materials that resolve to no
    // diffuse texture (render as flat mat.albedo) or that carry emission, so
    // white/blown surfaces can be traced back to their shader.
    if (ri.Cvar_VariableIntegerValue("rt_debugMaterials")) {
        const MaterialData *d = &material->data;
        float emitMag = (d->emission[0] + d->emission[1] + d->emission[2]) * d->emission[3];
        if (d->albedoTexture == 0 || emitMag > 0.0001f) {
            ri.Printf(PRINT_ALL,
                "RTX mat: %-40s albedoTex=%u emitTex=%u albedo=(%.2f %.2f %.2f) "
                "emit=(%.2f %.2f %.2f)x%.2f flags=0x%x\n",
                shader->name, d->albedoTexture, d->emissionTexture,
                d->albedo[0], d->albedo[1], d->albedo[2],
                d->emission[0], d->emission[1], d->emission[2], d->emission[3],
                d->flags);
            for (int si = 0; si < MAX_SHADER_STAGES && shader->stages[si]; si++) {
                shaderStage_t *st = shader->stages[si];
                unsigned int db = st->stateBits & GLS_DSTBLEND_BITS;
                ri.Printf(PRINT_ALL,
                    "    stage %d: img=%-32s lightmap=%d tcGen=%d dstBlend=0x%x\n",
                    si, (st->bundle[0].image[0] ? st->bundle[0].image[0]->imgName : "(none)"),
                    (int)st->bundle[0].isLightmap, (int)st->bundle[0].tcGen, db);
            }
        }
    }

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
        materialCache.dirty = qtrue;
    }
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

    // Material buffer is HOST_VISIBLE and only changes when new shaders are
    // registered (world load / precache), so a mapped write is used. In-flight
    // frames may still be reading the buffer at that point, so drain the queue
    // before overwriting.
    vkQueueWaitIdle(vk.queue);
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
