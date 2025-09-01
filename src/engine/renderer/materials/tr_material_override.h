/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Material Override System with Automatic PBR Generation
Allows custom materials and automatic generation of PBR maps
===========================================================================
*/

#ifndef TR_MATERIAL_OVERRIDE_H
#define TR_MATERIAL_OVERRIDE_H

#include "../tr_local.h"
#include "tr_material.h"

// ============================================================================
// Material Override Configuration
// ============================================================================

#define MATERIAL_OVERRIDE_PATH "baseq3/materials"
#define MAX_OVERRIDE_MATERIALS 4096
#define MATERIAL_MAP_SIZE 512           // Default generated map resolution
#define MATERIAL_CACHE_SIZE (128 * 1024 * 1024) // 128MB cache for generated maps

// Material map types
typedef enum {
    MATMAP_DIFFUSE,
    MATMAP_NORMAL,
    MATMAP_SPECULAR,
    MATMAP_ROUGHNESS,
    MATMAP_METALLIC,
    MATMAP_HEIGHT,
    MATMAP_OCCLUSION,
    MATMAP_EMISSION,
    MATMAP_DETAIL,
    MATMAP_ENVIRONMENT,
    MATMAP_COUNT
} materialMapType_t;

// Material override flags
typedef enum {
    MATOVER_LOADED      = (1 << 0),
    MATOVER_GENERATED   = (1 << 1),
    MATOVER_CUSTOM      = (1 << 2),
    MATOVER_EXPORTING   = (1 << 3),
    MATOVER_CACHED      = (1 << 4),
    MATOVER_DIRTY       = (1 << 5)
} materialOverrideFlags_t;

// ============================================================================
// PBR Material Properties
// ============================================================================

typedef struct pbrProperties_s {
    // Base properties
    vec3_t      baseColor;              // Albedo color multiplier
    float       metallic;               // Metallic value (0-1)
    float       roughness;              // Roughness value (0-1)
    float       specular;               // Specular intensity (0-1)
    float       normalStrength;         // Normal map strength
    
    // Advanced properties
    float       heightScale;            // Parallax height scale
    float       occlusionStrength;      // AO strength
    vec3_t      emissiveColor;          // Emissive color
    float       emissiveIntensity;      // Emissive brightness
    float       detailBlend;            // Detail texture blend
    float       environmentBlend;       // Environment reflection blend
    
    // Surface properties
    float       smoothness;             // Inverted roughness
    float       glossiness;             // Specular glossiness
    float       reflectivity;           // Reflection amount
    float       clearCoat;              // Clear coat layer
    float       clearCoatRoughness;     // Clear coat roughness
    
    // Subsurface scattering
    float       subsurface;             // SSS amount
    vec3_t      subsurfaceColor;        // SSS color
    float       subsurfaceRadius;       // SSS radius
    
    // Anisotropy
    float       anisotropy;             // Anisotropic amount
    float       anisotropyRotation;     // Anisotropic rotation
} pbrProperties_t;

// ============================================================================
// Material Override Entry
// ============================================================================

typedef struct materialOverride_s {
    char                name[MAX_QPATH];
    int                 flags;
    
    // PBR properties
    pbrProperties_t     pbr;
    
    // Texture maps
    image_t             *maps[MATMAP_COUNT];
    char                mapPaths[MATMAP_COUNT][MAX_QPATH];
    
    // Pixel data for export (stored after generation)
    byte                *mapData[MATMAP_COUNT];
    int                 mapWidth[MATMAP_COUNT];
    int                 mapHeight[MATMAP_COUNT];
    
    // Generation parameters
    float               generateTime;
    int                 generateQuality;
    int                 generateResolution;
    qboolean            generated;          // Maps have been generated
    qboolean            needsGeneration;    // Needs to regenerate maps
    
    // Cache info
    int                 cacheSize;
    int                 lastAccess;
    
    // Original material reference
    material_t          *baseMaterial;
    image_t             *baseTexture;
    
    // Next in hash chain
    struct materialOverride_s *hashNext;
} materialOverride_t;

