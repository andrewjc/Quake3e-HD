/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Material Override System Implementation
Automatic PBR map generation using advanced heuristics
===========================================================================
*/

#include "../tr_local.h"
#include "tr_material.h"
#include "tr_material_override.h"
#include <stdio.h>
#include <math.h>
#ifdef _WIN32
#include <direct.h>  // For _mkdir
#else
#include <sys/stat.h>  // For mkdir
#include <sys/types.h>
#endif

// Helper macros
#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : (x) > (max) ? (max) : (x))
#endif

// Global system state
materialOverrideSystem_t matOverrideSystem;

// Forward declarations
static byte *MatOver_LoadTextureData(image_t *image, int *width, int *height);
static void MatOver_FreeTextureData(byte *data);
static void R_ResampleTexture(const byte *in, int inwidth, int inheight,
                               byte *out, int outwidth, int outheight);
void R_LoadTGA(const char *name, byte **pic, int *width, int *height);
void R_LoadJPG(const char *filename, unsigned char **pic, int *width, int *height);

// CVARs
cvar_t *r_materialOverride;
cvar_t *r_materialAutoGen;
cvar_t *r_materialExport;
cvar_t *r_materialGenQuality;
cvar_t *r_materialGenResolution;
cvar_t *r_materialGenAsync;
cvar_t *r_materialCacheSize;
cvar_t *r_materialDebug;

// Generation parameter CVARs
cvar_t *r_matgen_sobelStrength;
cvar_t *r_matgen_sobelThreshold;
cvar_t *r_matgen_luminanceWeight;
cvar_t *r_matgen_frequencyScale;
cvar_t *r_matgen_chromaticThreshold;
cvar_t *r_matgen_contrastRadius;
cvar_t *r_matgen_fusionNormal;
cvar_t *r_matgen_fusionRoughness;
cvar_t *r_matgen_fusionMetallic;

// ============================================================================
// System Initialization
// ============================================================================

/*
================
MatOver_Init

Initialize material override system
================
*/
void MatOver_Init(void) {
    ri.Printf(PRINT_ALL, "^2[MATOVER] Initializing Material Override System...\n");
    Com_Memset(&matOverrideSystem, 0, sizeof(matOverrideSystem));
    
    // Register CVARs
    ri.Printf(PRINT_ALL, "^2[MATOVER] Registering CVARs...\n");
    r_materialOverride = ri.Cvar_Get("r_materialOverride", "1", CVAR_ARCHIVE);
    r_materialAutoGen = ri.Cvar_Get("r_materialAutoGen", "1", CVAR_ARCHIVE);
    r_materialExport = ri.Cvar_Get("r_materialExport", "0", CVAR_ARCHIVE);
    r_materialGenQuality = ri.Cvar_Get("r_materialGenQuality", "2", CVAR_ARCHIVE);
    r_materialGenResolution = ri.Cvar_Get("r_materialGenResolution", "512", CVAR_ARCHIVE);
    r_materialGenAsync = ri.Cvar_Get("r_materialGenAsync", "1", CVAR_ARCHIVE);
    r_materialCacheSize = ri.Cvar_Get("r_materialCacheSize", "32", CVAR_ARCHIVE);  // 32MB default
    r_materialDebug = ri.Cvar_Get("r_materialDebug", "0", CVAR_CHEAT);
    
    // Generation parameters
    r_matgen_sobelStrength = ri.Cvar_Get("r_matgen_sobelStrength", "1.0", CVAR_ARCHIVE);
    r_matgen_sobelThreshold = ri.Cvar_Get("r_matgen_sobelThreshold", "0.1", CVAR_ARCHIVE);
    r_matgen_luminanceWeight = ri.Cvar_Get("r_matgen_luminanceWeight", "0.7", CVAR_ARCHIVE);
    r_matgen_frequencyScale = ri.Cvar_Get("r_matgen_frequencyScale", "1.0", CVAR_ARCHIVE);
    r_matgen_chromaticThreshold = ri.Cvar_Get("r_matgen_chromaticThreshold", "0.3", CVAR_ARCHIVE);
    r_matgen_contrastRadius = ri.Cvar_Get("r_matgen_contrastRadius", "5.0", CVAR_ARCHIVE);
    r_matgen_fusionNormal = ri.Cvar_Get("r_matgen_fusionNormal", "0.6:0.4", CVAR_ARCHIVE);
    r_matgen_fusionRoughness = ri.Cvar_Get("r_matgen_fusionRoughness", "0.5:0.5", CVAR_ARCHIVE);
    r_matgen_fusionMetallic = ri.Cvar_Get("r_matgen_fusionMetallic", "0.7:0.3", CVAR_ARCHIVE);
    
    ri.Printf(PRINT_ALL, "^2[MATOVER] CVARs registered: r_materialExport=%d, r_materialAutoGen=%d\n", 
              r_materialExport->integer, r_materialAutoGen->integer);
    
    // Initialize default generation parameters
    matOverrideSystem.defaultParams.sobelStrength = 1.0f;
    matOverrideSystem.defaultParams.sobelThreshold = 0.1f;
    matOverrideSystem.defaultParams.sobelKernelSize = 3;
    matOverrideSystem.defaultParams.luminanceWeight = 0.7f;
    matOverrideSystem.defaultParams.luminanceInvert = 0.0f;
    matOverrideSystem.defaultParams.luminanceGamma = 2.2f;
    matOverrideSystem.defaultParams.frequencyScale = 1.0f;
    matOverrideSystem.defaultParams.frequencyCutoff = 0.5f;
    matOverrideSystem.defaultParams.frequencyOctaves = 4;
    matOverrideSystem.defaultParams.chromaticThreshold = 0.3f;
    matOverrideSystem.defaultParams.chromaticSaturation = 0.2f;
    VectorSet(matOverrideSystem.defaultParams.chromaticBias, 0.8f, 0.8f, 0.9f);
    matOverrideSystem.defaultParams.contrastRadius = 5.0f;
    matOverrideSystem.defaultParams.contrastStrength = 1.0f;
    matOverrideSystem.defaultParams.contrastBias = 0.5f;
    
    // Fusion weights for Quake's art style
    matOverrideSystem.defaultParams.fusionWeights[0] = 0.35f; // Sobel
    matOverrideSystem.defaultParams.fusionWeights[1] = 0.25f; // Luminance
    matOverrideSystem.defaultParams.fusionWeights[2] = 0.20f; // Frequency
    matOverrideSystem.defaultParams.fusionWeights[3] = 0.10f; // Chromatic
    matOverrideSystem.defaultParams.fusionWeights[4] = 0.10f; // Contrast
    
    // Quality settings
    matOverrideSystem.defaultParams.iterations = 2;
    matOverrideSystem.defaultParams.samples = 16;
    matOverrideSystem.defaultParams.denoise = 0.3f;
    
    // Initialize cache with sanity check
    int cacheSize = r_materialCacheSize->integer;
    if (cacheSize > 64) {
        ri.Printf(PRINT_WARNING, "r_materialCacheSize %d MB exceeds maximum, capping at 64 MB\n", cacheSize);
        ri.Cvar_SetValue("r_materialCacheSize", 32);  // Reset to safe default
        cacheSize = 32;
    }
    MatOver_InitCache(cacheSize);
    
    matOverrideSystem.autoGenerate = r_materialAutoGen->integer ? qtrue : qfalse;
    matOverrideSystem.exportEnabled = r_materialExport->integer ? qtrue : qfalse;
    matOverrideSystem.asyncGeneration = r_materialGenAsync->integer ? qtrue : qfalse;
    
    // Register console commands
    ri.Cmd_AddCommand("mat_regenerate", MatOver_RegenerateAll_f);
    ri.Cmd_AddCommand("mat_export", MatOver_ExportAll_f);
    ri.Cmd_AddCommand("mat_clear", MatOver_ClearCache_f);
    ri.Cmd_AddCommand("mat_status", MatOver_Status_f);
    
    ri.Printf(PRINT_ALL, "Material Override System initialized\n");
    ri.Printf(PRINT_ALL, "  Auto-generation: %s\n", matOverrideSystem.autoGenerate ? "enabled" : "disabled");
    ri.Printf(PRINT_ALL, "  Export: %s\n", matOverrideSystem.exportEnabled ? "enabled" : "disabled");
    ri.Printf(PRINT_ALL, "  Cache size: %d MB\n", r_materialCacheSize->integer);
}

/*
================
MatOver_Shutdown

Cleanup material override system
================
*/
void MatOver_Shutdown(void) {
    MatOver_FreeCache();
    
    // Free all overrides
    for (int i = 0; i < matOverrideSystem.numOverrides; i++) {
        if (matOverrideSystem.overrides[i]) {
            Z_Free(matOverrideSystem.overrides[i]);
        }
    }
    
    Com_Memset(&matOverrideSystem, 0, sizeof(matOverrideSystem));
}

// ============================================================================
// Override Loading
// ============================================================================

