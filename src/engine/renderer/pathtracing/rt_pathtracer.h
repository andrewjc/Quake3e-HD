/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Ultra-Fast Path Traced Dynamic Lighting System
Hybrid approach combining rasterization with ray-traced lighting
===========================================================================
*/

#ifndef RT_PATHTRACER_H
#define RT_PATHTRACER_H

#include "../tr_local.h"

// ============================================================================
// Path Tracing Configuration
// ============================================================================

#define RT_MAX_BOUNCES          3       // Maximum light bounces
#define RT_SAMPLES_PER_PIXEL    2       // Samples per pixel per frame
#define RT_TEMPORAL_SAMPLES     16      // Temporal accumulation samples
#define RT_MAX_LIGHTS           256     // Maximum dynamic lights to trace
#define RT_MAX_STATIC_LIGHTS    1024    // Maximum static lights to trace
#define RT_CACHE_SIZE           65536   // Light cache entries
#define RT_PROBE_GRID_SIZE      32      // Irradiance probe grid resolution

// Ray tracing quality levels
typedef enum {
    RT_QUALITY_OFF,
    RT_QUALITY_LOW,         // 1 bounce, 1 sample
    RT_QUALITY_MEDIUM,      // 2 bounces, 2 samples
    RT_QUALITY_HIGH,        // 3 bounces, 4 samples
    RT_QUALITY_ULTRA        // 5 bounces, 8 samples, full GI
} rtQuality_t;

// Path tracing mode
typedef enum {
    RT_MODE_OFF,           // Path tracing disabled
    RT_MODE_DYNAMIC,       // Dynamic lights only (current)
    RT_MODE_ALL           // All lighting (static + dynamic)
} rtMode_t;

// ============================================================================
// Ray Structure
// ============================================================================

typedef struct ray_s {
    vec3_t  origin;
    vec3_t  direction;
    float   tMin;
    float   tMax;
    int     depth;          // Recursion depth
    float   ior;           // Index of refraction
} ray_t;

// ============================================================================
// Ray Hit Information
// ============================================================================

typedef struct hitInfo_s {
    float       t;              // Distance along ray
    vec3_t      point;          // World space hit point
    vec3_t      normal;         // Surface normal
    vec2_t      uv;            // Texture coordinates
    shader_t    *shader;       // Material/shader
    int         surfaceFlags;
    int         contents;
    
    // Shading data
    vec3_t      albedo;
    float       metallic;
    float       roughness;
    vec3_t      emission;
    
    // Geometry
    int         triangleIndex;
    int         primitiveID;
    struct mnode_s *node;      // BSP node
} hitInfo_t;

// ============================================================================
// Acceleration Structure - Enhanced BSP
// ============================================================================

typedef struct rtBspNode_s {
    // BSP data
    struct mnode_s  *node;
    cplane_t        *plane;
    
    // Bounding box for fast rejection
    vec3_t          mins;
    vec3_t          maxs;
    
    // Surface lists
    int             firstSurface;
    int             numSurfaces;
    
    // Light lists (cached)
    int             firstLight;
    int             numLights;
    
    // Children
    struct rtBspNode_s *children[2];
} rtBspNode_t;

// ============================================================================
// Light Cache for Temporal Coherence
// ============================================================================

typedef struct lightCacheEntry_s {
    vec3_t      position;       // World position
    vec3_t      irradiance;     // Cached indirect lighting
    vec3_t      normal;         // Surface normal
    float       confidence;     // Confidence value (0-1)
    int         frameUpdated;   // Last update frame
    int         sampleCount;    // Number of samples accumulated
} lightCacheEntry_t;

// ============================================================================
// Irradiance Probe for Global Illumination
// ============================================================================

typedef struct irradianceProbe_s {
    vec3_t      position;
    vec3_t      irradiance[6];  // 6 directions (cube faces)
    float       visibility[6];   // Visibility for each direction
    int         lastUpdate;
    qboolean    dynamic;
} irradianceProbe_t;

// ============================================================================
// Static Light Structure
// ============================================================================

typedef struct staticLight_s {
    vec3_t          origin;         // Light position
    vec3_t          color;          // Light color
    float           intensity;      // Light intensity
    float           radius;         // Light radius
    int             type;           // Light type (point, spot, etc)
    vec3_t          direction;      // For spotlights
    float           spotAngle;      // Spotlight cone angle
    qboolean        castShadows;    // Shadow casting
} staticLight_t;

// ============================================================================
// Path Tracer State
// ============================================================================

typedef struct pathTracer_s {
    // Configuration
    rtQuality_t     quality;
    rtMode_t        mode;           // Lighting mode
    int             maxBounces;
    int             samplesPerPixel;
    qboolean        enabled;
    
    // Acceleration structure
    rtBspNode_t     *bspTree;
    int             numNodes;
    
    // Light sources
    staticLight_t   *staticLights;
    int             numStaticLights;
    int             maxStaticLights;
    
    // Light cache
    lightCacheEntry_t *lightCache;
    int             cacheSize;
    unsigned int    cacheHash;
    
    // Irradiance probes
    irradianceProbe_t *probes;
    int             numProbes;
    vec3_t          probeGridOrigin;
    vec3_t          probeGridSize;
    
    // Temporal data
    float           *accumBuffer;      // Accumulated samples
    float           *varianceBuffer;   // Variance estimation
    int             *sampleBuffer;     // Sample count per pixel
    int             currentFrame;
    
    // Denoising
    float           *denoisedBuffer;
    float           denoiseSigma;
    float           denoiseThreshold;
    
    // Statistics
    int             raysTraced;
    int             triangleTests;
    int             boxTests;
    float           traceTime;
} pathTracer_t;

