# Phase 5: Dynamic Lighting Data Structures

## Executive Summary

Phase 5 implements the CPU-side data structures for unified dynamic lighting, establishing the foundation for per-pixel lighting. This phase introduces renderLight_t and interaction_t structures that pre-calculate light-surface relationships, enabling efficient multi-light rendering in subsequent phases.

## Current State Analysis

### Existing Dynamic Light System

```c
// Current dlight structure (tr_local.h)
typedef struct dlight_s {
    vec3_t  origin;
    vec3_t  origin2;    // For linear lights
    vec3_t  dir;        // origin2 - origin
    vec3_t  color;
    float   radius;
    vec3_t  transformed;    // origin in local coordinate system
    int     additive;       // texture detail is lost tho when the lightmap is dark
} dlight_t;

// Current implementation:
// - Maximum 32 dynamic lights (MAX_DLIGHTS)
// - Per-vertex lighting only
// - Simple radius-based attenuation
// - No shadows from dynamic lights
```

## New Lighting System Design

### 1. Core Light Structure

```c
// File: src/engine/renderer/tr_light.h (NEW FILE)

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

// Main light structure
typedef struct renderLight_s {
    // Identification
    int                 index;              // Light index
    int                 flags;              // LIGHTFLAG_* flags
    renderLightType_t   type;               // Light type
    
    // Transform
    vec3_t              origin;             // World position
    mat3_t              axis;               // Orientation
    
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
    
    // Culling info
    int                 viewCount;          // Last frame this light was visible
    int                 areaNum;            // BSP area containing light
    struct renderLight_s *areaNext;        // Next light in same area
    
    // Interaction chains
    struct interaction_s *firstInteraction; // First surface affected by this light
    struct interaction_s *lastInteraction;  // Last surface (for fast append)
    int                 numInteractions;    // Total interaction count
    
    // Performance stats
    int                 numShadowCasters;   // Surfaces casting shadows
    int                 numLitSurfaces;     // Surfaces being lit
    
    // Dynamic properties
    qboolean            isStatic;           // Static light (can be cached)
    qboolean            needsUpdate;        // Interactions need rebuilding
    int                 lastUpdateFrame;    // Frame of last update
    
} renderLight_t;
```

### 2. Light-Surface Interaction Structure

```c
// File: src/engine/renderer/tr_interaction.h (NEW FILE)

// Interaction between a light and a surface
typedef struct interaction_s {
    // Linked list pointers (intrusive lists)
    struct interaction_s *lightNext;       // Next surface for this light
    struct interaction_s *lightPrev;       // Previous surface for this light
    struct interaction_s *surfaceNext;     // Next light for this surface  
    struct interaction_s *surfacePrev;     // Previous light for this surface
    
    // References
    renderLight_t       *light;            // The light
    drawSurf_t          *surface;          // The surface
    
    // Cached lighting data
    srfTriangles_t      *lightTris;        // Triangles facing the light
    int                 numLightTris;      // Number of lit triangles
    srfTriangles_t      *shadowTris;       // Shadow volume triangles
    int                 numShadowTris;     // Number of shadow triangles
    
    // Culling/visibility
    vec3_t              bounds[2];          // Interaction AABB
    qboolean            culled;             // Culled this frame
    int                 scissorRect[4];     // Screen-space scissor rectangle
    
    // Optimization flags
    qboolean            isStatic;           // Can be cached
    qboolean            isEmpty;            // No triangles affected
    qboolean            castsShadow;        // Surface casts shadow from light
    qboolean            receivesLight;      // Surface receives light
    
    // Dynamic updates
    int                 lastUpdateFrame;    // Last frame this was updated
    int                 dynamicFrameCount;  // Frames since last update
    
    // Memory management
    struct interaction_s *nextFree;        // For free list
    int                 index;              // Interaction pool index
    
} interaction_t;

// Interaction management
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
```

### 3. Light Management System

```c
// File: src/engine/renderer/lighting/tr_light_mgmt.c (NEW FILE)

#define MAX_RENDER_LIGHTS   256
#define MAX_INTERACTIONS    4096

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
    
    // Interaction manager
    interactionManager_t interactionManager;
    
    // Shadow casters
    drawSurf_t          *shadowCasters[MAX_DRAWSURFS];
    int                 numShadowCasters;
    
    // Performance
    int                 frameNumber;
    double              lightProcessTime;
    double              interactionTime;
    
} lightSystem_t;

static lightSystem_t tr_lightSystem;

// Initialize lighting system
void R_InitLightingSystem(void) {
    Com_Memset(&tr_lightSystem, 0, sizeof(tr_lightSystem));
    
    // Allocate interaction pool
    tr_lightSystem.interactionManager.maxInteractions = MAX_INTERACTIONS;
    tr_lightSystem.interactionManager.interactions = 
        ri.Hunk_Alloc(sizeof(interaction_t) * MAX_INTERACTIONS, h_low);
    
    // Initialize free list
    R_InitInteractionFreeList();
    
    // Register cvars
    r_dynamicLighting = ri.Cvar_Get("r_dynamicLighting", "1", CVAR_ARCHIVE);
    r_maxLights = ri.Cvar_Get("r_maxLights", "128", CVAR_ARCHIVE);
    r_lightCullDistance = ri.Cvar_Get("r_lightCullDistance", "8192", CVAR_ARCHIVE);
}
```