/*
================
MatOver_Load

Load or create material override
================
*/
materialOverride_t* MatOver_Load(const char *materialName) {
    materialOverride_t *override;
    char path[MAX_QPATH];
    
    ri.Printf(PRINT_ALL, "^3[MATOVER] MatOver_Load called for: %s\n", materialName);
    
    if (!r_materialOverride->integer) {
        ri.Printf(PRINT_ALL, "^3[MATOVER] Material override disabled\n");
        return NULL;
    }
    
    // Check if already loaded
    override = MatOver_FindInHash(materialName);
    if (override && (override->flags & MATOVER_LOADED)) {
        return override;
    }
    
    // Try to load from disk
    Com_sprintf(path, sizeof(path), "%s/%s", 
                MATERIAL_OVERRIDE_PATH, materialName);
    ri.Printf(PRINT_ALL, "^3[MATOVER] Looking for override at: %s\n", path);
    
    if (MatOver_FileExists(va("%s/material.txt", path))) {
        if (!override) {
            override = Z_Malloc(sizeof(materialOverride_t));
            Q_strncpyz(override->name, materialName, sizeof(override->name));
            MatOver_AddToHash(override);
        }
        
        if (MatOver_LoadProperties(override, path)) {
            MatOver_LoadMaps(override, path);
            override->flags |= MATOVER_LOADED | MATOVER_CUSTOM;
            
            if (r_materialDebug->integer) {
                ri.Printf(PRINT_ALL, "Loaded material override: %s\n", materialName);
            }
            
            return override;
        }
    }
    
    // Auto-generate if enabled and export mode is on
    ri.Printf(PRINT_ALL, "^3[MATOVER] Auto-gen check: autoGenerate=%d, exportEnabled=%d\n",
              matOverrideSystem.autoGenerate, matOverrideSystem.exportEnabled);
    if (matOverrideSystem.autoGenerate || matOverrideSystem.exportEnabled) {
        // For now, always try to create override even without Material_Find
        material_t *baseMat = Material_Find(materialName);
        ri.Printf(PRINT_ALL, "^3[MATOVER] Found base material: %s = %p\n", materialName, baseMat);
        
        // Create a dummy material if not found
        if (!baseMat) {
            ri.Printf(PRINT_ALL, "^3[MATOVER] Creating dummy material for: %s\n", materialName);
            baseMat = Z_Malloc(sizeof(material_t));
            Q_strncpyz(baseMat->name, materialName, sizeof(baseMat->name));
            // Try to find the shader/texture image directly
            baseMat->stages[0].colorMap = R_FindImageFile(materialName, IMGFLAG_NONE);
            if (!baseMat->stages[0].colorMap) {
                // Try common texture paths
                char texPath[MAX_QPATH];
                Com_sprintf(texPath, sizeof(texPath), "textures/%s", materialName);
                baseMat->stages[0].colorMap = R_FindImageFile(texPath, IMGFLAG_NONE);
            }
        }
        
        if (baseMat) {
            override = MatOver_Create(baseMat);
            ri.Printf(PRINT_ALL, "^3[MATOVER] Created override: %p\n", override);
            if (override) {
                ri.Printf(PRINT_ALL, "^3[MATOVER] Queueing generation for: %s\n", materialName);
                MatOver_QueueGeneration(override);
                return override;
            }
        }
    }
    
    return NULL;
}

/*
================
MatOver_LoadProperties

Load PBR properties from material.txt
================
*/
qboolean MatOver_LoadProperties(materialOverride_t *override, const char *path) {
    char filename[MAX_QPATH];
    char buffer[4096];
    char *token, *text;
    int len;
    
    Com_sprintf(filename, sizeof(filename), "%s/material.txt", path);
    
    void *fileBuffer = NULL;
    len = ri.FS_ReadFile(filename, &fileBuffer);
    if (len <= 0 || !fileBuffer) {
        return qfalse;
    }
    
    if (len >= sizeof(buffer)) {
        ri.FS_FreeFile(fileBuffer);
        return qfalse;
    }
    
    Com_Memcpy(buffer, fileBuffer, len);
    buffer[len] = 0;
    ri.FS_FreeFile(fileBuffer);
    
    // Parse properties
    text = buffer;
    while (1) {
        token = COM_ParseExt(&text, qtrue);
        if (!token[0]) {
            break;
        }
        
        // Base properties
        if (!Q_stricmp(token, "baseColor")) {
            override->pbr.baseColor[0] = atof(COM_ParseExt(&text, qfalse));
            override->pbr.baseColor[1] = atof(COM_ParseExt(&text, qfalse));
            override->pbr.baseColor[2] = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "metallic")) {
            override->pbr.metallic = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "roughness")) {
            override->pbr.roughness = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "specular")) {
            override->pbr.specular = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "normalStrength")) {
            override->pbr.normalStrength = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "heightScale")) {
            override->pbr.heightScale = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "occlusionStrength")) {
            override->pbr.occlusionStrength = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "emissiveColor")) {
            override->pbr.emissiveColor[0] = atof(COM_ParseExt(&text, qfalse));
            override->pbr.emissiveColor[1] = atof(COM_ParseExt(&text, qfalse));
            override->pbr.emissiveColor[2] = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "emissiveIntensity")) {
            override->pbr.emissiveIntensity = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "subsurface")) {
            override->pbr.subsurface = atof(COM_ParseExt(&text, qfalse));
        }
        else if (!Q_stricmp(token, "anisotropy")) {
            override->pbr.anisotropy = atof(COM_ParseExt(&text, qfalse));
        }
        // Map paths
        else if (!Q_stricmp(token, "diffuseMap")) {
            Q_strncpyz(override->mapPaths[MATMAP_DIFFUSE], COM_ParseExt(&text, qfalse), MAX_QPATH);
        }
        else if (!Q_stricmp(token, "normalMap")) {
            Q_strncpyz(override->mapPaths[MATMAP_NORMAL], COM_ParseExt(&text, qfalse), MAX_QPATH);
        }
        else if (!Q_stricmp(token, "specularMap")) {
            Q_strncpyz(override->mapPaths[MATMAP_SPECULAR], COM_ParseExt(&text, qfalse), MAX_QPATH);
        }
        else if (!Q_stricmp(token, "roughnessMap")) {
            Q_strncpyz(override->mapPaths[MATMAP_ROUGHNESS], COM_ParseExt(&text, qfalse), MAX_QPATH);
        }
        else if (!Q_stricmp(token, "metallicMap")) {
            Q_strncpyz(override->mapPaths[MATMAP_METALLIC], COM_ParseExt(&text, qfalse), MAX_QPATH);
        }
        else if (!Q_stricmp(token, "heightMap")) {
            Q_strncpyz(override->mapPaths[MATMAP_HEIGHT], COM_ParseExt(&text, qfalse), MAX_QPATH);
        }
        else if (!Q_stricmp(token, "occlusionMap")) {
            Q_strncpyz(override->mapPaths[MATMAP_OCCLUSION], COM_ParseExt(&text, qfalse), MAX_QPATH);
        }
        else if (!Q_stricmp(token, "emissionMap")) {
            Q_strncpyz(override->mapPaths[MATMAP_EMISSION], COM_ParseExt(&text, qfalse), MAX_QPATH);
        }
    }
    
    return qtrue;
}

/*
================
MatOver_LoadMaps

Load texture maps for material
================
*/
qboolean MatOver_LoadMaps(materialOverride_t *override, const char *path) {
    char filename[MAX_QPATH];
    int loaded = 0;
    
    static const char *mapNames[MATMAP_COUNT] = {
        "diffuse", "normal", "specular", "roughness", "metallic",
        "height", "occlusion", "emission", "detail", "environment"
    };
    
    for (int i = 0; i < MATMAP_COUNT; i++) {
        // Try custom path first
        if (override->mapPaths[i][0]) {
            Com_sprintf(filename, sizeof(filename), "%s/%s", path, override->mapPaths[i]);
        } else {
            // Try standard naming
            Com_sprintf(filename, sizeof(filename), "%s/%s.tga", path, mapNames[i]);
        }
        
        override->maps[i] = R_FindImageFile(filename, IMGFLAG_NONE);
        if (override->maps[i]) {
            loaded++;
        }
    }
    
    return (loaded > 0) ? qtrue : qfalse;
}

// ============================================================================
// Override Creation and Export
// ============================================================================

