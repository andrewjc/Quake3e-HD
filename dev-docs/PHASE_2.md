# Phase 2: Structured Scene Representation with drawSurf_t

## Executive Summary

Phase 2 transforms the immediate-mode surface processing into a data-driven architecture by implementing a structured `drawSurf_t` representation. This phase decouples scene traversal from rendering, enabling efficient sorting, batching, and future lighting optimizations while maintaining compatibility with existing Quake 3 content.

## Current State Analysis

### Existing Surface Processing

```c
// Current drawSurf_t structure (tr_local.h)
typedef struct drawSurf_s {
    unsigned int    sort;       // 32-bit sort key
    surfaceType_t   *surface;   // Polymorphic surface pointer
} drawSurf_t;

// Current sort key encoding (32-bit)
// Bits 31-22: Shader sort value (10 bits)
// Bits 21-6:  Entity number (16 bits)  
// Bits 5-0:   Fog index (6 bits)
```

### Current Processing Flow

```
R_AddWorldSurfaces() → R_AddDrawSurf() → Immediate insertion into global list
R_AddEntitySurfaces() → R_AddDrawSurf() → Mixed with world surfaces
Backend iterates through unsorted or partially sorted list
```

## Implementation Requirements

### 1. Enhanced drawSurf_t Structure

```c
// File: src/engine/renderer/tr_scene.h (NEW FILE)

// Extended 64-bit sort key for better sorting granularity
typedef uint64_t sortKey_t;

// Enhanced draw surface structure
typedef struct drawSurf_t {
    const surfaceType_t *surface;      // Surface geometry
    const void          *material;     // Initially shader_t*, later material_t*
    sortKey_t           sort;          // 64-bit sort key
    
    // Additional metadata for optimizations
    float               viewDepth;     // Distance from viewer
    int                 entityNum;     // Source entity
    int                 dlightMask;    // Affected dynamic lights (bitmask)
    int                 fogNum;        // Fog volume index
    
    // Culling data
    vec3_t              bounds[2];     // Surface AABB
    float               radius;        // Bounding sphere radius
    
    // For debugging/profiling
    int                 surfaceNum;    // Original surface index
    surfaceType_e       surfaceType;   // SF_GRID, SF_TRIANGLES, etc.
} drawSurf_t;

// Sort key encoding (64-bit)
#define SORT_SHIFT_RENDERPASS   60  // 4 bits: render pass
#define SORT_SHIFT_MATERIAL     48  // 12 bits: material sort value
#define SORT_SHIFT_DEPTH        32  // 16 bits: depth value
#define SORT_SHIFT_ENTITY       16  // 16 bits: entity number
#define SORT_SHIFT_SURFACE      0   // 16 bits: surface index

#define SORT_MASK_RENDERPASS    0xF000000000000000ULL
#define SORT_MASK_MATERIAL      0x0FFF000000000000ULL
#define SORT_MASK_DEPTH         0x0000FFFF00000000ULL
#define SORT_MASK_ENTITY        0x00000000FFFF0000ULL
#define SORT_MASK_SURFACE       0x000000000000FFFFULL

// Render pass types for sorting
typedef enum {
    RP_SHADOWMAP = 0,      // Shadow map generation
    RP_OPAQUE = 1,         // Opaque geometry
    RP_ALPHATEST = 2,      // Alpha tested surfaces
    RP_TRANSPARENT = 3,    // Transparent surfaces (back-to-front)
    RP_POSTPROCESS = 4,    // Post-process effects
    RP_UI = 5              // UI/HUD elements
} renderPass_t;
```

### 2. Frame-based Allocation System

