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

#ifndef TR_LIGHT_DYNAMIC_H
#define TR_LIGHT_DYNAMIC_H

#include "../tr_local.h"

/*
================================================================================
Phase 5: Dynamic Lighting Data Structures

This file defines the enhanced dynamic lighting system that supports per-pixel
lighting, shadows, and multiple light types.
================================================================================
*/

// Matrix types
typedef vec_t mat3_t[9];   // 3x3 matrix
typedef vec_t mat4_t[16];  // 4x4 matrix

// Light types
typedef enum {
    RL_OMNI,        // Point light (sphere)
    RL_PROJ,        // Projected/spot light (frustum)
    RL_DIRECTIONAL, // Sun/directional light
    RL_AMBIENT,     // Ambient light volume
    RL_FOG          // Fog volume
} renderLightType_t;

// Light flags
#define LIGHTFLAG_NOSHADOWS         0x0001
#define LIGHTFLAG_NOSPECULAR        0x0002
#define LIGHTFLAG_NODIFFUSE         0x0004
#define LIGHTFLAG_NOSELFSHADOW      0x0008
#define LIGHTFLAG_FORCESHADOWS      0x0010
#define LIGHTFLAG_PARALLEL          0x0020
#define LIGHTFLAG_POINTLIGHT        0x0040
#define LIGHTFLAG_SPOTLIGHT         0x0080
#define LIGHTFLAG_VOLUMETRIC        0x0100
#define LIGHTFLAG_STATIC            0x0200

// Forward declarations
typedef struct interaction_s interaction_t;
typedef struct shadowMapInfo_s shadowMapInfo_t;

// Main light structure
typedef struct renderLight_s {
    // Identification
    int                 index;              // Light index
    int                 flags;              // LIGHTFLAG_* flags
    renderLightType_t   type;               // Light type
    
    // Transform
    vec3_t              origin;             // World position
    mat3_t              axis;               // Orientation (3x3 matrix)
    
    // Light volume (for culling)
    vec3_t              mins;               // AABB minimum
    vec3_t              maxs;               // AABB maximum
    float               radius;             // Bounding sphere radius
    
    // Light properties
    vec3_t              color;              // RGB color (normalized)
    float               intensity;          // Brightness multiplier
    
    // Attenuation
    float               constant;           // Constant attenuation
    float               linear;             // Linear attenuation
    float               quadratic;          // Quadratic attenuation
    float               cutoffDistance;     // Maximum influence distance
    
    // Projection (for spot/projected lights)
    float               fovX;               // Horizontal FOV
    float               fovY;               // Vertical FOV
    vec3_t              target;             // Look-at point
    float               nearClip;           // Near clip plane
    float               farClip;            // Far clip plane
    mat4_t              projectionMatrix;   // Light projection matrix
    mat4_t              viewMatrix;         // Light view matrix
    
    // Texture projection
    image_t             *projectionImage;   // Cookie/gobo texture
    image_t             *falloffImage;      // Falloff texture
    
    // Shadow properties
    int                 shadowLod;          // Shadow detail level (0-3)
    float               shadowBias;         // Shadow depth bias
    float               shadowSoftness;     // Soft shadow radius
    shadowMapInfo_t     *shadowMap;         // Shadow map info
    
    // Culling info
    int                 viewCount;          // Last frame this light was visible
    int                 areaNum;            // BSP area containing light
    struct renderLight_s *areaNext;        // Next light in same area
    int                 scissorRect[4];     // Screen-space scissor rectangle
    
    // Interaction chains
    interaction_t       *firstInteraction;  // First surface affected by this light
    interaction_t       *lastInteraction;   // Last surface (for fast append)
    int                 numInteractions;    // Total interaction count
    
    // Performance stats
    int                 numShadowCasters;   // Surfaces casting shadows
    int                 numLitSurfaces;     // Surfaces being lit
    
    // Dynamic properties
    qboolean            isStatic;           // Static light (can be cached)
    qboolean            needsUpdate;        // Interactions need rebuilding
    int                 lastUpdateFrame;    // Frame of last update
    
} renderLight_t;

// Light-Surface Interaction Structure
struct interaction_s {
    // Linked list pointers (intrusive lists)
    interaction_t       *lightNext;        // Next surface for this light
    interaction_t       *lightPrev;        // Previous surface for this light
    interaction_t       *surfaceNext;      // Next light for this surface  
    interaction_t       *surfacePrev;      // Previous light for this surface
    
    // References
    renderLight_t       *light;            // The light
    msurface_t          *surface;          // The surface
    
    // Cached lighting data
    srfTriangles_t      *lightTris;        // Triangles facing the light
    int                 numLightTris;      // Number of lit triangles
    srfTriangles_t      *shadowTris;       // Shadow volume triangles
    int                 numShadowTris;     // Number of shadow triangles
    
    // Culling/visibility
    vec3_t              bounds[2];          // Interaction AABB
    qboolean            culled;             // Culled this frame
    int                 scissorRect[4];     // Screen-space scissor rectangle
    float               depthBounds[2];     // Min/max depth for depth bounds test
    
    // Optimization flags
    qboolean            isStatic;           // Can be cached
    qboolean            isEmpty;            // No triangles affected
    qboolean            castsShadow;        // Surface casts shadow from light
    qboolean            receivesLight;      // Surface receives light
    
    // Dynamic updates
    int                 lastUpdateFrame;    // Last frame this was updated
    int                 dynamicFrameCount;  // Frames since last update
    
