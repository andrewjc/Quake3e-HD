/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

RTX Hardware Raytracing Acceleration
Vulkan Ray Tracing (VK_KHR_ray_tracing) support
===========================================================================
*/

#ifndef RT_RTX_H
#define RT_RTX_H

#include "../core/tr_local.h"
#include "rt_pathtracer.h"
#include "../vulkan/vk.h"

// ============================================================================
// RTX Configuration
// ============================================================================

#define RTX_MAX_INSTANCES       4096    // Maximum geometry instances
#define RTX_MAX_MATERIALS       1024    // Maximum material bindings
#define RTX_MAX_RECURSION       8        // Maximum ray recursion depth
#define RTX_SHADER_TABLE_SIZE   65536    // Shader binding table size

// RTX feature flags
typedef enum {
    RTX_FEATURE_NONE            = 0,
    RTX_FEATURE_BASIC           = (1 << 0),  // Basic ray tracing
    RTX_FEATURE_MOTION_BLUR     = (1 << 1),  // Motion vectors
    RTX_FEATURE_RAY_QUERY       = (1 << 2),  // Inline ray queries
    RTX_FEATURE_INDIRECT        = (1 << 3),  // Indirect ray dispatch
    RTX_FEATURE_DLSS            = (1 << 4),  // NVIDIA DLSS
    RTX_FEATURE_REFLEX          = (1 << 5),  // NVIDIA Reflex
    RTX_FEATURE_DENOISER        = (1 << 6),  // Hardware denoiser
    RTX_FEATURE_RAY_TRACING     = (1 << 7),  // Ray tracing support
} rtxFeature_t;

// GPU vendor types
typedef enum {
    RTX_GPU_UNKNOWN = 0,
    RTX_GPU_NVIDIA,
    RTX_GPU_AMD,
    RTX_GPU_INTEL
} rtxGpuType_t;

// ============================================================================
// Acceleration Structure Types
// ============================================================================

// Bottom Level Acceleration Structure (BLAS) - per model
typedef struct rtxBLAS_s {
    void                    *handle;           // API-specific handle
    int                     numTriangles;
    int                     numVertices;
    vec3_t                  *vertices;
    unsigned int            *indices;
    vec3_t                  aabbMin;
    vec3_t                  aabbMax;
    qboolean                isDynamic;
    unsigned int            buildFlags;
    size_t                  scratchSize;
    void                    *scratchBuffer;
} rtxBLAS_t;

// Top Level Acceleration Structure (TLAS) - scene
typedef struct rtxTLAS_s {
    void                    *handle;           // API-specific handle
    int                     numInstances;
    struct rtxInstance_s    *instances;
    unsigned int            buildFlags;
    size_t                  scratchSize;
    void                    *scratchBuffer;
    qboolean                needsRebuild;
} rtxTLAS_t;

// Instance data for TLAS
typedef struct rtxInstance_s {
    rtxBLAS_t               *blas;
    float                   transform[12];     // 3x4 transform matrix
    unsigned int            instanceID;
    unsigned int            mask;
    unsigned int            shaderOffset;
    unsigned int            flags;
    material_t              *material;
} rtxInstance_t;

// ============================================================================
// Shader Binding Table
// ============================================================================

typedef struct rtxShaderRecord_s {
    void                    *shaderIdentifier;
    size_t                  identifierSize;
    void                    *localRootData;
    size_t                  localRootSize;
} rtxShaderRecord_t;

typedef struct rtxShaderTable_s {
    rtxShaderRecord_t       *raygenShaders;
    rtxShaderRecord_t       *missShaders;
    rtxShaderRecord_t       *hitShaders;
    rtxShaderRecord_t       *callableShaders;
    int                     numRaygen;
    int                     numMiss;
    int                     numHit;
    int                     numCallable;
    void                    *tableBuffer;
    size_t                  tableSize;
} rtxShaderTable_t;

// ============================================================================
// Ray Dispatch Parameters
// ============================================================================