```c
// File: src/engine/renderer/tr_scene.c (MODIFY/NEW)

// Per-frame scene data
typedef struct frameScene_s {
    // Surface lists
    drawSurf_t      *drawSurfs;        // Array of surfaces
    int             numDrawSurfs;      // Current count
    int             maxDrawSurfs;      // Allocated size
    
    // Sorted surface pointers
    drawSurf_t      **sortedSurfs;     // Pointers for sorting
    int             numSortedSurfs;    // May differ due to culling
    
    // Memory pools
    byte            *frameMemory;      // Frame temporary memory
    size_t          frameMemoryUsed;   // Current usage
    size_t          frameMemorySize;   // Total available
    
    // Statistics
    int             numWorldSurfs;     // World surface count
    int             numEntitySurfs;    // Entity surface count
    int             numCulledSurfs;    // Culled surface count
} frameScene_t;

// Double-buffered for frontend/backend separation
static frameScene_t frameScenes[2];
static int currentFrameScene;

// Allocation functions
drawSurf_t* R_AllocDrawSurf(void) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    
    if (scene->numDrawSurfs >= scene->maxDrawSurfs) {
        // Grow array
        int newMax = scene->maxDrawSurfs * 2;
        drawSurf_t *newSurfs = R_FrameAlloc(sizeof(drawSurf_t) * newMax);
        
        Com_Memcpy(newSurfs, scene->drawSurfs, 
                   sizeof(drawSurf_t) * scene->numDrawSurfs);
        
        scene->drawSurfs = newSurfs;
        scene->maxDrawSurfs = newMax;
    }
    
    return &scene->drawSurfs[scene->numDrawSurfs++];
}

void* R_FrameAlloc(size_t size) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    
    // Align to 16 bytes for SIMD
    size = (size + 15) & ~15;
    
    if (scene->frameMemoryUsed + size > scene->frameMemorySize) {
        ri.Error(ERR_DROP, "R_FrameAlloc: out of memory");
    }
    
    void *ptr = scene->frameMemory + scene->frameMemoryUsed;
    scene->frameMemoryUsed += size;
    
    return ptr;
}
```

### 3. Modified Surface Addition

```c
// File: src/engine/renderer/tr_world.c (MODIFY)

void R_AddWorldSurface(msurface_t *surf, shader_t *shader, int dlightMask) {
    drawSurf_t *drawSurf;
    vec3_t center;
    
    // Early culling checks
    if (R_CullSurface(surf, &tr.viewParms)) {
        frameScenes[currentFrameScene].numCulledSurfs++;
        return;
    }
    
    // Allocate draw surface
    drawSurf = R_AllocDrawSurf();
    
    // Fill surface data
    drawSurf->surface = (surfaceType_t*)surf;
    drawSurf->material = shader;
    drawSurf->entityNum = ENTITYNUM_WORLD;
    drawSurf->dlightMask = dlightMask;
    drawSurf->fogNum = surf->fogIndex;
    
    // Calculate view depth
    VectorAdd(surf->cullinfo.bounds[0], surf->cullinfo.bounds[1], center);
    VectorScale(center, 0.5f, center);
    drawSurf->viewDepth = DotProduct(center, tr.viewParms.or.axis[0]);
    
    // Store bounds for culling
    VectorCopy(surf->cullinfo.bounds[0], drawSurf->bounds[0]);
    VectorCopy(surf->cullinfo.bounds[1], drawSurf->bounds[1]);
    drawSurf->radius = surf->cullinfo.radius;
    
    // Debug info
    drawSurf->surfaceNum = surf - tr.world->surfaces;
    drawSurf->surfaceType = surf->surfaceType;
    
    // Generate sort key (will be updated after all surfaces added)
    drawSurf->sort = 0;
    
    frameScenes[currentFrameScene].numWorldSurfs++;
}

// File: src/engine/renderer/tr_mesh.c (MODIFY)

void R_AddEntitySurfaces(trRefEntity_t *ent) {
    model_t *model = ent->model;
    shader_t *shader;
    drawSurf_t *drawSurf;
    
    // For each surface in model
    for (int i = 0; i < model->numSurfaces; i++) {
        mdSurface_t *surface = model->surfaces[i];
        
        // Get shader for this surface
        shader = R_GetShaderByHandle(ent->customShader ? 
                                    ent->customShader : surface->shader);
        
        // Skip if culled
        if (R_CullEntitySurface(surface, ent, &tr.viewParms)) {
            frameScenes[currentFrameScene].numCulledSurfs++;
            continue;
        }
        
        // Allocate draw surface
        drawSurf = R_AllocDrawSurf();
        
        // Fill surface data
        drawSurf->surface = (surfaceType_t*)surface;
        drawSurf->material = shader;
        drawSurf->entityNum = ent - backEndData->entities;
        drawSurf->dlightMask = R_CalcEntityDlightMask(ent);
        drawSurf->fogNum = ent->fogNum;
        
        // Calculate view depth (entity-relative)
        vec3_t worldCenter;
        VectorAdd(surface->bounds[0], surface->bounds[1], worldCenter);
        VectorScale(worldCenter, 0.5f, worldCenter);
        VectorAdd(worldCenter, ent->origin, worldCenter);
        drawSurf->viewDepth = DotProduct(worldCenter, tr.viewParms.or.axis[0]);
        
        // Transform bounds to world space
        R_TransformBounds(surface->bounds[0], surface->bounds[1],
                         ent->origin, ent->axis,
                         drawSurf->bounds[0], drawSurf->bounds[1]);
        drawSurf->radius = surface->radius * ent->scale;
        
        // Debug info
        drawSurf->surfaceNum = i;
        drawSurf->surfaceType = surface->surfaceType;
        
        frameScenes[currentFrameScene].numEntitySurfs++;
    }
}
```