### 4. Light Creation and Management

```c
// File: src/engine/renderer/lighting/tr_light_create.c (NEW FILE)

// Create a new light
renderLight_t* R_CreateLight(void) {
    if (tr_lightSystem.numLights >= MAX_RENDER_LIGHTS) {
        ri.Printf(PRINT_WARNING, "R_CreateLight: MAX_RENDER_LIGHTS hit\n");
        return NULL;
    }
    
    renderLight_t *light = &tr_lightSystem.lights[tr_lightSystem.numLights++];
    Com_Memset(light, 0, sizeof(renderLight_t));
    
    // Set defaults
    light->index = tr_lightSystem.numLights - 1;
    light->type = RL_OMNI;
    light->intensity = 1.0f;
    light->constant = 1.0f;
    light->linear = 0.0f;
    light->quadratic = 1.0f;
    light->cutoffDistance = 512.0f;
    VectorSet(light->color, 1.0f, 1.0f, 1.0f);
    MatrixIdentity(light->axis);
    
    return light;
}

// Add light from game (replaces RE_AddLightToScene)
void RE_AddDynamicLight(const vec3_t origin, float radius, 
                       float r, float g, float b) {
    renderLight_t *light = R_CreateLight();
    if (!light) return;
    
    VectorCopy(origin, light->origin);
    light->radius = radius;
    light->cutoffDistance = radius;
    VectorSet(light->color, r, g, b);
    light->type = RL_OMNI;
    light->isStatic = qfalse;
    
    // Calculate bounds
    VectorSet(light->mins, origin[0] - radius, origin[1] - radius, origin[2] - radius);
    VectorSet(light->maxs, origin[0] + radius, origin[1] + radius, origin[2] + radius);
    
    // Add to active list
    tr_lightSystem.activeLights[tr_lightSystem.numActiveLights++] = light;
}

// Update light properties
void R_UpdateLight(renderLight_t *light) {
    // Recalculate bounds
    switch (light->type) {
    case RL_OMNI:
        VectorSet(light->mins, 
                 light->origin[0] - light->radius,
                 light->origin[1] - light->radius,
                 light->origin[2] - light->radius);
        VectorSet(light->maxs,
                 light->origin[0] + light->radius,
                 light->origin[1] + light->radius,
                 light->origin[2] + light->radius);
        break;
        
    case RL_PROJ:
        // Calculate frustum bounds
        R_CalculateFrustumBounds(light);
        break;
        
    case RL_DIRECTIONAL:
        // Infinite bounds
        VectorSet(light->mins, -99999, -99999, -99999);
        VectorSet(light->maxs, 99999, 99999, 99999);
        break;
    }
    
    light->needsUpdate = qtrue;
}
```

### 5. Interaction Generation