/*
================
MatOver_Create

Create new material override
================
*/
materialOverride_t* MatOver_Create(material_t *material) {
    materialOverride_t *override;
    
    ri.Printf(PRINT_ALL, "^4[MATOVER] MatOver_Create called for: %s\n", material ? material->name : "NULL");
    
    if (!material) {
        ri.Printf(PRINT_ALL, "^1[MATOVER] MatOver_Create: NULL material\n");
        return NULL;
    }
    
    if (matOverrideSystem.numOverrides >= MAX_OVERRIDE_MATERIALS) {
        ri.Printf(PRINT_WARNING, "Material override limit reached\n");
        return NULL;
    }
    
    override = Z_Malloc(sizeof(materialOverride_t));
    Q_strncpyz(override->name, material->name, sizeof(override->name));
    
    // Set default PBR properties based on material type
    VectorSet(override->pbr.baseColor, 1.0f, 1.0f, 1.0f);
    override->pbr.roughness = 0.5f;
    override->pbr.metallic = 0.0f;
    override->pbr.specular = 0.5f;
    override->pbr.normalStrength = 1.0f;
    override->pbr.occlusionStrength = 1.0f;
    override->pbr.heightScale = 0.02f;
    
    // Detect material type from name/shader
    if (Q_stristr(material->name, "metal") || Q_stristr(material->name, "steel")) {
        override->pbr.metallic = 0.9f;
        override->pbr.roughness = 0.3f;
    } else if (Q_stristr(material->name, "stone") || Q_stristr(material->name, "rock")) {
        override->pbr.roughness = 0.8f;
        override->pbr.specular = 0.2f;
    } else if (Q_stristr(material->name, "wood")) {
        override->pbr.roughness = 0.6f;
        override->pbr.specular = 0.3f;
    } else if (Q_stristr(material->name, "water") || Q_stristr(material->name, "glass")) {
        override->pbr.roughness = 0.1f;
        override->pbr.specular = 0.9f;
    }
    
    override->baseMaterial = material;
    
    // Get base texture
    if (material->stages[0].colorMap) {
        override->baseTexture = material->stages[0].colorMap;
    }
    
    override->generateResolution = r_materialGenResolution->integer;
    override->generateQuality = r_materialGenQuality->integer;
    
    // Ensure we have a valid resolution
    if (override->generateResolution <= 0) {
        override->generateResolution = 512;  // Default resolution
    }
    
    ri.Printf(PRINT_ALL, "^3[MATOVER] Resolution set to: %d\n", override->generateResolution);
    
    // Add to system
    matOverrideSystem.overrides[matOverrideSystem.numOverrides++] = override;
    MatOver_AddToHash(override);
    
    override->flags = MATOVER_GENERATED;
    
    return override;
}

/*
================
MatOver_Export

Export material override to disk
================
*/
qboolean MatOver_Export(materialOverride_t *override) {
    char path[MAX_QPATH];
    
    ri.Printf(PRINT_ALL, "^5[MATOVER] Export called for: %s\n", override ? override->name : "NULL");
    
    if (!override) {
        ri.Printf(PRINT_ALL, "^1[MATOVER] Export failed: NULL override\n");
        return qfalse;
    }
    
    if (!matOverrideSystem.exportEnabled) {
        ri.Printf(PRINT_ALL, "^1[MATOVER] Export failed: exportEnabled=%d\n", matOverrideSystem.exportEnabled);
        return qfalse;
    }
    
    // First create the base material directory
    ri.Printf(PRINT_ALL, "^5[MATOVER] Creating directory: %s\n", MATERIAL_OVERRIDE_PATH);
    MatOver_CreateDirectory(MATERIAL_OVERRIDE_PATH);
    
    Com_sprintf(path, sizeof(path), "%s/%s",
                MATERIAL_OVERRIDE_PATH, override->name);
    
    // Create directory for this specific material
    ri.Printf(PRINT_ALL, "^5[MATOVER] Creating material directory: %s\n", path);
    MatOver_CreateDirectory(path);
    
    // Export properties
    if (!MatOver_ExportProperties(override, path)) {
        return qfalse;
    }
    
    // Export maps
    if (!MatOver_ExportMaps(override, path)) {
        return qfalse;
    }
    
    override->flags |= MATOVER_CUSTOM;
    
    if (r_materialDebug->integer) {
        ri.Printf(PRINT_ALL, "Exported material: %s\n", override->name);
    }
    
    return qtrue;
}

// ============================================================================
// PBR Map Generation - Core Algorithms
// ============================================================================

/*
================
MatGen_SobelEdgeDetection

Generate normal map using Sobel edge detection
Optimized for Quake's high-contrast textures
================
*/
void MatGen_SobelEdgeDetection(const byte *input, byte *output, int width, int height, const edgeDetector_t *params) {
    // Sobel kernels
    static const float sobelX[9] = {
        -1, 0, 1,
        -2, 0, 2,
        -1, 0, 1
    };
    
    static const float sobelY[9] = {
        -1, -2, -1,
         0,  0,  0,
         1,  2,  1
    };
    
    float strength = params ? params->strength : r_matgen_sobelStrength->value;
    float threshold = params ? params->threshold : r_matgen_sobelThreshold->value;
    
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            float gx = 0, gy = 0;
            float samples[9];
            int idx = 0;
            
            // Sample 3x3 neighborhood
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int px = x + dx;
                    int py = y + dy;
                    int offset = (py * width + px) * 4;
                    
                    // Convert to grayscale
                    float gray = input[offset] * 0.299f + 
                                input[offset + 1] * 0.587f + 
                                input[offset + 2] * 0.114f;
                    samples[idx++] = gray / 255.0f;
                }
            }
            
            // Apply Sobel operators
            for (int i = 0; i < 9; i++) {
                gx += samples[i] * sobelX[i];
                gy += samples[i] * sobelY[i];
            }
            
            // Apply threshold
            if (fabs(gx) < threshold) gx = 0;
            if (fabs(gy) < threshold) gy = 0;
            
            // Convert gradients to normal
            vec3_t normal;
            normal[0] = -gx * strength;
            normal[1] = -gy * strength;
            normal[2] = 1.0f;
            VectorNormalize(normal);
            
            // Encode to RGB
            int outIdx = (y * width + x) * 4;
            output[outIdx] = (byte)((normal[0] * 0.5f + 0.5f) * 255);
            output[outIdx + 1] = (byte)((normal[1] * 0.5f + 0.5f) * 255);
            output[outIdx + 2] = (byte)((normal[2] * 0.5f + 0.5f) * 255);
            output[outIdx + 3] = 255;
        }
    }
}

/*
================
MatGen_LuminanceAnalysis

Generate roughness map from luminance
Dark areas are rough, bright areas are smooth (typical for Quake textures)
================
*/
void MatGen_LuminanceAnalysis(const byte *input, byte *output, int width, int height, float weight, qboolean invert) {
    float gamma = matOverrideSystem.defaultParams.luminanceGamma;
    
    for (int i = 0; i < width * height; i++) {
        int idx = i * 4;
        
        // Calculate luminance
        float lum = input[idx] * 0.299f + 
                   input[idx + 1] * 0.587f + 
                   input[idx + 2] * 0.114f;
        lum /= 255.0f;
        
        // Apply gamma correction
        lum = pow(lum, gamma);
        
        // Weight and invert if needed
        float roughness = invert ? lum : (1.0f - lum);
        roughness = roughness * weight + (1.0f - weight) * 0.5f;
        
        // Enhance contrast for Quake's aesthetic
        roughness = pow(roughness, 1.5f);
        
        byte value = (byte)(CLAMP(roughness, 0, 1) * 255);
        output[idx] = value;
        output[idx + 1] = value;
        output[idx + 2] = value;
        output[idx + 3] = 255;
    }
}

/*
================
MatGen_FrequencyAnalysis

Analyze frequency content for roughness estimation
High frequency = rough surface
================
*/
void MatGen_FrequencyAnalysis(const byte *input, byte *output, int width, int height, const freqAnalyzer_t *params) {
    int blockSize = params ? params->blockSize : 8;
    float scale = r_matgen_frequencyScale->value;
    
    for (int by = 0; by < height; by += blockSize) {
        for (int bx = 0; bx < width; bx += blockSize) {
            float variance = 0;
            float mean = 0;
            int count = 0;
            
            // Calculate mean
            for (int y = by; y < by + blockSize && y < height; y++) {
                for (int x = bx; x < bx + blockSize && x < width; x++) {
                    int idx = (y * width + x) * 4;
                    float gray = (input[idx] + input[idx + 1] + input[idx + 2]) / 3.0f;
                    mean += gray;
                    count++;
                }
            }
            mean /= count;
            
            // Calculate variance (frequency indicator)
            for (int y = by; y < by + blockSize && y < height; y++) {
                for (int x = bx; x < bx + blockSize && x < width; x++) {
                    int idx = (y * width + x) * 4;
                    float gray = (input[idx] + input[idx + 1] + input[idx + 2]) / 3.0f;
                    float diff = gray - mean;
                    variance += diff * diff;
                }
            }
            variance = sqrt(variance / count) / 255.0f;
            
            // Scale and convert to roughness
            float roughness = CLAMP(variance * scale, 0, 1);
            byte value = (byte)(roughness * 255);
            
            // Fill block
            for (int y = by; y < by + blockSize && y < height; y++) {
                for (int x = bx; x < bx + blockSize && x < width; x++) {
                    int idx = (y * width + x) * 4;
                    output[idx] = value;
                    output[idx + 1] = value;
                    output[idx + 2] = value;
                    output[idx + 3] = 255;
                }
            }
        }
    }
}