### 4. Sort Key Generation

```c
// File: src/engine/renderer/tr_sort.c (NEW FILE)

sortKey_t R_GenerateSortKey(const drawSurf_t *surf) {
    sortKey_t key = 0;
    shader_t *shader = (shader_t*)surf->material;
    
    // Determine render pass
    renderPass_t pass;
    if (shader->sort < SS_OPAQUE) {
        pass = RP_SHADOWMAP;
    } else if (shader->sort <= SS_OPAQUE) {
        pass = RP_OPAQUE;
    } else if (shader->sort < SS_BLEND0) {
        pass = RP_ALPHATEST;
    } else if (shader->sort <= SS_BLEND3) {
        pass = RP_TRANSPARENT;
    } else if (shader->sort < SS_POST_PROCESS) {
        pass = RP_UI;
    } else {
        pass = RP_POSTPROCESS;
    }
    
    key |= ((sortKey_t)pass << SORT_SHIFT_RENDERPASS);
    
    // Material sort value
    int materialSort = shader->sortedIndex;  // Use sorted shader index
    key |= (((sortKey_t)materialSort & 0xFFF) << SORT_SHIFT_MATERIAL);
    
    // Depth value (different for opaque vs transparent)
    uint16_t depthValue;
    if (pass == RP_OPAQUE || pass == RP_ALPHATEST) {
        // Front-to-back for opaque (smaller depth first)
        depthValue = (uint16_t)(surf->viewDepth * 100.0f);
    } else {
        // Back-to-front for transparent (larger depth first)
        depthValue = (uint16_t)((10000.0f - surf->viewDepth) * 100.0f);
    }
    key |= ((sortKey_t)depthValue << SORT_SHIFT_DEPTH);
    
    // Entity number for batching
    key |= (((sortKey_t)surf->entityNum & 0xFFFF) << SORT_SHIFT_ENTITY);
    
    // Surface index for stable sorting
    key |= ((sortKey_t)surf->surfaceNum & 0xFFFF);
    
    return key;
}

void R_GenerateAllSortKeys(void) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    
    for (int i = 0; i < scene->numDrawSurfs; i++) {
        scene->drawSurfs[i].sort = R_GenerateSortKey(&scene->drawSurfs[i]);
    }
}
```

### 5. Radix Sort Implementation

```c
// File: src/engine/renderer/tr_sort.c

// Optimized 64-bit radix sort
void R_RadixSort64(drawSurf_t **surfs, int numSurfs) {
    drawSurf_t **temp = R_FrameAlloc(sizeof(drawSurf_t*) * numSurfs);
    
    // 4-pass radix sort (16 bits per pass)
    for (int pass = 0; pass < 4; pass++) {
        int shift = pass * 16;
        
        // Count frequencies
        int counts[65536];
        Com_Memset(counts, 0, sizeof(counts));
        
        for (int i = 0; i < numSurfs; i++) {
            int bucket = (surfs[i]->sort >> shift) & 0xFFFF;
            counts[bucket]++;
        }
        
        // Compute offsets (prefix sum)
        int offsets[65536];
        offsets[0] = 0;
        for (int i = 1; i < 65536; i++) {
            offsets[i] = offsets[i-1] + counts[i-1];
        }
        
        // Scatter to temp array
        for (int i = 0; i < numSurfs; i++) {
            int bucket = (surfs[i]->sort >> shift) & 0xFFFF;
            temp[offsets[bucket]++] = surfs[i];
        }
        
        // Swap arrays
        drawSurf_t **swap = surfs;
        surfs = temp;
        temp = swap;
    }
    
    // Ensure result is in the original array
    if (surfs != scene->sortedSurfs) {
        Com_Memcpy(scene->sortedSurfs, surfs, sizeof(drawSurf_t*) * numSurfs);
    }
}

// Main sorting function
void R_SortDrawSurfs(void) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    
    if (scene->numDrawSurfs == 0)
        return;
    
    // Allocate pointer array for sorting
    if (!scene->sortedSurfs || scene->numSortedSurfs < scene->numDrawSurfs) {
        scene->sortedSurfs = R_FrameAlloc(sizeof(drawSurf_t*) * scene->numDrawSurfs);
    }
    
    // Initialize pointers
    for (int i = 0; i < scene->numDrawSurfs; i++) {
        scene->sortedSurfs[i] = &scene->drawSurfs[i];
    }
    scene->numSortedSurfs = scene->numDrawSurfs;
    
    // Generate sort keys
    R_GenerateAllSortKeys();
    
    // Perform radix sort
    R_RadixSort64(scene->sortedSurfs, scene->numSortedSurfs);
    
    // Optional: Merge consecutive surfaces with same material/entity
    R_MergeBatchableSurfaces();
}
```