```c
// File: src/engine/renderer/lighting/tr_interaction.c (NEW FILE)

// Allocate interaction from pool
interaction_t* R_AllocInteraction(void) {
    interactionManager_t *mgr = &tr_lightSystem.interactionManager;
    
    if (!mgr->freeList) {
        if (mgr->numInteractions >= mgr->maxInteractions) {
            ri.Printf(PRINT_WARNING, "R_AllocInteraction: MAX_INTERACTIONS hit\n");
            return NULL;
        }
        return &mgr->interactions[mgr->numInteractions++];
    }
    
    interaction_t *inter = mgr->freeList;
    mgr->freeList = inter->nextFree;
    return inter;
}

// Free interaction back to pool
void R_FreeInteraction(interaction_t *inter) {
    // Remove from light list
    if (inter->lightPrev) {
        inter->lightPrev->lightNext = inter->lightNext;
    } else {
        inter->light->firstInteraction = inter->lightNext;
    }
    if (inter->lightNext) {
        inter->lightNext->lightPrev = inter->lightPrev;
    } else {
        inter->light->lastInteraction = inter->lightPrev;
    }
    
    // Remove from surface list
    if (inter->surfacePrev) {
        inter->surfacePrev->surfaceNext = inter->surfaceNext;
    } else {
        inter->surface->firstInteraction = inter->surfaceNext;
    }
    if (inter->surfaceNext) {
        inter->surfaceNext->surfacePrev = inter->surfacePrev;
    }
    
    // Add to free list
    interactionManager_t *mgr = &tr_lightSystem.interactionManager;
    inter->nextFree = mgr->freeList;
    mgr->freeList = inter;
}

// Generate interactions for a light
void R_GenerateLightInteractions(renderLight_t *light, viewParms_t *view) {
    double startTime = ri.Milliseconds();
    
    // Clear existing interactions if dynamic
    if (!light->isStatic || light->needsUpdate) {
        R_ClearLightInteractions(light);
    }
    
    // Find surfaces in light volume
    for (int i = 0; i < view->numDrawSurfs; i++) {
        drawSurf_t *surf = view->drawSurfs[i];
        
        // Quick sphere test
        if (!R_SurfaceInLightVolume(surf, light)) {
            continue;
        }
        
        // Check if interaction already exists (for static lights)
        if (light->isStatic && !light->needsUpdate) {
            if (R_FindInteraction(light, surf)) {
                continue;
            }
        }
        
        // Create new interaction
        interaction_t *inter = R_AllocInteraction();
        if (!inter) break;
        
        inter->light = light;
        inter->surface = surf;
        
        // Add to light's list
        inter->lightPrev = light->lastInteraction;
        inter->lightNext = NULL;
        if (light->lastInteraction) {
            light->lastInteraction->lightNext = inter;
        } else {
            light->firstInteraction = inter;
        }
        light->lastInteraction = inter;
        
        // Add to surface's list
        inter->surfacePrev = NULL;
        inter->surfaceNext = surf->firstInteraction;
        if (surf->firstInteraction) {
            surf->firstInteraction->surfacePrev = inter;
        }
        surf->firstInteraction = inter;
        
        // Process interaction
        R_ProcessInteraction(inter);
        
        light->numInteractions++;
    }
    
    light->needsUpdate = qfalse;
    light->lastUpdateFrame = tr_lightSystem.frameNumber;
    
    tr_lightSystem.interactionTime += ri.Milliseconds() - startTime;
}

// Process light-surface interaction
void R_ProcessInteraction(interaction_t *inter) {
    renderLight_t *light = inter->light;
    drawSurf_t *surf = inter->surface;
    
    // Calculate interaction bounds
    for (int i = 0; i < 3; i++) {
        inter->bounds[0][i] = max(surf->bounds[0][i], light->mins[i]);
        inter->bounds[1][i] = min(surf->bounds[1][i], light->maxs[i]);
    }
    
    // Check if surface can cast shadows
    material_t *mat = (material_t*)surf->material;
    if (!(mat->materialFlags & MATERIAL_NOSHADOWS) &&
        !(light->flags & LIGHTFLAG_NOSHADOWS)) {
        inter->castsShadow = qtrue;
        light->numShadowCasters++;
    }
    
    // Check if surface receives light
    if (!(mat->materialFlags & MATERIAL_NOLIGHT)) {
        inter->receivesLight = qtrue;
        light->numLitSurfaces++;
    }
    
    // Mark as static if both light and surface are static
    if (light->isStatic && surf->isStatic) {
        inter->isStatic = qtrue;
        tr_lightSystem.interactionManager.numStaticCached++;
    }
}
```

### 6. Light Culling

```c
// File: src/engine/renderer/lighting/tr_light_cull.c (NEW FILE)

// Cull lights against view
void R_CullLights(viewParms_t *view) {
    tr_lightSystem.numVisibleLights = 0;
    
    for (int i = 0; i < tr_lightSystem.numActiveLights; i++) {
        renderLight_t *light = tr_lightSystem.activeLights[i];
        
        // Distance cull
        float dist = Distance(light->origin, view->or.origin);
        if (dist > light->cutoffDistance + r_lightCullDistance->value) {
            continue;
        }
        
        // Frustum cull
        if (R_CullBox(light->mins, light->maxs)) {
            continue;
        }
        
        // PVS cull
        if (!R_LightInPVS(light, view)) {
            continue;
        }
        
        // Add to visible list
        tr_lightSystem.visibleLights[tr_lightSystem.numVisibleLights++] = light;
        light->viewCount = tr.viewCount;
    }
}

// Check if light affects PVS
qboolean R_LightInPVS(renderLight_t *light, viewParms_t *view) {
    // Get light's area
    int lightArea = R_PointInArea(light->origin);
    if (lightArea < 0) {
        return qtrue; // Outside world, always visible
    }
    
    // Check if any visible area can see the light
    for (int i = 0; i < view->numVisibleAreas; i++) {
        if (tr.world->areaVisibility[lightArea][view->visibleAreas[i]]) {
            return qtrue;
        }
    }
    
    return qfalse;
}

// Cull interactions
void R_CullInteractions(renderLight_t *light, viewParms_t *view) {
    interaction_t *inter = light->firstInteraction;
    
    while (inter) {
        // Frustum cull interaction bounds
        if (R_CullBox(inter->bounds[0], inter->bounds[1])) {
            inter->culled = qtrue;
            tr_lightSystem.interactionManager.numCulled++;
        } else {
            inter->culled = qfalse;
        }
        
        inter = inter->lightNext;
    }
}
```