/*
================
MatGen_ChromaticAnalysis

Detect metallic surfaces from color properties
Metals have low saturation and specific hue ranges
================
*/
void MatGen_ChromaticAnalysis(const byte *input, byte *output, int width, int height, float threshold, const vec3_t bias) {
    for (int i = 0; i < width * height; i++) {
        int idx = i * 4;
        
        float r = input[idx] / 255.0f;
        float g = input[idx + 1] / 255.0f;
        float b = input[idx + 2] / 255.0f;
        
        // Calculate saturation
        float maxVal = MAX(r, MAX(g, b));
        float minVal = MIN(r, MIN(g, b));
        float saturation = (maxVal > 0) ? ((maxVal - minVal) / maxVal) : 0;
        
        // Low saturation indicates potential metal
        float metalness = 0;
        if (saturation < threshold) {
            metalness = 1.0f - (saturation / threshold);
            
            // Apply color bias for typical Quake metals
            float colorMatch = 0;
            if (bias) {
                vec3_t color = { r, g, b };
                vec3_t diff;
                VectorSubtract(color, bias, diff);
                colorMatch = 1.0f - VectorLength(diff);
                metalness *= colorMatch;
            }
        }
        
        // Additional check for specific metal colors
        float grayness = 1.0f - fabs(r - g) - fabs(g - b) - fabs(b - r);
        metalness = MAX(metalness, grayness * 0.5f);
        
        byte value = (byte)(CLAMP(metalness, 0, 1) * 255);
        output[idx] = value;
        output[idx + 1] = value;
        output[idx + 2] = value;
        output[idx + 3] = 255;
    }
}

/*
================
MatGen_LocalContrastAnalysis

Analyze local contrast for specular highlights
High contrast areas are likely specular
================
*/
void MatGen_LocalContrastAnalysis(const byte *input, byte *output, int width, int height, float radius, float strength) {
    int iRadius = (int)radius;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float localMin = 255;
            float localMax = 0;
            int count = 0;
            
            // Sample neighborhood
            for (int dy = -iRadius; dy <= iRadius; dy++) {
                for (int dx = -iRadius; dx <= iRadius; dx++) {
                    int px = CLAMP(x + dx, 0, width - 1);
                    int py = CLAMP(y + dy, 0, height - 1);
                    int idx = (py * width + px) * 4;
                    
                    float gray = (input[idx] + input[idx + 1] + input[idx + 2]) / 3.0f;
                    localMin = MIN(localMin, gray);
                    localMax = MAX(localMax, gray);
                    count++;
                }
            }
            
            // Calculate local contrast
            float contrast = (localMax - localMin) / 255.0f;
            contrast = pow(contrast, 1.0f / strength);
            
            int outIdx = (y * width + x) * 4;
            byte value = (byte)(CLAMP(contrast, 0, 1) * 255);
            output[outIdx] = value;
            output[outIdx + 1] = value;
            output[outIdx + 2] = value;
            output[outIdx + 3] = 255;
        }
    }
}

/*
================
MatGen_FuseMaps

Fuse multiple generated maps using weighted combination
================
*/
void MatGen_FuseMaps(byte **inputs, byte *output, int width, int height, int numInputs, const float *weights) {
    float totalWeight = 0;
    for (int i = 0; i < numInputs; i++) {
        totalWeight += weights[i];
    }
    
    if (totalWeight <= 0) {
        totalWeight = 1.0f;
    }
    
    for (int pixel = 0; pixel < width * height; pixel++) {
        int idx = pixel * 4;
        vec3_t result = { 0, 0, 0 };
        
        for (int i = 0; i < numInputs; i++) {
            if (inputs[i]) {
                float w = weights[i] / totalWeight;
                result[0] += inputs[i][idx] * w;
                result[1] += inputs[i][idx + 1] * w;
                result[2] += inputs[i][idx + 2] * w;
            }
        }
        
        output[idx] = (byte)CLAMP(result[0], 0, 255);
        output[idx + 1] = (byte)CLAMP(result[1], 0, 255);
        output[idx + 2] = (byte)CLAMP(result[2], 0, 255);
        output[idx + 3] = 255;
    }
}

// ============================================================================
// Complete Map Generation
// ============================================================================

/*
================
MatOver_GenerateMaps

Generate all PBR maps for material
================
*/
void MatOver_GenerateMaps(materialOverride_t *override) {
    ri.Printf(PRINT_ALL, "^6[MATOVER] GenerateMaps called for: %s\n", override ? override->name : "NULL");
    
    if (!override || !override->baseTexture) {
        ri.Printf(PRINT_ALL, "^1[MATOVER] GenerateMaps failed: override=%p, baseTexture=%p\n", 
                  override, override ? override->baseTexture : NULL);
        return;
    }
    
    int startTime = ri.Milliseconds();
    
    // Generate each map type
    MatOver_GenerateNormalMap(override);
    MatOver_GenerateRoughnessMap(override);
    MatOver_GenerateMetallicMap(override);
    MatOver_GenerateSpecularMap(override);
    MatOver_GenerateOcclusionMap(override);
    MatOver_GenerateHeightMap(override);
    
    override->generateTime = (ri.Milliseconds() - startTime) / 1000.0f;
    override->flags |= MATOVER_GENERATED;
    
    matOverrideSystem.mapsGenerated++;
    matOverrideSystem.totalGenTime += override->generateTime;
    
    // Export if enabled
    ri.Printf(PRINT_ALL, "^6[MATOVER] Export check: exportEnabled=%d\n", matOverrideSystem.exportEnabled);
    if (matOverrideSystem.exportEnabled) {
        ri.Printf(PRINT_ALL, "^6[MATOVER] Calling MatOver_Export...\n");
        MatOver_Export(override);
    }
    
    if (r_materialDebug->integer) {
        ri.Printf(PRINT_ALL, "Generated PBR maps for %s in %.2fs\n", 
                  override->name, override->generateTime);
    }
}

/*
================
MatOver_GenerateNormalMap

Generate normal map using fused Sobel and curvature analysis
================
*/
void MatOver_GenerateNormalMap(materialOverride_t *override) {
    if (!override->baseTexture) {
        return;
    }
    
    int width = override->generateResolution;
    int height = override->generateResolution;
    int size = width * height * 4;
    
    // Allocate buffers
    byte *source = MatOver_AllocCache(size);
    byte *sobel = MatOver_AllocCache(size);
    byte *result = MatOver_AllocCache(size);
    
    if (!source || !sobel || !result) {
        return;
    }
    
    // Load texture data and resample to target resolution
    int srcWidth, srcHeight;
    byte *srcData = MatOver_LoadTextureData(override->baseTexture, &srcWidth, &srcHeight);
    if (srcData) {
        R_ResampleTexture(srcData, srcWidth, srcHeight, source, width, height);
        MatOver_FreeTextureData(srcData);
    } else {
        // Fill with default if loading fails
        Com_Memset(source, 128, width * height * 4);
    }
    
    // Apply Sobel edge detection
    edgeDetector_t edgeParams = {
        .strength = r_matgen_sobelStrength->value * override->pbr.normalStrength,
        .threshold = r_matgen_sobelThreshold->value,
        .passes = matOverrideSystem.defaultParams.iterations
    };
    
    MatGen_SobelEdgeDetection(source, sobel, width, height, &edgeParams);
    
    // For better results, run a second pass with different kernel
    if (r_materialGenQuality->integer >= 2) {
        byte *temp = MatOver_AllocCache(size);
        if (temp) {
            edgeParams.strength *= 0.5f;
            edgeParams.threshold *= 2.0f;
            MatGen_SobelEdgeDetection(source, temp, width, height, &edgeParams);
            
            // Combine both passes
            byte *inputs[2] = { sobel, temp };
            float weights[2] = { 0.7f, 0.3f };
            MatGen_FuseMaps(inputs, result, width, height, 2, weights);
            Com_Memcpy(sobel, result, size);
        }
    }
    
    // Store pixel data for export
    if (override->mapData[MATMAP_NORMAL]) {
        Z_Free(override->mapData[MATMAP_NORMAL]);
    }
    override->mapData[MATMAP_NORMAL] = Z_Malloc(size);
    Com_Memcpy(override->mapData[MATMAP_NORMAL], sobel, size);
    override->mapWidth[MATMAP_NORMAL] = width;
    override->mapHeight[MATMAP_NORMAL] = height;
    ri.Printf(PRINT_ALL, "^2[MATOVER] Stored normal map: %dx%d (%d bytes)\n", width, height, size);
    
    // R_CreateImage expects data allocated with Hunk_AllocateTempMemory
    // Copy our data to properly allocated memory
    byte *tempData = ri.Hunk_AllocateTempMemory(size);
    Com_Memcpy(tempData, sobel, size);
    
    // Create image (using 0 for flags as IMGFLAG constants not available)
    override->maps[MATMAP_NORMAL] = R_CreateImage(va("%s_normal", override->name),
                                                  NULL, tempData, width, height, 0);
}