    // Memory management
    interaction_t       *nextFree;          // For free list
    int                 index;              // Interaction pool index
};

// Interaction manager
typedef struct {
    interaction_t       *interactions;      // Pool of interactions
    int                 numInteractions;    // Current count
    int                 maxInteractions;    // Pool size
    interaction_t       *freeList;          // Free interaction list
    
    // Statistics
    int                 numStaticCached;    // Cached static interactions
    int                 numDynamicCreated;  // Dynamic interactions this frame
    int                 numCulled;          // Culled interactions
    
} interactionManager_t;

// Shadow map information
struct shadowMapInfo_s {
    int                 width;              // Shadow map width
    int                 height;             // Shadow map height
    int                 lod;                // Level of detail
    image_t             *depthImage;        // Depth texture
    mat4_t              lightViewMatrix;    // Light view matrix
    mat4_t              lightProjMatrix;    // Light projection matrix
    float               zNear;              // Near plane
    float               zFar;               // Far plane
    qboolean            needsUpdate;        // Needs re-rendering
};

// Light management constants
#define MAX_RENDER_LIGHTS   256
#define MAX_INTERACTIONS    4096
#define MAX_SHADOW_MAPS     32

// Light system management
typedef struct lightSystem_s {
    // Light storage
    renderLight_t       lights[MAX_RENDER_LIGHTS];
    int                 numLights;
    
    // Light lists
    renderLight_t       *activeLights[MAX_RENDER_LIGHTS];
    int                 numActiveLights;
    renderLight_t       *visibleLights[MAX_RENDER_LIGHTS];
    int                 numVisibleLights;
    
    // Area-based light lists (for PVS culling)
    renderLight_t       *areaLights[MAX_MAP_AREAS];
    
    // Interaction management
    interactionManager_t interactionMgr;
    
    // Shadow map pool
    shadowMapInfo_t     shadowMaps[MAX_SHADOW_MAPS];
    int                 numShadowMaps;
    
    // Frame counters
    int                 frameCount;
    int                 visCount;
    
    // Performance stats
    int                 totalLightTests;
    int                 totalLightCulled;
    int                 totalInteractionTests;
    int                 totalInteractionCulled;
    
} lightSystem_t;

// Global light system
extern lightSystem_t tr_lightSystem;

// Light creation and management
renderLight_t* R_AllocRenderLight(void);
void R_FreeRenderLight(renderLight_t *light);
void R_InitRenderLight(renderLight_t *light);
void R_UpdateRenderLight(renderLight_t *light);

// Light-surface interaction
interaction_t* R_CreateInteraction(renderLight_t *light, msurface_t *surf);
void R_FreeInteraction(interaction_t *inter);
void R_UpdateInteraction(interaction_t *inter);
void R_LinkInteraction(interaction_t *inter);
void R_UnlinkInteraction(interaction_t *inter);

// Light culling
void R_CullLights(void);
qboolean R_CullLightBounds(renderLight_t *light);
void R_AddLightToArea(renderLight_t *light, int areaNum);
void R_RemoveLightFromArea(renderLight_t *light);

// Light queries
renderLight_t* R_GetNearestLight(const vec3_t point);
int R_GatherLights(const vec3_t mins, const vec3_t maxs, renderLight_t **list, int maxLights);
qboolean R_LightAffectsSurface(renderLight_t *light, msurface_t *surf);

// System management
void R_InitLightSystem(void);
void R_ShutdownLightSystem(void);
void R_ClearLights(void);
void R_UpdateLightSystem(void);

// Debug visualization
void R_DrawLightVolumes(void);
void R_DrawLightInteractions(void);
void R_DrawShadowFrusta(void);

// Legacy compatibility
void R_ConvertDlightToRenderLight(dlight_t *dl, renderLight_t *rl);
void R_ProcessDynamicLights(void);

// Shadow mapping
shadowMapInfo_t* R_AllocShadowMap(renderLight_t *light);
void R_FreeShadowMap(shadowMapInfo_t *shadow);
void R_RenderShadowMap(renderLight_t *light);
void R_BindShadowMap(shadowMapInfo_t *shadow);

// Light properties
void R_SetLightColor(renderLight_t *light, float r, float g, float b);
void R_SetLightIntensity(renderLight_t *light, float intensity);
void R_SetLightAttenuation(renderLight_t *light, float constant, float linear, float quadratic);
void R_SetSpotLightAngles(renderLight_t *light, float fovX, float fovY);
void R_SetLightProjectionTexture(renderLight_t *light, const char *name);

// Performance monitoring
int R_GetActiveLightCount(void);
int R_GetVisibleLightCount(void);
int R_GetInteractionCount(void);
int R_GetShadowMapCount(void);

// Backend rendering functions (Phase 6)
void RB_SetupLightingShader(renderLight_t *light);
void RB_DrawInteraction(interaction_t *inter);
void RB_RenderBasePass(void);
void RB_RenderLightingPasses(void);
void RB_CalcLightGridColor(const vec3_t position, vec3_t color);
void RB_SetupEntityLighting(trRefEntity_t *ent);
void RB_LightingDebugVisualization(void);

// Scissoring optimization (Phase 7)
void R_CalcLightScissorRectangle(renderLight_t *light, viewParms_t *view, int *scissor);
void R_GetInteractionDepthBounds(interaction_t *inter, float *minDepth, float *maxDepth);
void R_SetDepthBoundsTest(interaction_t *inter);
void R_OptimizeLightScissors(void);
void R_ScissorStatistics(void);

#endif // TR_LIGHT_DYNAMIC_H