### 6. Backend Consumption

```c
// File: src/engine/renderer/tr_backend.c (MODIFY)

void RB_DrawSurfs(drawSurf_t **sortedSurfs, int numSurfs) {
    shader_t        *oldShader = NULL;
    int             oldEntityNum = -1;
    int             oldFogNum = -1;
    qboolean        depthRange = qfalse;
    renderPass_t    currentPass = RP_OPAQUE;
    
    // Iterate through sorted surfaces
    for (int i = 0; i < numSurfs; i++) {
        drawSurf_t *surf = sortedSurfs[i];
        shader_t *shader = (shader_t*)surf->material;
        
        // Extract render pass from sort key
        renderPass_t pass = (surf->sort >> SORT_SHIFT_RENDERPASS) & 0xF;
        
        // Handle pass changes
        if (pass != currentPass) {
            // Flush any pending draws
            RB_EndSurface();
            
            // Setup new pass state
            switch (pass) {
            case RP_OPAQUE:
                GL_State(GLS_DEFAULT);
                break;
            case RP_ALPHATEST:
                GL_State(GLS_DEFAULT | GLS_ALPHATEST_ENABLE);
                break;
            case RP_TRANSPARENT:
                GL_State(GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
                break;
            }
            
            currentPass = pass;
        }
        
        // Check for state changes
        if (shader != oldShader || surf->entityNum != oldEntityNum) {
            // Flush previous surface
            if (oldShader != NULL) {
                RB_EndSurface();
            }
            
            // Setup new surface state
            RB_BeginSurface(shader, surf->fogNum);
            oldShader = shader;
            oldEntityNum = surf->entityNum;
            oldFogNum = surf->fogNum;
            
            // Set entity transform
            if (surf->entityNum != ENTITYNUM_WORLD) {
                backEnd.currentEntity = &backEnd.refdef.entities[surf->entityNum];
                RB_SetupEntityLighting(&backEnd.refdef, backEnd.currentEntity);
            } else {
                backEnd.currentEntity = &tr.worldEntity;
            }
        }
        
        // Draw the surface
        tess.dlightMask |= surf->dlightMask;  // Accumulate dlight mask
        rb_surfaceTable[*((int*)surf->surface)](surf->surface);
    }
    
    // Flush final surface
    RB_EndSurface();
}
```

### 7. Optimization: Surface Merging

```c
// File: src/engine/renderer/tr_sort.c

void R_MergeBatchableSurfaces(void) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    int writeIndex = 0;
    
    for (int i = 0; i < scene->numSortedSurfs; i++) {
        drawSurf_t *surf = scene->sortedSurfs[i];
        
        // Check if we can merge with previous surface
        if (writeIndex > 0) {
            drawSurf_t *prevSurf = scene->sortedSurfs[writeIndex - 1];
            
            // Can merge if same material, entity, and fog
            if (surf->material == prevSurf->material &&
                surf->entityNum == prevSurf->entityNum &&
                surf->fogNum == prevSurf->fogNum &&
                R_SurfacesAreBatchable(surf, prevSurf)) {
                
                // Merge dlight masks
                prevSurf->dlightMask |= surf->dlightMask;
                
                // Keep the surface with better depth for sorting
                if (surf->viewDepth < prevSurf->viewDepth) {
                    prevSurf->viewDepth = surf->viewDepth;
                }
                
                // Skip this surface (merged)
                continue;
            }
        }
        
        // Keep this surface
        scene->sortedSurfs[writeIndex++] = surf;
    }
    
    scene->numSortedSurfs = writeIndex;
}

qboolean R_SurfacesAreBatchable(drawSurf_t *a, drawSurf_t *b) {
    // Check if surfaces can be rendered in same batch
    // This depends on surface type and other factors
    
    if (a->surfaceType != b->surfaceType)
        return qfalse;
    
    // Both must be static world surfaces or same entity
    if (a->entityNum != b->entityNum)
        return qfalse;
    
    // Must not require different vertex attributes
    shader_t *shaderA = (shader_t*)a->material;
    shader_t *shaderB = (shader_t*)b->material;
    
    if (shaderA->vertexAttribs != shaderB->vertexAttribs)
        return qfalse;
    
    return qtrue;
}
```