/*
================
MatOver_GenerateRoughnessMap

Generate roughness using luminance and frequency analysis
================
*/
void MatOver_GenerateRoughnessMap(materialOverride_t *override) {
    if (!override->baseTexture) {
        return;
    }
    
    int width = override->generateResolution;
    int height = override->generateResolution;
    int size = width * height * 4;
    
    // Allocate buffers
    byte *source = MatOver_AllocCache(size);
    byte *luminance = MatOver_AllocCache(size);
    byte *frequency = MatOver_AllocCache(size);
    byte *result = MatOver_AllocCache(size);
    
    if (!source || !luminance || !frequency || !result) {
        return;
    }
    
    // Resample base texture
    int srcWidth, srcHeight;
    byte *srcData = MatOver_LoadTextureData(override->baseTexture, &srcWidth, &srcHeight);
    if (srcData) {
        R_ResampleTexture(srcData, srcWidth, srcHeight, source, width, height);
        MatOver_FreeTextureData(srcData);
    } else {
        Com_Memset(source, 128, width * height * 4);
    }
    
    // Luminance-based roughness
    MatGen_LuminanceAnalysis(source, luminance, width, height,
                            r_matgen_luminanceWeight->value, qfalse);
    
    // Frequency-based roughness
    freqAnalyzer_t freqParams = {
        .blockSize = 8,
        .spectrum = NULL,
        .spectrumSize = 0,
        .weights = NULL
    };
    MatGen_FrequencyAnalysis(source, frequency, width, height, &freqParams);
    
    // Fuse results
    byte *inputs[2] = { luminance, frequency };
    float weights[2] = { 0.6f, 0.4f };  // Favor luminance for Quake textures
    MatGen_FuseMaps(inputs, result, width, height, 2, weights);
    
    // Adjust based on material properties
    for (int i = 0; i < width * height; i++) {
        int idx = i * 4;
        float r = result[idx] / 255.0f;
        r = r * (1.0f - override->pbr.roughness) + override->pbr.roughness;
        result[idx] = result[idx + 1] = result[idx + 2] = (byte)(r * 255);
    }
    
    // Store pixel data for export
    if (override->mapData[MATMAP_ROUGHNESS]) {
        Z_Free(override->mapData[MATMAP_ROUGHNESS]);
    }
    override->mapData[MATMAP_ROUGHNESS] = Z_Malloc(size);
    Com_Memcpy(override->mapData[MATMAP_ROUGHNESS], result, size);
    override->mapWidth[MATMAP_ROUGHNESS] = width;
    override->mapHeight[MATMAP_ROUGHNESS] = height;
    
    // Create image with properly allocated memory
    byte *tempData = ri.Hunk_AllocateTempMemory(size);
    Com_Memcpy(tempData, result, size);
    override->maps[MATMAP_ROUGHNESS] = R_CreateImage(va("%s_roughness", override->name),
                                                     NULL, tempData, width, height,
                                                     IMGFLAG_NONE);
}

/*
================
MatOver_GenerateMetallicMap

Generate metallic map using chromatic and contrast analysis
================
*/
void MatOver_GenerateMetallicMap(materialOverride_t *override) {
    if (!override->baseTexture) {
        return;
    }
    
    int width = override->generateResolution;
    int height = override->generateResolution;
    int size = width * height * 4;
    
    // Allocate buffers
    byte *source = MatOver_AllocCache(size);
    byte *chromatic = MatOver_AllocCache(size);
    byte *contrast = MatOver_AllocCache(size);
    byte *result = MatOver_AllocCache(size);
    
    if (!source || !chromatic || !contrast || !result) {
        return;
    }
    
    // Resample base texture
    int srcWidth, srcHeight;
    byte *srcData = MatOver_LoadTextureData(override->baseTexture, &srcWidth, &srcHeight);
    if (srcData) {
        R_ResampleTexture(srcData, srcWidth, srcHeight, source, width, height);
        MatOver_FreeTextureData(srcData);
    } else {
        Com_Memset(source, 128, width * height * 4);
    }
    
    // Chromatic analysis for metal detection
    vec3_t metalBias = { 0.8f, 0.8f, 0.85f };  // Typical Quake metal color
    MatGen_ChromaticAnalysis(source, chromatic, width, height,
                            r_matgen_chromaticThreshold->value, metalBias);
    
    // Local contrast for specular areas
    MatGen_LocalContrastAnalysis(source, contrast, width, height,
                                r_matgen_contrastRadius->value, 1.5f);
    
    // Fuse results
    byte *inputs[2] = { chromatic, contrast };
    float weights[2] = { 0.7f, 0.3f };  // Favor chromatic for metal detection
    MatGen_FuseMaps(inputs, result, width, height, 2, weights);
    
    // Adjust based on material properties
    for (int i = 0; i < width * height; i++) {
        int idx = i * 4;
        float m = result[idx] / 255.0f;
        m = m * override->pbr.metallic;
        result[idx] = result[idx + 1] = result[idx + 2] = (byte)(m * 255);
    }
    
    // Store pixel data for export
    if (override->mapData[MATMAP_METALLIC]) {
        Z_Free(override->mapData[MATMAP_METALLIC]);
    }
    override->mapData[MATMAP_METALLIC] = Z_Malloc(size);
    Com_Memcpy(override->mapData[MATMAP_METALLIC], result, size);
    override->mapWidth[MATMAP_METALLIC] = width;
    override->mapHeight[MATMAP_METALLIC] = height;
    
    // Create image with properly allocated memory
    byte *tempData = ri.Hunk_AllocateTempMemory(size);
    Com_Memcpy(tempData, result, size);
    override->maps[MATMAP_METALLIC] = R_CreateImage(va("%s_metallic", override->name),
                                                    NULL, tempData, width, height,
                                                    IMGFLAG_NONE);
}

/*
================
MatOver_GenerateSpecularMap

Generate specular map from contrast and roughness inverse
================
*/
void MatOver_GenerateSpecularMap(materialOverride_t *override) {
    if (!override->baseTexture || !override->maps[MATMAP_ROUGHNESS]) {
        return;
    }
    
    int width = override->generateResolution;
    int height = override->generateResolution;
    int size = width * height * 4;
    
    byte *source = MatOver_AllocCache(size);
    byte *result = MatOver_AllocCache(size);
    
    if (!source || !result) {
        return;
    }
    
    // Load roughness data as base for specular
    int rWidth, rHeight;
    byte *roughnessData = MatOver_LoadTextureData(override->maps[MATMAP_ROUGHNESS], &rWidth, &rHeight);
    if (!roughnessData) {
        // Cache-allocated memory is managed by the cache system, don't free it
        return;
    }
    
    // Resample if needed
    if (rWidth != width || rHeight != height) {
        byte *resampled = Z_Malloc(size);
        R_ResampleTexture(roughnessData, rWidth, rHeight, resampled, width, height);
        Com_Memcpy(source, resampled, size);
        Z_Free(resampled);
    } else {
        Com_Memcpy(source, roughnessData, size);
    }
    MatOver_FreeTextureData(roughnessData);
    
    // Invert roughness to get specular
    for (int i = 0; i < width * height; i++) {
        int idx = i * 4;
        float rough = source[idx] / 255.0f;
        float spec = (1.0f - rough) * override->pbr.specular;
        
        // Enhance for metals  
        // Note: We'd need to load metallic map data here if needed
        // For now, skip metallic enhancement
        
        result[idx] = result[idx + 1] = result[idx + 2] = (byte)(spec * 255);
        result[idx + 3] = 255;
    }
    
    // Store pixel data for export
    if (override->mapData[MATMAP_SPECULAR]) {
        Z_Free(override->mapData[MATMAP_SPECULAR]);
    }
    override->mapData[MATMAP_SPECULAR] = Z_Malloc(size);
    Com_Memcpy(override->mapData[MATMAP_SPECULAR], result, size);
    override->mapWidth[MATMAP_SPECULAR] = width;
    override->mapHeight[MATMAP_SPECULAR] = height;
    
    // Create image with properly allocated memory
    byte *tempData = ri.Hunk_AllocateTempMemory(size);
    Com_Memcpy(tempData, result, size);
    override->maps[MATMAP_SPECULAR] = R_CreateImage(va("%s_specular", override->name),
                                                    NULL, tempData, width, height,
                                                    IMGFLAG_NONE);
}

/*
================
MatOver_GenerateOcclusionMap

Generate ambient occlusion from height and normal maps
================
*/
void MatOver_GenerateOcclusionMap(materialOverride_t *override) {
    if (!override->maps[MATMAP_HEIGHT]) {
        // Simple AO from luminance if no height map
        if (!override->baseTexture) {
            return;
        }
        
        int width = override->generateResolution;
        int height = override->generateResolution;
        int size = width * height * 4;
        
        byte *source = MatOver_AllocCache(size);
        byte *result = MatOver_AllocCache(size);
        
        if (!source || !result) {
            return;
        }
        
        int srcWidth2, srcHeight2;
        byte *srcData2 = MatOver_LoadTextureData(override->baseTexture, &srcWidth2, &srcHeight2);
        if (srcData2) {
            R_ResampleTexture(srcData2, srcWidth2, srcHeight2, source, width, height);
            MatOver_FreeTextureData(srcData2);
        } else {
            Com_Memset(source, 128, width * height * 4);
        }
        
        // Use inverted luminance as simple AO
        MatGen_LuminanceAnalysis(source, result, width, height, 0.5f, qtrue);
        
        // Soften the AO
        for (int i = 0; i < width * height; i++) {
            int idx = i * 4;
            float ao = result[idx] / 255.0f;
            ao = 0.5f + ao * 0.5f * override->pbr.occlusionStrength;
            result[idx] = result[idx + 1] = result[idx + 2] = (byte)(ao * 255);
        }
        
        // Store pixel data for export
        if (override->mapData[MATMAP_OCCLUSION]) {
            Z_Free(override->mapData[MATMAP_OCCLUSION]);
        }
        override->mapData[MATMAP_OCCLUSION] = Z_Malloc(size);
        Com_Memcpy(override->mapData[MATMAP_OCCLUSION], result, size);
        override->mapWidth[MATMAP_OCCLUSION] = width;
        override->mapHeight[MATMAP_OCCLUSION] = height;
        
        // Create image with properly allocated memory
        byte *tempData = ri.Hunk_AllocateTempMemory(size);
        Com_Memcpy(tempData, result, size);
        override->maps[MATMAP_OCCLUSION] = R_CreateImage(va("%s_occlusion", override->name),
                                                         NULL, tempData, width, height,
                                                         IMGFLAG_NONE);
    }
}