### 7. Integration with Existing Systems

```c
// File: src/engine/renderer/tr_main.c (MODIFY)

// Modified render scene to include light processing
void R_RenderScene(viewParms_t *view) {
    // ... existing setup ...
    
    // Process lights
    R_CullLights(view);
    
    // Generate interactions
    for (int i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        renderLight_t *light = tr_lightSystem.visibleLights[i];
        R_GenerateLightInteractions(light, view);
        R_CullInteractions(light, view);
    }
    
    // Add light interactions to draw list
    R_AddLightInteractionsToDrawList(view);
    
    // ... rest of rendering ...
}

// Convert old dlight system
void R_ConvertDlights(void) {
    // Clear active lights
    tr_lightSystem.numActiveLights = 0;
    
    // Convert dlights to renderLights
    for (int i = 0; i < tr.refdef.num_dlights; i++) {
        dlight_t *dl = &tr.refdef.dlights[i];
        
        renderLight_t *light = R_CreateLight();
        VectorCopy(dl->origin, light->origin);
        VectorCopy(dl->color, light->color);
        light->radius = dl->radius;
        light->cutoffDistance = dl->radius;
        light->type = RL_OMNI;
        light->isStatic = qfalse;
        
        R_UpdateLight(light);
        
        tr_lightSystem.activeLights[tr_lightSystem.numActiveLights++] = light;
    }
}
```

## Memory Management

### Memory Pools

```c
// Static allocation
#define LIGHT_MEMORY_BLOCK  (sizeof(renderLight_t) * MAX_RENDER_LIGHTS)
#define INTERACTION_MEMORY_BLOCK (sizeof(interaction_t) * MAX_INTERACTIONS)

// Total memory usage:
// Lights: 256 * ~512 bytes = 128KB
// Interactions: 4096 * ~128 bytes = 512KB
// Total: ~640KB static allocation
```

## Performance Optimizations

### Spatial Acceleration

```c
// Light grid for spatial queries
typedef struct lightGrid_s {
    vec3_t              mins;
    vec3_t              maxs;
    vec3_t              cellSize;
    int                 gridSize[3];
    renderLight_t       ***cells;  // 3D grid of light lists
} lightGrid_t;

void R_BuildLightGrid(void) {
    // Build spatial grid for fast light lookups
    // Used to quickly find lights affecting a surface
}
```

## Testing and Validation

### Debug Visualization

```c
void R_DrawLightVolumes(void) {
    if (!r_showLightVolumes->integer)
        return;
    
    for (int i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        renderLight_t *light = tr_lightSystem.visibleLights[i];
        
        vec4_t color;
        VectorCopy(light->color, color);
        color[3] = 0.25f;
        
        switch (light->type) {
        case RL_OMNI:
            R_DebugSphere(light->origin, light->radius, color);
            break;
        case RL_PROJ:
            R_DebugFrustum(light, color);
            break;
        }
    }
}

void R_DrawInteractions(void) {
    if (!r_showInteractions->integer)
        return;
    
    for (int i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        renderLight_t *light = tr_lightSystem.visibleLights[i];
        interaction_t *inter = light->firstInteraction;
        
        while (inter) {
            if (!inter->culled) {
                R_DebugBounds(inter->bounds[0], inter->bounds[1], colorYellow);
            }
            inter = inter->lightNext;
        }
    }
}
```

## Success Criteria

Phase 5 is complete when:

1. ✓ renderLight_t structure implemented
2. ✓ interaction_t structure implemented
3. ✓ Light management system operational
4. ✓ Interaction generation working
5. ✓ Light culling functional
6. ✓ Old dlight system compatible
7. ✓ Memory usage within bounds
8. ✓ Debug visualization tools working

## Dependencies for Next Phases

### Phase 6 (Additive Lighting) Requirements
- Interaction list for rendering
- Light properties for shading

### Phase 7 (Light Scissoring) Requirements
- Interaction bounds for scissor calculation
- Per-interaction culling

### Phase 8 (Shadow Volumes) Requirements
- Shadow caster identification
- Light position for shadow generation

This lighting data structure system provides the foundation for all advanced lighting features in subsequent phases.