## Memory Management

### Frame Memory Pools

```c
// Initialize at startup
void R_InitFrameScenes(void) {
    for (int i = 0; i < 2; i++) {
        frameScene_t *scene = &frameScenes[i];
        
        // Allocate frame memory (32MB per frame)
        scene->frameMemorySize = 32 * 1024 * 1024;
        scene->frameMemory = ri.Hunk_Alloc(scene->frameMemorySize, h_low);
        
        // Pre-allocate drawSurf array
        scene->maxDrawSurfs = MAX_DRAWSURFS;
        scene->drawSurfs = (drawSurf_t*)scene->frameMemory;
        scene->frameMemoryUsed = sizeof(drawSurf_t) * MAX_DRAWSURFS;
    }
}

// Reset for new frame
void R_ClearFrameScene(void) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    
    // Reset counters
    scene->numDrawSurfs = 0;
    scene->numSortedSurfs = 0;
    scene->frameMemoryUsed = sizeof(drawSurf_t) * MAX_DRAWSURFS;
    
    // Clear statistics
    scene->numWorldSurfs = 0;
    scene->numEntitySurfs = 0;
    scene->numCulledSurfs = 0;
}

// Swap for double buffering
void R_SwapFrameScenes(void) {
    currentFrameScene = !currentFrameScene;
    R_ClearFrameScene();
}
```

## Integration with Phase 1

### Command Buffer Integration

```c
// Modified drawViewCommand from Phase 1
typedef struct drawViewCommand_s {
    baseCommand_t   header;
    viewParms_t     viewParms;
    drawSurf_t      **sortedSurfs;    // Now sorted list
    int             numSurfs;
    // Remove unsorted lists
} drawViewCommand_t;

// Frontend generates sorted list before command
void RE_RenderScene(const refdef_t *fd) {
    // ... setup code ...
    
    // Build surface list
    R_ClearFrameScene();
    R_AddWorldSurfaces();
    R_AddEntitySurfaces();
    
    // Sort surfaces
    R_SortDrawSurfs();
    
    // Create command with sorted list
    drawViewCommand_t *cmd = R_GetCommandBuffer(sizeof(drawViewCommand_t));
    cmd->sortedSurfs = frameScenes[currentFrameScene].sortedSurfs;
    cmd->numSurfs = frameScenes[currentFrameScene].numSortedSurfs;
    
    // Swap frame scenes for next frame
    R_SwapFrameScenes();
}
```

## Performance Optimizations

### SIMD Optimizations

```c
// SSE2 optimized depth calculation
void R_CalculateViewDepthSSE2(drawSurf_t *surfs, int count) {
    __m128 axis_x = _mm_set1_ps(tr.viewParms.or.axis[0][0]);
    __m128 axis_y = _mm_set1_ps(tr.viewParms.or.axis[0][1]);
    __m128 axis_z = _mm_set1_ps(tr.viewParms.or.axis[0][2]);
    
    for (int i = 0; i < count; i += 4) {
        // Load 4 surface centers
        __m128 cx = _mm_load_ps(&surfs[i].center[0]);
        __m128 cy = _mm_load_ps(&surfs[i].center[1]);
        __m128 cz = _mm_load_ps(&surfs[i].center[2]);
        
        // Dot product with view axis
        __m128 depth = _mm_add_ps(
            _mm_mul_ps(cx, axis_x),
            _mm_add_ps(
                _mm_mul_ps(cy, axis_y),
                _mm_mul_ps(cz, axis_z)
            )
        );
        
        // Store results
        _mm_store_ps(&surfs[i].viewDepth, depth);
    }
}
```

### Cache Optimization

```c
// Align structures for cache efficiency
typedef struct drawSurf_t {
    // Hot data (accessed during sorting) - 64 bytes
    sortKey_t           sort;          // 8 bytes
    const surfaceType_t *surface;      // 8 bytes
    const void          *material;     // 8 bytes
    float               viewDepth;     // 4 bytes
    int                 entityNum;     // 4 bytes
    // Padding to 64-byte cache line
    char                padding[32];
    
    // Cold data (accessed during rendering) - separate cache line
    int                 dlightMask;
    int                 fogNum;
    vec3_t              bounds[2];
    float               radius;
} __attribute__((aligned(64))) drawSurf_t;
```