/*
================
MatOver_GenerateHeightMap

Generate height map from luminance and edge detection
================
*/
void MatOver_GenerateHeightMap(materialOverride_t *override) {
    if (!override->baseTexture) {
        return;
    }
    
    int width = override->generateResolution;
    int height = override->generateResolution;
    int size = width * height * 4;
    
    byte *source = MatOver_AllocCache(size);
    byte *result = MatOver_AllocCache(size);
    
    if (!source || !result) {
        return;
    }
    
    int srcWidth, srcHeight;
    byte *srcData = MatOver_LoadTextureData(override->baseTexture, &srcWidth, &srcHeight);
    if (srcData) {
        R_ResampleTexture(srcData, srcWidth, srcHeight, source, width, height);
        MatOver_FreeTextureData(srcData);
    } else {
        Com_Memset(source, 128, width * height * 4);
    }
    
    // Use luminance as height base
    MatGen_LuminanceAnalysis(source, result, width, height, 1.0f, qfalse);
    
    // Blur for smoother height transitions
    if (r_materialGenQuality->integer >= 2) {
        // Simple box blur
        byte *temp = MatOver_AllocCache(size);
        if (temp) {
            for (int y = 1; y < height - 1; y++) {
                for (int x = 1; x < width - 1; x++) {
                    float sum = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int idx = ((y + dy) * width + (x + dx)) * 4;
                            sum += result[idx];
                        }
                    }
                    int idx = (y * width + x) * 4;
                    byte value = (byte)(sum / 9.0f);
                    temp[idx] = temp[idx + 1] = temp[idx + 2] = value;
                    temp[idx + 3] = 255;
                }
            }
            Com_Memcpy(result, temp, size);
        }
    }
    
    // Store pixel data for export
    if (override->mapData[MATMAP_HEIGHT]) {
        Z_Free(override->mapData[MATMAP_HEIGHT]);
    }
    override->mapData[MATMAP_HEIGHT] = Z_Malloc(size);
    Com_Memcpy(override->mapData[MATMAP_HEIGHT], result, size);
    override->mapWidth[MATMAP_HEIGHT] = width;
    override->mapHeight[MATMAP_HEIGHT] = height;
    
    // Create image with properly allocated memory  
    byte *tempData = ri.Hunk_AllocateTempMemory(size);
    Com_Memcpy(tempData, result, size);
    override->maps[MATMAP_HEIGHT] = R_CreateImage(va("%s_height", override->name),
                                                  NULL, tempData, width, height,
                                                  IMGFLAG_NONE);
}

// ============================================================================
// Utility Functions
// ============================================================================

/*
================
MatOver_CreateDirectory

Create directory for material export
================
*/
void MatOver_CreateDirectory(const char *path) {
    char temp[MAX_QPATH];
    char *p = NULL;
    size_t len;
    
    ri.Printf(PRINT_ALL, "^4[MATOVER] Creating directory: %s\n", path);
    
    // Copy path to temp buffer
    Q_strncpyz(temp, path, sizeof(temp));
    len = strlen(temp);
    
    // Remove trailing slash if present
    if (temp[len - 1] == '/' || temp[len - 1] == '\\') {
        temp[len - 1] = 0;
    }
    
    // Create directories recursively
    for (p = temp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
#ifdef _WIN32
            _mkdir(temp);
#else
            mkdir(temp, 0755);
#endif
            *p = '/';
        }
    }
    
    // Create the final directory
#ifdef _WIN32
    _mkdir(temp);
#else
    mkdir(temp, 0755);
#endif
}

/*
================
MatOver_FileExists

Check if file exists
================
*/
qboolean MatOver_FileExists(const char *path) {
    return ri.FS_FileExists(path);
}

/*
================
MatOver_Hash

Generate hash for material name
================
*/

// Helper function to get stored map data
static byte *MatOver_GetMapData(materialOverride_t *override, int mapType, int *width, int *height) {
    if (!override || mapType < 0 || mapType >= MATMAP_COUNT) {
        return NULL;
    }
    
    if (override->mapData[mapType]) {
        int w = override->mapWidth[mapType];
        int h = override->mapHeight[mapType];
        
        ri.Printf(PRINT_ALL, "^4[MATOVER] GetMapData type %d: %dx%d\n", mapType, w, h);
        
        if (width) *width = w;
        if (height) *height = h;
        
        // Return a copy of the data
        int size = w * h * 4;
        if (size <= 0) {
            ri.Printf(PRINT_WARNING, "^1[MATOVER] Invalid size for map %d: %d\n", mapType, size);
            return NULL;
        }
        
        byte *data = Z_Malloc(size);
        Com_Memcpy(data, override->mapData[mapType], size);
        return data;
    }
    
    ri.Printf(PRINT_ALL, "^1[MATOVER] No data for map type %d\n", mapType);
    return NULL;
}

// Helper functions for texture data access
static byte *MatOver_LoadTextureData(image_t *image, int *width, int *height) {
    byte *pic = NULL;
    int w = 0, h = 0;
    
    if (!image) {
        return NULL;
    }
    
    // Try to load from file if it has a name
    if (!image->imgName) {
        return NULL;
    }
    
    // Try to load the texture file
    // Check extension and load appropriately
    const char *ext = strrchr(image->imgName, '.');
    if (ext) {
        if (!Q_stricmp(ext, ".tga")) {
            R_LoadTGA(image->imgName, &pic, &w, &h);
        } else if (!Q_stricmp(ext, ".jpg") || !Q_stricmp(ext, ".jpeg")) {
            R_LoadJPG(image->imgName, (unsigned char **)&pic, &w, &h);
        }
    }
    
    // Try alternative extension if we have imgName2
    if (!pic && image->imgName2) {
        ext = strrchr(image->imgName2, '.');
        if (ext) {
            if (!Q_stricmp(ext, ".tga")) {
                R_LoadTGA(image->imgName2, &pic, &w, &h);
            } else if (!Q_stricmp(ext, ".jpg") || !Q_stricmp(ext, ".jpeg")) {
                R_LoadJPG(image->imgName2, (unsigned char **)&pic, &w, &h);
            }
        }
    }
    
    if (pic) {
        if (width) *width = w;
        if (height) *height = h;
        return pic;
    }
    
    // Fallback to image dimensions without data
    if (width) *width = image->width;
    if (height) *height = image->height;
    return NULL;
}

static void MatOver_FreeTextureData(byte *data) {
    if (data) {
        ri.Free(data);
    }
}

unsigned int MatOver_Hash(const char *name) {
    unsigned int hash = 0;
    while (*name) {
        hash = hash * 31 + tolower(*name);
        name++;
    }
    return hash & 1023;
}

/*
================
MatOver_AddToHash

Add override to hash table
================
*/
void MatOver_AddToHash(materialOverride_t *override) {
    unsigned int hash = MatOver_Hash(override->name);
    override->hashNext = matOverrideSystem.hashTable[hash];
    matOverrideSystem.hashTable[hash] = override;
}

/*
================
MatOver_FindInHash

Find override in hash table
================
*/
materialOverride_t* MatOver_FindInHash(const char *name) {
    unsigned int hash = MatOver_Hash(name);
    materialOverride_t *override = matOverrideSystem.hashTable[hash];
    
    while (override) {
        if (!Q_stricmp(override->name, name)) {
            return override;
        }
        override = override->hashNext;
    }
    
    return NULL;
}

// ============================================================================
// Cache Management
// ============================================================================

/*
================
MatOver_InitCache

Initialize cache memory
================
*/
void MatOver_InitCache(int sizeInMB) {
    // Limit cache size to something reasonable
    if (sizeInMB <= 0) {
        sizeInMB = 16; // Default to 16MB
    } else if (sizeInMB > 64) {
        sizeInMB = 64; // Cap at 64MB to avoid exhausting hunk memory
        ri.Printf(PRINT_WARNING, "Material cache size capped at 64MB\n");
    }
    
    matOverrideSystem.cacheSize = sizeInMB * 1024 * 1024;
    matOverrideSystem.cacheMemory = ri.Hunk_Alloc(matOverrideSystem.cacheSize, h_low);
    matOverrideSystem.cacheUsed = 0;
}

/*
================
MatOver_FreeCache

Free cache memory
================
*/
void MatOver_FreeCache(void) {
    matOverrideSystem.cacheMemory = NULL;
    matOverrideSystem.cacheSize = 0;
    matOverrideSystem.cacheUsed = 0;
}