// ============================================================================
// Map Generation Parameters
// ============================================================================

typedef struct mapGenParams_s {
    // Sobel edge detection
    float       sobelStrength;          // Edge detection strength
    float       sobelThreshold;         // Edge threshold
    int         sobelKernelSize;        // Kernel size (3, 5, 7)
    
    // Luminance analysis
    float       luminanceWeight;        // Weight for roughness calc
    float       luminanceInvert;        // Invert luminance mapping
    float       luminanceGamma;         // Gamma correction
    
    // Frequency analysis
    float       frequencyScale;         // Frequency scaling
    float       frequencyCutoff;        // High frequency cutoff
    int         frequencyOctaves;       // Number of octaves
    
    // Chromatic analysis
    float       chromaticThreshold;     // Metal detection threshold
    float       chromaticSaturation;    // Saturation threshold
    vec3_t      chromaticBias;          // Color bias for metals
    
    // Local contrast
    float       contrastRadius;         // Analysis radius
    float       contrastStrength;       // Contrast strength
    float       contrastBias;           // Contrast bias
    
    // Fusion weights
    float       fusionWeights[5];       // Weights for each method
    
    // Quality settings
    int         iterations;             // Processing iterations
    int         samples;                // Sampling count
    float       denoise;                // Denoising strength
    
    // Material hints
    qboolean    isMetal;                // Hint: metallic surface
    qboolean    isStone;                // Hint: stone/concrete
    qboolean    isOrganic;              // Hint: organic material
    qboolean    isLiquid;               // Hint: liquid/wet surface
} mapGenParams_t;

// ============================================================================
// Generation Methods
// ============================================================================

// Edge detection for normal maps
typedef struct edgeDetector_s {
    float       kernel[9];              // Convolution kernel
    float       strength;               // Detection strength
    float       threshold;              // Edge threshold
    int         passes;                 // Number of passes
} edgeDetector_t;

// Frequency analyzer
typedef struct freqAnalyzer_s {
    float       *spectrum;              // Frequency spectrum
    int         spectrumSize;           // Spectrum size
    float       *weights;               // Frequency weights
    int         blockSize;              // Analysis block size
} freqAnalyzer_t;

// ============================================================================
// Global State
// ============================================================================

typedef struct materialOverrideSystem_s {
    // Override database
    materialOverride_t  *overrides[MAX_OVERRIDE_MATERIALS];
    int                 numOverrides;
    materialOverride_t  *hashTable[1024];
    
    // Generation system
    mapGenParams_t      defaultParams;
    qboolean            autoGenerate;
    qboolean            exportEnabled;
    int                 generationQueue[MAX_OVERRIDE_MATERIALS];
    int                 queueCount;
    
    // Cache management
    byte                *cacheMemory;
    int                 cacheUsed;
    int                 cacheSize;
    
    // Statistics
    int                 mapsGenerated;
    int                 mapsLoaded;
    float               totalGenTime;
    
    // Thread pool for async generation
    qboolean            asyncGeneration;
    int                 numThreads;
    void                *threadPool;
} materialOverrideSystem_t;

extern materialOverrideSystem_t matOverrideSystem;

// ============================================================================
// Core Functions
// ============================================================================

// System initialization
void MatOver_Init(void);
void MatOver_Shutdown(void);
void MatOver_Reset(void);

// Shader integration
void MatOver_ProcessShader(shader_t *shader);

// Override loading
materialOverride_t* MatOver_Load(const char *materialName);
qboolean MatOver_LoadProperties(materialOverride_t *override, const char *path);
qboolean MatOver_LoadMaps(materialOverride_t *override, const char *path);
void MatOver_Apply(material_t *material, materialOverride_t *override);

// Override creation/export
materialOverride_t* MatOver_Create(material_t *material);
qboolean MatOver_Export(materialOverride_t *override);
qboolean MatOver_ExportProperties(materialOverride_t *override, const char *path);
qboolean MatOver_ExportMaps(materialOverride_t *override, const char *path);