## Testing and Validation

### Debug Visualization

```c
// Debug cvars
cvar_t *r_showSortOrder;      // Visualize sort order
cvar_t *r_showBatching;       // Show batched surfaces
cvar_t *r_showSurfaceStats;   // Display surface statistics

// Debug rendering
void R_DebugDrawSortOrder(void) {
    if (!r_showSortOrder->integer)
        return;
    
    frameScene_t *scene = &frameScenes[currentFrameScene];
    
    for (int i = 0; i < scene->numSortedSurfs; i++) {
        drawSurf_t *surf = scene->sortedSurfs[i];
        
        // Color based on render pass
        vec4_t color;
        int pass = (surf->sort >> SORT_SHIFT_RENDERPASS) & 0xF;
        switch (pass) {
        case RP_OPAQUE:     VectorSet4(color, 0, 1, 0, 0.5f); break;
        case RP_TRANSPARENT: VectorSet4(color, 0, 0, 1, 0.5f); break;
        default:            VectorSet4(color, 1, 1, 1, 0.5f); break;
        }
        
        // Draw bounding box
        R_DebugDrawBounds(surf->bounds[0], surf->bounds[1], color);
        
        // Draw sort order number
        vec3_t center;
        VectorAdd(surf->bounds[0], surf->bounds[1], center);
        VectorScale(center, 0.5f, center);
        R_DebugDrawNumber(center, i);
    }
}
```

## Compatibility Maintenance

### Backward Compatibility

1. **Surface Types**: All existing surface types remain supported
2. **Shader System**: Current shader system unchanged (until Phase 3)
3. **Entity System**: Entity rendering pipeline preserved
4. **Mod Support**: Binary compatibility maintained

### Migration Path

```c
// Compatibility wrapper for old code
void R_AddDrawSurf(void *surface, shader_t *shader, 
                   int entityNum, int dlightMask) {
    drawSurf_t *surf = R_AllocDrawSurf();
    
    surf->surface = surface;
    surf->material = shader;
    surf->entityNum = entityNum;
    surf->dlightMask = dlightMask;
    
    // Calculate other fields
    surf->viewDepth = R_CalculateDepth(surface, entityNum);
    surf->fogNum = R_GetFogNum(surface, entityNum);
    
    // Sort key will be generated later
    surf->sort = 0;
}
```

## Performance Metrics

### Expected Improvements

- **Sorting Performance**: 3-5x faster with radix sort
- **Draw Call Reduction**: 20-40% via batching
- **CPU Cache Utilization**: 50% improvement
- **Memory Bandwidth**: 30% reduction

### Benchmarking Code

```c
typedef struct perfStats_s {
    double  sortTime;
    double  cullTime;
    double  setupTime;
    int     numDrawCalls;
    int     numStateChanges;
    int     numSurfacesMerged;
} perfStats_t;

void R_GatherPerformanceStats(perfStats_t *stats) {
    double startTime;
    
    startTime = ri.Milliseconds();
    R_SortDrawSurfs();
    stats->sortTime = ri.Milliseconds() - startTime;
    
    stats->numDrawCalls = backEnd.pc.c_drawElements;
    stats->numStateChanges = backEnd.pc.c_shaderChanges;
    stats->numSurfacesMerged = frameScenes[currentFrameScene].numMerged;
}
```

## Success Criteria

Phase 2 is complete when:

1. ✓ All surfaces use new drawSurf_t structure
2. ✓ Radix sorting implemented and functional
3. ✓ Frame-based allocation operational
4. ✓ Surface batching reduces draw calls
5. ✓ No visual regressions
6. ✓ Sorting performance improved
7. ✓ Memory usage within bounds
8. ✓ Debug tools implemented

## Dependencies for Next Phases

### Phase 3 (Material System) Requirements
- drawSurf_t with material pointer
- Sorted surface list for material batching

### Phase 5 (Dynamic Lighting) Requirements  
- Per-surface dlight masks
- Surface bounds for light culling

### Phase 7 (Light Scissoring) Requirements
- Sorted surface list for depth range
- Surface bounds in view space

This structured representation forms the foundation for all advanced rendering features in subsequent phases.