/*
================
MatOver_AllocCache

Allocate from cache
================
*/
byte* MatOver_AllocCache(int size) {
    if (matOverrideSystem.cacheUsed + size > matOverrideSystem.cacheSize) {
        MatOver_CompactCache();
        if (matOverrideSystem.cacheUsed + size > matOverrideSystem.cacheSize) {
            return NULL;
        }
    }
    
    byte *ptr = matOverrideSystem.cacheMemory + matOverrideSystem.cacheUsed;
    matOverrideSystem.cacheUsed += size;
    return ptr;
}

/*
================
MatOver_CompactCache

Compact cache memory
================
*/
void MatOver_CompactCache(void) {
    // Simple reset for now
    matOverrideSystem.cacheUsed = 0;
}

// ============================================================================
// Helper Functions
// ============================================================================

/*
================
R_ResampleTexture

Resample texture to new resolution
================
*/
static void R_ResampleTexture(const byte *in, int inwidth, int inheight,
                              byte *out, int outwidth, int outheight) {
    int i, j;
    const byte *inrow;
    float xscale = (float)inwidth / outwidth;
    float yscale = (float)inheight / outheight;
    
    for (i = 0; i < outheight; i++) {
        inrow = in + ((int)(i * yscale) * inwidth) * 4;
        for (j = 0; j < outwidth; j++) {
            int idx = ((int)(j * xscale)) * 4;
            out[0] = inrow[idx];
            out[1] = inrow[idx + 1];
            out[2] = inrow[idx + 2];
            out[3] = inrow[idx + 3];
            out += 4;
        }
    }
}

/*
================
R_SaveImageToFile

Save image data to TGA file
================
*/
qboolean R_SaveImageToFile(const char *filename, const byte *data, 
                           int width, int height, int components) {
    byte *fileData;
    byte *buffer;
    int size, totalSize;
    
    // TGA header
    byte header[18];
    Com_Memset(header, 0, 18);
    header[2] = 2;  // Uncompressed RGB
    header[12] = width & 255;
    header[13] = (width >> 8) & 255;
    header[14] = height & 255;
    header[15] = (height >> 8) & 255;
    header[16] = components * 8;  // Bits per pixel
    header[17] = 0x20;  // Top-left origin
    
    // Calculate total file size
    size = width * height * components;
    totalSize = 18 + size;
    
    // Allocate buffer for entire file
    fileData = Z_Malloc(totalSize);
    
    // Copy header
    Com_Memcpy(fileData, header, 18);
    
    // Convert RGBA to BGRA for TGA and copy to file buffer
    buffer = fileData + 18;
    
    for (int i = 0; i < width * height; i++) {
        int idx = i * components;
        if (components >= 3) {
            buffer[idx] = data[idx + 2];      // B
            buffer[idx + 1] = data[idx + 1];  // G
            buffer[idx + 2] = data[idx];      // R
            if (components == 4) {
                buffer[idx + 3] = data[idx + 3];  // A
            }
        } else {
            Com_Memcpy(&buffer[idx], &data[idx], components);
        }
    }
    
    // Write entire file at once using FS_WriteFile
    ri.FS_WriteFile(filename, fileData, totalSize);
    
    Z_Free(fileData);
    
    return qtrue;
}

/*
================
CLAMP

Clamp value between min and max
================
*/
#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : (x) > (max) ? (max) : (x))
#endif

/*
================
MIN/MAX

Min/max macros
================
*/
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// ============================================================================
// Shader Integration
// ============================================================================

/*
================
MatOver_ProcessShader

Process a newly registered shader for material export
================
*/
void MatOver_ProcessShader(shader_t *shader) {
    materialOverride_t *override;
    image_t *baseImage = NULL;
    
    // Only process if material export is enabled
    if (!r_materialExport || !r_materialExport->integer) {
        return;
    }
    
    // Skip special shaders
    if (!shader || shader->defaultShader || shader->isSky) {
        return;
    }
    
    // Skip if shader name starts with certain prefixes
    if (Q_strncmp(shader->name, "gfx/", 4) == 0 ||
        Q_strncmp(shader->name, "menu/", 5) == 0 ||
        Q_strncmp(shader->name, "ui/", 3) == 0 ||
        Q_strncmp(shader->name, "console/", 8) == 0 ||
        Q_strncmp(shader->name, "sprites/", 8) == 0 ||
        Q_strncmp(shader->name, "fonts/", 6) == 0) {
        return;
    }
    
    ri.Printf(PRINT_ALL, "^2[MATOVER] Processing shader: %s (stages=%d)\n", 
              shader->name, shader->numUnfoggedPasses);
    
    // Find the base diffuse texture from the shader stages
    for (int i = 0; i < shader->numUnfoggedPasses && i < MAX_SHADER_STAGES; i++) {
        if (shader->stages[i] && shader->stages[i]->bundle[0].image[0]) {
            // Skip lightmap stages
            if (shader->stages[i]->bundle[0].lightmap == LIGHTMAP_INDEX_NONE) {
                baseImage = shader->stages[i]->bundle[0].image[0];
                ri.Printf(PRINT_ALL, "^2[MATOVER] Found base texture in stage %d: %s\n", 
                          i, baseImage->imgName);
                break;
            }
        }
    }
    
    // If no base image found, can't generate PBR maps
    if (!baseImage) {
        ri.Printf(PRINT_ALL, "^3[MATOVER] No base texture found for shader: %s\n", shader->name);
        return;
    }
    
    // Check if override already exists
    override = MatOver_FindInHash(shader->name);
    if (override && (override->flags & MATOVER_GENERATED)) {
        ri.Printf(PRINT_ALL, "^3[MATOVER] Override already exists for: %s\n", shader->name);
        return;
    }
    
    // Create new override
    if (!override) {
        override = Z_Malloc(sizeof(materialOverride_t));
        Q_strncpyz(override->name, shader->name, sizeof(override->name));
        MatOver_AddToHash(override);
    }
    
    // Set up the override with shader data
    override->baseTexture = baseImage;
    
    // Create a dummy material structure for compatibility
    if (!override->baseMaterial) {
        override->baseMaterial = Z_Malloc(sizeof(material_t));
        Q_strncpyz(override->baseMaterial->name, shader->name, sizeof(override->baseMaterial->name));
        override->baseMaterial->stages[0].colorMap = baseImage;
    }
    
    // Set default PBR properties based on shader name hints
    VectorSet(override->pbr.baseColor, 1.0f, 1.0f, 1.0f);
    override->pbr.roughness = 0.5f;
    override->pbr.metallic = 0.0f;
    override->pbr.specular = 0.5f;
    override->pbr.normalStrength = 1.0f;
    override->pbr.occlusionStrength = 1.0f;
    override->pbr.heightScale = 0.02f;
    
    // Detect material type from shader name
    if (Q_stristr(shader->name, "metal") || Q_stristr(shader->name, "steel") || 
        Q_stristr(shader->name, "chrome") || Q_stristr(shader->name, "iron")) {
        override->pbr.metallic = 0.9f;
        override->pbr.roughness = 0.3f;
    } else if (Q_stristr(shader->name, "stone") || Q_stristr(shader->name, "rock") ||
               Q_stristr(shader->name, "brick") || Q_stristr(shader->name, "concrete")) {
        override->pbr.roughness = 0.8f;
        override->pbr.specular = 0.2f;
    } else if (Q_stristr(shader->name, "wood") || Q_stristr(shader->name, "plank")) {
        override->pbr.roughness = 0.6f;
        override->pbr.specular = 0.3f;
    } else if (Q_stristr(shader->name, "water") || Q_stristr(shader->name, "glass") ||
               Q_stristr(shader->name, "mirror")) {
        override->pbr.roughness = 0.1f;
        override->pbr.specular = 0.9f;
    } else if (Q_stristr(shader->name, "floor") || Q_stristr(shader->name, "ground")) {
        override->pbr.roughness = 0.7f;
        override->pbr.specular = 0.3f;
    }
    
    ri.Printf(PRINT_ALL, "^2[MATOVER] Queueing shader for PBR generation: %s\n", shader->name);
    
    // Queue for generation
    MatOver_QueueGeneration(override);
}

// ============================================================================
// Processing Queue
// ============================================================================

/*
================
MatOver_QueueGeneration

Queue material for generation
================
*/
void MatOver_QueueGeneration(materialOverride_t *override) {
    ri.Printf(PRINT_ALL, "^5[MATOVER] QueueGeneration called: override=%p, queueCount=%d\n",
              override, matOverrideSystem.queueCount);
    
    if (!override || matOverrideSystem.queueCount >= MAX_OVERRIDE_MATERIALS) {
        ri.Printf(PRINT_ALL, "^1[MATOVER] QueueGeneration failed: override=%p, queue full=%d\n",
                  override, matOverrideSystem.queueCount >= MAX_OVERRIDE_MATERIALS);
        return;
    }
    
    // Store the override directly in the list
    if (matOverrideSystem.numOverrides >= MAX_OVERRIDE_MATERIALS) {
        ri.Printf(PRINT_ALL, "^1[MATOVER] Override list full\n");
        return;
    }
    
    // Add to override list and queue
    matOverrideSystem.overrides[matOverrideSystem.numOverrides] = override;
    matOverrideSystem.generationQueue[matOverrideSystem.queueCount++] = matOverrideSystem.numOverrides;
    matOverrideSystem.numOverrides++;
    
    override->flags |= MATOVER_EXPORTING;
}