// ============================================================================
// Global Path Tracer Instance
// ============================================================================

extern pathTracer_t rt;

// ============================================================================
// Core Functions
// ============================================================================

// Initialization
void RT_InitPathTracer(void);
void RT_ShutdownPathTracer(void);
void RT_BuildAccelerationStructure(void);

// Ray tracing
qboolean RT_TraceRay(const ray_t *ray, hitInfo_t *hit);
qboolean RT_TraceShadowRay(const vec3_t origin, const vec3_t target, float maxDist);
void RT_TracePath(const ray_t *ray, int depth, vec3_t result);
void RT_ComputeLightingAtPoint(const vec3_t point, vec3_t result);

// Intersection tests
qboolean RT_RayTriangleIntersect(const ray_t *ray, const vec3_t v0, const vec3_t v1, const vec3_t v2, float *t, vec2_t *uv);
qboolean RT_RayBoxIntersect(const ray_t *ray, const vec3_t mins, const vec3_t maxs, float *tMin, float *tMax);
qboolean RT_RayBSPIntersect(const ray_t *ray, rtBspNode_t *node, hitInfo_t *hit);

// Shading
void RT_EvaluateBRDF(const vec3_t wi, const vec3_t wo, const vec3_t normal, const vec3_t albedo, float roughness, float metallic, vec3_t result);
void RT_SampleBRDF(const vec3_t wo, const vec3_t normal, float roughness, vec3_t wi, float *pdf, vec3_t result);
void RT_EvaluateDirectLighting(const hitInfo_t *hit, const vec3_t wo, vec3_t result);
void RT_EvaluateIndirectLighting(const hitInfo_t *hit, const vec3_t wo, int depth, vec3_t result);
void RT_EvaluateStaticLighting(const hitInfo_t *hit, const vec3_t wo, vec3_t result);

// Sampling
void RT_GenerateRay(int x, int y, int sample, ray_t *ray);
void RT_CosineSampleHemisphere(const vec3_t normal, vec3_t result);
void RT_UniformSampleHemisphere(const vec3_t normal, vec3_t result);
void RT_HammersleySequence(int i, int n, vec2_t result);
float RT_RadicalInverse(unsigned int bits);

// Light cache
void RT_InitLightCache(void);
void RT_UpdateLightCache(const vec3_t pos, const vec3_t normal, const vec3_t irradiance);
qboolean RT_QueryLightCache(const vec3_t pos, const vec3_t normal, vec3_t irradiance);
void RT_ClearLightCache(void);

// Irradiance probes
void RT_InitProbeGrid(const vec3_t mins, const vec3_t maxs);
void RT_UpdateProbe(int probeIndex);
void RT_SampleProbeGrid(const vec3_t pos, const vec3_t normal, vec3_t result);

// Denoising
void RT_InitDenoiser(void);
void RT_DenoiseFrame(float *input, float *output, int width, int height);
void RT_ApplyTemporalFilter(float *current, float *history, float *output, int width, int height);
void RT_ApplySpatialFilter(float *input, float *output, int width, int height);

// Temporal accumulation
void RT_InitTemporalBuffers(void);
void RT_AccumulateSample(int x, int y, const vec3_t color);
void RT_GetAccumulatedColor(int x, int y, vec3_t result);
void RT_ResetAccumulation(void);

// Integration with main renderer
void RT_RenderPathTracedLighting(void);
void RT_UpdateDynamicLights(void);
void RT_ExtractStaticLights(void);
void RT_BeginFrame(void);
void RT_EndFrame(void);

// Debug visualization
void RT_DrawDebugRays(void);
void RT_DrawProbeGrid(void);
void RT_DrawLightCache(void);

// CVARs
extern cvar_t *rt_enable;
extern cvar_t *rt_mode;            // New: lighting mode (off/dynamic/all)
extern cvar_t *rt_quality;
extern cvar_t *rt_bounces;
extern cvar_t *rt_samples;
extern cvar_t *rt_denoise;
extern cvar_t *rt_temporal;
extern cvar_t *rt_probes;
extern cvar_t *rt_cache;
extern cvar_t *rt_debug;
extern cvar_t *rt_staticLights;    // New: enable static light extraction

// Full-screen ray tracing dispatch
void RT_AllocateScreenBuffers(int width, int height);
void RT_RenderFullScreen(void);
void RT_CopyToFramebuffer(void);
void RT_ScreenDispatchCommands(void);
void RT_FreeScreenBuffers(void);

#endif // RT_PATHTRACER_H