typedef struct rtxDispatchRays_s {
    int                     width;
    int                     height;
    int                     depth;
    rtxShaderTable_t        *shaderTable;
    int                     maxRecursion;
} rtxDispatchRays_t;

// ============================================================================
// RTX Pipeline State
// ============================================================================

typedef struct rtxPipeline_s {
    void                    *handle;           // API pipeline handle
    void                    *pipelineLayout;   // VK pipeline layout
    rtxShaderTable_t        shaderTable;
    int                     maxRecursion;
    unsigned int            flags;
} rtxPipeline_t;

// ============================================================================
// RTX Denoiser
// ============================================================================

typedef struct rtxDenoiser_s {
    qboolean                enabled;
    void                    *context;
    void                    *inputBuffer;      // Noisy input
    void                    *albedoBuffer;     // Material albedo
    void                    *normalBuffer;     // World normals
    void                    *motionBuffer;     // Motion vectors
    void                    *outputBuffer;     // Denoised output
    int                     width;
    int                     height;
    float                   blendFactor;       // Temporal blend
} rtxDenoiser_t;

// ============================================================================
// RTX Global State
// ============================================================================

typedef struct rtxState_s {
    // Capabilities
    qboolean                available;
    unsigned int            features;
    int                     rayTracingTier;
    rtxGpuType_t            gpuType;
    
    // Device resources
    void                    *device;           // VkDevice
    void                    *commandList;      // Command list/buffer
    void                    *descriptorHeap;   // Descriptor heap/pool
    
    // Acceleration structures
    rtxTLAS_t               tlas;
    rtxBLAS_t               *blasPool;
    int                     numBLAS;
    int                     maxBLAS;
    
    // Pipeline
    rtxPipeline_t           primaryPipeline;   // Primary rays
    rtxPipeline_t           shadowPipeline;    // Shadow rays
    rtxPipeline_t           giPipeline;        // Global illumination
    
    // Output buffers
    void                    *colorBuffer;
    void                    *depthBuffer;
    void                    *normalBuffer;
    void                    *motionBuffer;
    
    // Denoiser
    rtxDenoiser_t           denoiser;
    
    // Performance
    unsigned int            frameCount;
    float                   buildTime;
    float                   traceTime;
    float                   denoiseTime;
} rtxState_t;

// ============================================================================
// Global RTX Instance
// ============================================================================

extern rtxState_t rtx;

// ============================================================================
// Core RTX Functions
// ============================================================================

// Initialization
qboolean RTX_Init(void);
void RTX_Shutdown(void);
qboolean RTX_IsAvailable(void);
unsigned int RTX_GetFeatures(void);

// Acceleration structure management
rtxBLAS_t* RTX_CreateBLAS(const vec3_t *vertices, int numVerts, 
                          const unsigned int *indices, int numIndices,
                          qboolean isDynamic);
void RTX_DestroyBLAS(rtxBLAS_t *blas);
void RTX_UpdateBLAS(rtxBLAS_t *blas, const vec3_t *vertices);

rtxTLAS_t* RTX_CreateTLAS(int maxInstances);
void RTX_DestroyTLAS(rtxTLAS_t *tlas);
void RTX_AddInstance(rtxTLAS_t *tlas, rtxBLAS_t *blas, 
                     const float *transform, material_t *material);
void RTX_BuildTLAS(rtxTLAS_t *tlas);

// Pipeline management
rtxPipeline_t* RTX_CreatePipeline(const char *shaderPath, int maxRecursion);
void RTX_DestroyPipeline(rtxPipeline_t *pipeline);
void RTX_BindPipeline(rtxPipeline_t *pipeline);

// Ray dispatch
void RTX_DispatchRays(const rtxDispatchRays_t *params);
void RTX_TraceScene(int width, int height);

// Denoising
qboolean RTX_InitDenoiser(int width, int height);
void RTX_DenoiseFrame(void *input, void *output);
void RTX_ShutdownDenoiser(void);