/*
================
MatOver_ProcessQueue

Process generation queue
================
*/
void MatOver_ProcessQueue(void) {
    if (matOverrideSystem.queueCount == 0) {
        return;
    }
    
    ri.Printf(PRINT_ALL, "^6[MATOVER] ProcessQueue: processing %d items\n", matOverrideSystem.queueCount);
    
    // Process one item per frame to avoid hitches
    int idx = matOverrideSystem.generationQueue[0];
    materialOverride_t *override = NULL;
    
    if (idx >= 0 && idx < matOverrideSystem.numOverrides) {
        override = matOverrideSystem.overrides[idx];
    }
    
    if (override) {
        MatOver_GenerateMaps(override);
        override->flags &= ~MATOVER_EXPORTING;
    }
    
    // Shift queue
    for (int i = 1; i < matOverrideSystem.queueCount; i++) {
        matOverrideSystem.generationQueue[i - 1] = matOverrideSystem.generationQueue[i];
    }
    matOverrideSystem.queueCount--;
}

/*
================
MatOver_Apply

Apply override to material
================
*/
void MatOver_Apply(material_t *material, materialOverride_t *override) {
    if (!material || !override) {
        return;
    }
    
    // Apply textures
    if (override->maps[MATMAP_DIFFUSE]) {
        material->stages[0].colorMap = override->maps[MATMAP_DIFFUSE];
    }
    if (override->maps[MATMAP_NORMAL]) {
        material->normalMap = override->maps[MATMAP_NORMAL];
        material->hasNormalMap = qtrue;
    }
    if (override->maps[MATMAP_SPECULAR]) {
        material->specularMap = override->maps[MATMAP_SPECULAR];
        material->hasSpecularMap = qtrue;
    }
    if (override->maps[MATMAP_ROUGHNESS]) {
        material->roughnessMap = override->maps[MATMAP_ROUGHNESS];
    }
    if (override->maps[MATMAP_METALLIC]) {
        material->metallicMap = override->maps[MATMAP_METALLIC];
    }
    if (override->maps[MATMAP_HEIGHT]) {
        material->heightMap = override->maps[MATMAP_HEIGHT];
        material->hasHeightMap = qtrue;
    }
    
    // Apply PBR properties
    VectorCopy(override->pbr.baseColor, material->baseColor);
    material->metallic = override->pbr.metallic;
    material->roughness = override->pbr.roughness;
    material->specular = override->pbr.specular;
    material->normalStrength = override->pbr.normalStrength;
    material->heightScale = override->pbr.heightScale;
    material->isPBR = qtrue;
    
    // Update flags
    material->materialFlags |= MATERIAL_PBR;
    if (override->maps[MATMAP_NORMAL]) {
        material->materialFlags |= MATERIAL_NEEDSTANGENT;
    }
}

/*
================
MatOver_WriteTGA

Write TGA file to physical filesystem
================
*/
static void MatOver_WriteTGA(const char *filename, byte *data, int width, int height) {
    FILE *f;
    byte *buffer;
    int i, c;
    int row;
    unsigned char *flip;
    unsigned char *src, *dst;
    
    buffer = ri.Malloc(width * height * 4 + 18);
    Com_Memset(buffer, 0, 18);
    buffer[2] = 2;        // uncompressed type
    buffer[12] = width & 255;
    buffer[13] = width >> 8;
    buffer[14] = height & 255;
    buffer[15] = height >> 8;
    buffer[16] = 32;      // pixel size
    
    // swap rgb to bgr
    c = 18 + width * height * 4;
    for (i = 18; i < c; i += 4) {
        buffer[i] = data[i - 18 + 2];      // blue
        buffer[i + 1] = data[i - 18 + 1];  // green
        buffer[i + 2] = data[i - 18 + 0];  // red
        buffer[i + 3] = data[i - 18 + 3];  // alpha
    }
    
    // flip upside down
    flip = (unsigned char *)ri.Malloc(width * 4);
    for (row = 0; row < height / 2; row++) {
        src = buffer + 18 + row * 4 * width;
        dst = buffer + 18 + (height - row - 1) * 4 * width;
        
        Com_Memcpy(flip, src, width * 4);
        Com_Memcpy(src, dst, width * 4);
        Com_Memcpy(dst, flip, width * 4);
    }
    ri.Free(flip);
    
    // Write to physical file
    f = fopen(filename, "wb");
    if (f) {
        fwrite(buffer, 1, c, f);
        fclose(f);
        if (r_materialDebug->integer) {
            ri.Printf(PRINT_ALL, "Wrote TGA: %s\n", filename);
        }
    } else {
        ri.Printf(PRINT_WARNING, "Failed to write TGA: %s\n", filename);
    }
    
    ri.Free(buffer);
}

/*
================
MatOver_ExportProperties

Export material properties to file
================
*/
qboolean MatOver_ExportProperties(materialOverride_t *override, const char *path) {
    char filename[MAX_QPATH];
    char buffer[8192];
    char temp[1024];
    FILE *f;
    
    Com_sprintf(filename, sizeof(filename), "%s/material.txt", path);
    
    // Write properties - build the buffer piece by piece to avoid overflow
    buffer[0] = '\0';
    Q_strcat(buffer, sizeof(buffer), "// Auto-generated PBR material\n");
    Com_sprintf(temp, sizeof(temp), "// %s\n\n", override->name);
    Q_strcat(buffer, sizeof(buffer), temp);
    
    Com_sprintf(temp, sizeof(temp), "baseColor %.3f %.3f %.3f\n",
                override->pbr.baseColor[0], override->pbr.baseColor[1], override->pbr.baseColor[2]);
    Q_strcat(buffer, sizeof(buffer), temp);
    
    Com_sprintf(temp, sizeof(temp), "metallic %.3f\n", override->pbr.metallic);
    Q_strcat(buffer, sizeof(buffer), temp);
    
    Com_sprintf(temp, sizeof(temp), "roughness %.3f\n", override->pbr.roughness);
    Q_strcat(buffer, sizeof(buffer), temp);
    
    Com_sprintf(temp, sizeof(temp), "specular %.3f\n", override->pbr.specular);
    Q_strcat(buffer, sizeof(buffer), temp);
    
    Com_sprintf(temp, sizeof(temp), "normalStrength %.3f\n", override->pbr.normalStrength);
    Q_strcat(buffer, sizeof(buffer), temp);
    
    Com_sprintf(temp, sizeof(temp), "heightScale %.3f\n", override->pbr.heightScale);
    Q_strcat(buffer, sizeof(buffer), temp);
    
    Com_sprintf(temp, sizeof(temp), "occlusionStrength %.3f\n", override->pbr.occlusionStrength);
    Q_strcat(buffer, sizeof(buffer), temp);
    
    // Write map references
    static const char *mapNames[MATMAP_COUNT] = {
        "diffuseMap", "normalMap", "specularMap", "roughnessMap", "metallicMap",
        "heightMap", "occlusionMap", "emissionMap", "detailMap", "environmentMap"
    };
    
    for (int i = 0; i < MATMAP_COUNT; i++) {
        if (override->maps[i]) {
            // Ensure the string won't overflow
            if (strlen(mapNames[i]) * 2 + 10 < sizeof(temp)) {
                Com_sprintf(temp, sizeof(temp), "%s %s.tga\n",
                           mapNames[i], mapNames[i]);
                Q_strcat(buffer, sizeof(buffer), temp);
            }
        }
    }
    
    // Write to physical file
    f = fopen(filename, "w");
    if (f) {
        fwrite(buffer, 1, strlen(buffer), f);
        fclose(f);
        if (r_materialDebug->integer) {
            ri.Printf(PRINT_ALL, "Wrote material properties: %s\n", filename);
        }
    } else {
        ri.Printf(PRINT_WARNING, "Failed to write material properties: %s\n", filename);
        return qfalse;
    }
    
    return qtrue;
}

/*
================
MatOver_ExportMaps

Export texture maps to disk
================
*/
qboolean MatOver_ExportMaps(materialOverride_t *override, const char *path) {
    static const char *mapNames[MATMAP_COUNT] = {
        "diffuse", "normal", "specular", "roughness", "metallic",
        "height", "occlusion", "emission", "detail", "environment"
    };
    
    for (int i = 0; i < MATMAP_COUNT; i++) {
        if (override->maps[i] && override->mapData[i]) {
            char filename[MAX_QPATH];
            Com_sprintf(filename, sizeof(filename), "%s/%s.tga", path, mapNames[i]);
            
            // Use stored pixel data for export
            int width, height;
            byte *data = MatOver_GetMapData(override, i, &width, &height);
            if (data) {
                ri.Printf(PRINT_ALL, "^6[MATOVER] Exporting %s: %dx%d\n", filename, width, height);
                MatOver_WriteTGA(filename, data, width, height);
                Z_Free(data);  // Free the copy returned by MatOver_GetMapData
                if (r_materialDebug->integer) {
                    ri.Printf(PRINT_ALL, "Exported map: %s\n", filename);
                }
            }
        }
    }
    
    return qtrue;
}