// Map generation
void MatOver_GenerateMaps(materialOverride_t *override);
void MatOver_GenerateNormalMap(materialOverride_t *override);
void MatOver_GenerateRoughnessMap(materialOverride_t *override);
void MatOver_GenerateMetallicMap(materialOverride_t *override);
void MatOver_GenerateSpecularMap(materialOverride_t *override);
void MatOver_GenerateOcclusionMap(materialOverride_t *override);
void MatOver_GenerateHeightMap(materialOverride_t *override);

// Generation algorithms
void MatGen_SobelEdgeDetection(const byte *input, byte *output, int width, int height, const edgeDetector_t *params);
void MatGen_LuminanceAnalysis(const byte *input, byte *output, int width, int height, float weight, qboolean invert);
void MatGen_FrequencyAnalysis(const byte *input, byte *output, int width, int height, const freqAnalyzer_t *params);
void MatGen_ChromaticAnalysis(const byte *input, byte *output, int width, int height, float threshold, const vec3_t bias);
void MatGen_LocalContrastAnalysis(const byte *input, byte *output, int width, int height, float radius, float strength);

// Fusion method
void MatGen_FuseMaps(byte **inputs, byte *output, int width, int height, int numInputs, const float *weights);

// Advanced generation
void MatGen_EnhanceNormalMap(byte *normalMap, int width, int height, float strength);
void MatGen_GenerateDetailNormal(const byte *diffuse, byte *detail, int width, int height);
void MatGen_EstimateRoughness(const byte *diffuse, const byte *normal, byte *roughness, int width, int height);
void MatGen_DetectMetallic(const byte *diffuse, byte *metallic, int width, int height);
// void MatGen_ComputeAmbientOcclusion(const byte *normal, const byte *heightMap, byte *ao, int width, int height);

// Utility functions
void MatOver_CreateDirectory(const char *path);
qboolean MatOver_FileExists(const char *path);
void MatOver_WriteImage(const char *path, const byte *data, int width, int height, int components);
byte* MatOver_ReadImage(const char *path, int *width, int *height, int *components);

// Hash table
unsigned int MatOver_Hash(const char *name);
void MatOver_AddToHash(materialOverride_t *override);
materialOverride_t* MatOver_FindInHash(const char *name);

// Cache management
void MatOver_InitCache(int sizeInMB);
void MatOver_FreeCache(void);
byte* MatOver_AllocCache(int size);
void MatOver_CompactCache(void);

// Processing queue
void MatOver_QueueGeneration(materialOverride_t *override);
void MatOver_ProcessQueue(void);
qboolean MatOver_IsQueued(const char *name);

// Console commands
void MatOver_RegenerateAll_f(void);
void MatOver_ExportAll_f(void);
void MatOver_ClearCache_f(void);
void MatOver_Status_f(void);
void MatOver_ListOverrides_f(void);

// CVARs
extern cvar_t *r_materialOverride;
extern cvar_t *r_materialAutoGen;
extern cvar_t *r_materialExport;
extern cvar_t *r_materialGenQuality;
extern cvar_t *r_materialGenResolution;
extern cvar_t *r_materialGenAsync;
extern cvar_t *r_materialCacheSize;
extern cvar_t *r_materialDebug;

// Generation parameter CVARs
extern cvar_t *r_matgen_sobelStrength;
extern cvar_t *r_matgen_sobelThreshold;
extern cvar_t *r_matgen_luminanceWeight;
extern cvar_t *r_matgen_frequencyScale;
extern cvar_t *r_matgen_chromaticThreshold;
extern cvar_t *r_matgen_contrastRadius;
extern cvar_t *r_matgen_fusionNormal;
extern cvar_t *r_matgen_fusionRoughness;
extern cvar_t *r_matgen_fusionMetallic;

#endif // TR_MATERIAL_OVERRIDE_H