// Resource management
void* RTX_CreateBuffer(size_t size, unsigned int usage);
void RTX_DestroyBuffer(void *buffer);
void RTX_UpdateBuffer(void *buffer, const void *data, size_t size);

// Synchronization
void RTX_BeginFrame(void);
void RTX_EndFrame(void);
void RTX_WaitForCompletion(void);

// Debug
void RTX_DrawDebugOverlay(void);
void RTX_DumpStats(void);

// ============================================================================
// Vulkan Ray Tracing implementation
// ============================================================================

qboolean RTX_InitVulkanRT(void);
void RTX_ShutdownVulkanRT(void);
void RTX_BuildAccelerationStructureVK(void);
void RTX_DispatchRaysVK(const rtxDispatchRays_t *params);
VkDeviceAddress RTX_GetBufferDeviceAddress(VkBuffer buffer);

// ============================================================================
// Pipeline Management
// ============================================================================

qboolean RTX_InitializePipeline(void);
void RTX_ShutdownPipeline(void);
VkPipeline RTX_GetPipeline(void);
VkPipelineLayout RTX_GetPipelineLayout(void);
VkDescriptorSet RTX_GetDescriptorSet(void);
void RTX_GetSBTRegions(VkStridedDeviceAddressRegionKHR *raygen,
                      VkStridedDeviceAddressRegionKHR *miss,
                      VkStridedDeviceAddressRegionKHR *hit,
                      VkStridedDeviceAddressRegionKHR *callable);
void RTX_UpdateDescriptorSets(VkAccelerationStructureKHR tlas,
                             VkImageView colorImage, VkImageView albedoImage,
                             VkImageView normalImage, VkImageView motionImage,
                             VkImageView depthImage);

// ============================================================================
// Material System
// ============================================================================

void RTX_InitMaterialCache(void);
void RTX_ShutdownMaterialCache(void);
void RTX_BuildMaterialBuffer(void);
void RTX_UploadMaterialBuffer(VkDevice device, VkCommandBuffer commandBuffer,
                              VkBuffer materialBuffer);
qboolean RTX_IsMaterialCacheDirty(void);
int RTX_GetNumMaterials(void);

// ============================================================================
// Scene Management
// ============================================================================

void RTX_UpdateScene(void);
void RTX_UpdateCamera(const refdef_t *refdef);
void RTX_UpdateLights(void);
void RTX_CopyToBackbuffer(void);

// ============================================================================
// Integration with path tracer
// ============================================================================

void RTX_AcceleratePathTracing(const ray_t *ray, hitInfo_t *hit);
void RTX_ShadowRayQuery(const vec3_t origin, const vec3_t target, float *visibility);
void RTX_AmbientOcclusionQuery(const vec3_t pos, const vec3_t normal, float *ao);

// ============================================================================
// DLSS Integration
// ============================================================================

typedef enum {
    DLSS_MODE_OFF,
    DLSS_MODE_PERFORMANCE,      // 2x upscaling
    DLSS_MODE_BALANCED,         // 1.7x upscaling
    DLSS_MODE_QUALITY,          // 1.5x upscaling
    DLSS_MODE_ULTRA_PERFORMANCE // 3x upscaling
} dlssMode_t;

qboolean RTX_InitDLSS(void);
void RTX_SetDLSSMode(dlssMode_t mode);
void RTX_UpscaleWithDLSS(void *input, void *output, int inputWidth, int inputHeight);
void RTX_ShutdownDLSS(void);

// ============================================================================
// CVARs
// ============================================================================

extern cvar_t *rtx_enable;
extern cvar_t *rtx_quality;        // 0=off, 1=low, 2=medium, 3=high, 4=ultra
extern cvar_t *rtx_denoise;
extern cvar_t *rtx_dlss;
extern cvar_t *rtx_reflex;
extern cvar_t *rtx_gi_bounces;
extern cvar_t *rtx_reflection_quality;
extern cvar_t *rtx_shadow_quality;
extern cvar_t *rtx_debug;

#endif // RT_RTX_H