# Phases 6-10: Advanced Rendering Features Summary

## Phase 6: Additive Lighting Passes

### Overview
Implements GPU-side per-pixel dynamic lighting using the interaction data from Phase 5.

### Key Implementation Points

#### Shader Implementation
```glsl
// Fragment shader for per-pixel lighting
vec3 CalculateLighting(vec3 position, vec3 normal, vec3 viewDir, 
                       vec3 lightPos, vec3 lightColor, float radius) {
    vec3 lightDir = normalize(lightPos - position);
    float distance = length(lightPos - position);
    
    // Attenuation
    float attenuation = 1.0 / (1.0 + 0.09 * distance + 0.032 * distance * distance);
    attenuation *= max(0.0, 1.0 - (distance / radius));
    
    // Diffuse
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = lightColor * NdotL * attenuation;
    
    // Specular (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), 32.0);
    vec3 specular = lightColor * spec * attenuation;
    
    return diffuse + specular;
}
```

#### Backend Rendering Loop
```c
void RB_RenderLightingPasses(void) {
    // Base pass (ambient + lightmaps)
    RB_RenderBasePass();
    
    // Enable additive blending
    GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHTEST_ENABLE);
    
    // Render each light
    for (int i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        renderLight_t *light = tr_lightSystem.visibleLights[i];
        interaction_t *inter = light->firstInteraction;
        
        while (inter) {
            if (!inter->culled && inter->receivesLight) {
                RB_SetupLightingShader(light);
                RB_DrawInteraction(inter);
            }
            inter = inter->lightNext;
        }
    }
}
```

### Critical Data Structures
- Light uniforms buffer for GPU
- Per-interaction vertex buffers
- Normal map support integration

### Success Metrics
- 10+ dynamic lights rendered simultaneously
- < 2ms per light on mid-range hardware
- Proper normal mapping support

---

## Phase 7: Light Scissoring Optimization

### Overview
Implements screen-space scissor rectangles to limit pixel shader execution to affected screen regions.

### Key Implementation Points

#### Scissor Calculation
```c
void R_CalcLightScissorRectangle(renderLight_t *light, viewParms_t *view, 
                                 int *scissor) {
    // Project light bounds to screen space
    vec3_t corners[8];
    R_GetBoundsCorners(light->mins, light->maxs, corners);
    
    float minX = view->viewportWidth, minY = view->viewportHeight;
    float maxX = 0, maxY = 0;
    
    for (int i = 0; i < 8; i++) {
        vec3_t projected;
        if (R_ProjectPoint(corners[i], view, projected)) {
            minX = min(minX, projected[0]);
            minY = min(minY, projected[1]);
            maxX = max(maxX, projected[0]);
            maxY = max(maxY, projected[1]);
        }
    }
    
    // Convert to pixel coordinates
    scissor[0] = (int)minX;
    scissor[1] = (int)minY;
    scissor[2] = (int)(maxX - minX);
    scissor[3] = (int)(maxY - minY);
}
```

#### Depth Bounds Test
```c
void R_SetDepthBoundsTest(interaction_t *inter) {
    // Calculate min/max depth of interaction
    float minDepth = 1.0f, maxDepth = 0.0f;
    
    // Project interaction bounds to get depth range
    R_GetInteractionDepthBounds(inter, &minDepth, &maxDepth);
    
    // Set hardware depth bounds test
    if (glConfig.depthBoundsTestAvailable) {
        glDepthBoundsEXT(minDepth, maxDepth);
        glEnable(GL_DEPTH_BOUNDS_TEST_EXT);
    }
}
```

### Performance Impact
- 30-50% reduction in fragment shader invocations
- Significant improvement with many small lights
- Minimal CPU overhead (< 0.1ms per light)

---

## Phase 8: Shadow Volume Generation (CPU Frontend)

### Overview
Implements CPU-side shadow volume generation using silhouette detection.

### Key Implementation Points

#### Silhouette Detection
```c
typedef struct silEdge_s {
    int v1, v2;           // Vertex indices
    int f1, f2;           // Adjacent face indices
} silEdge_t;

void R_FindSilhouetteEdges(srfTriangles_t *tri, vec3_t lightPos, 
                          silEdge_t *edges, int *numEdges) {
    byte *facing = alloca(tri->numTriangles);
    
    // Determine which triangles face the light
    for (int i = 0; i < tri->numTriangles; i++) {
        vec3_t normal;
        R_GetTriangleNormal(tri, i, normal);
        vec3_t toLight;
        VectorSubtract(lightPos, tri->verts[tri->indexes[i*3]].xyz, toLight);
        facing[i] = (DotProduct(normal, toLight) > 0);
    }
    
    // Find silhouette edges
    *numEdges = 0;
    for (int i = 0; i < tri->numEdges; i++) {
        edge_t *edge = &tri->edges[i];
        if (facing[edge->f1] != facing[edge->f2]) {
            edges[*numEdges].v1 = edge->v1;
            edges[*numEdges].v2 = edge->v2;
            (*numEdges)++;
        }
    }
}
```

#### Shadow Volume Construction
```c
void R_CreateShadowVolume(renderLight_t *light, srfTriangles_t *tri,
                         shadowVertex_t **verts, int *numVerts) {
    silEdge_t edges[MAX_EDGES];
    int numEdges;
    
    R_FindSilhouetteEdges(tri, light->origin, edges, &numEdges);
    
    // Allocate shadow vertices (4 per edge for quad)
    *numVerts = numEdges * 4;
    *verts = R_FrameAlloc(sizeof(shadowVertex_t) * (*numVerts));
    
    // Extrude edges away from light
    for (int i = 0; i < numEdges; i++) {
        vec3_t v1, v2, v3, v4;
        
        VectorCopy(tri->verts[edges[i].v1].xyz, v1);
        VectorCopy(tri->verts[edges[i].v2].xyz, v2);
        
        // Project to infinity
        VectorSubtract(v1, light->origin, v3);
        VectorMA(v1, 1000.0f, v3, v3);
        VectorSubtract(v2, light->origin, v4);
        VectorMA(v2, 1000.0f, v4, v4);
        
        // Create quad
        (*verts)[i*4+0].xyz = v1;
        (*verts)[i*4+1].xyz = v2;
        (*verts)[i*4+2].xyz = v4;
        (*verts)[i*4+3].xyz = v3;
    }
}
```

### Optimizations
- Edge connectivity preprocessing
- Static shadow volume caching
- SIMD silhouette detection

---

## Phase 9: Stencil Shadow Rendering (GPU Backend)

### Overview
Uses GPU stencil buffer to render pixel-accurate hard shadows.

### Key Implementation Points

#### Stencil Shadow Algorithm
```c
void RB_StencilShadowPass(renderLight_t *light) {
    // 1. Clear stencil buffer
    glClear(GL_STENCIL_BUFFER_BIT);
    
    // 2. Z-prepass (fill depth buffer)
    RB_DepthPrepass();
    
    // 3. Render shadow volumes
    glEnable(GL_STENCIL_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    
    // Z-fail algorithm (Carmack's reverse)
    interaction_t *inter = light->firstInteraction;
    while (inter) {
        if (inter->castsShadow) {
            // Back faces, increment on z-fail
            glCullFace(GL_FRONT);
            glStencilOp(GL_KEEP, GL_INCR_WRAP, GL_KEEP);
            RB_DrawShadowVolume(inter->shadowTris);
            
            // Front faces, decrement on z-fail
            glCullFace(GL_BACK);
            glStencilOp(GL_KEEP, GL_DECR_WRAP, GL_KEEP);
            RB_DrawShadowVolume(inter->shadowTris);
        }
        inter = inter->lightNext;
    }
    
    // 4. Render lit surfaces where stencil == 0
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    
    RB_RenderLitSurfaces(light);
}
```

#### Shadow Volume Caps
```c
void RB_AddShadowCaps(shadowVolume_t *volume, renderLight_t *light) {
    // Near cap (light-facing triangles)
    for (int i = 0; i < volume->numFrontTris; i++) {
        RB_AddTriangle(volume->frontTris[i]);
    }
    
    // Far cap (extruded back-facing triangles)
    for (int i = 0; i < volume->numBackTris; i++) {
        triangle_t tri = volume->backTris[i];
        // Extrude vertices to infinity
        for (int j = 0; j < 3; j++) {
            vec3_t dir;
            VectorSubtract(tri.verts[j], light->origin, dir);
            VectorMA(tri.verts[j], 1000.0f, dir, tri.verts[j]);
        }
        RB_AddTriangle(tri);
    }
}
```

### Performance Considerations
- Fill rate intensive
- Benefits from hierarchical-Z
- Consider shadow maps for distant shadows

---

## Phase 10: Performance, Memory, and Final Polish

### Overview
Optimizes the complete renderer with parallel processing, SIMD, and efficient memory management.

### Key Implementation Points

#### Frame Memory System
```c
typedef struct frameData_s {
    byte    *base;
    size_t  size;
    size_t  used;
    
    // Sub-allocators
    struct {
        void    *base;
        size_t  used;
        size_t  size;
    } vertexPool, indexPool, uniformPool, tempPool;
} frameData_t;

// Double-buffered frame data
static frameData_t frameData[2];
static int currentFrame;

void* Frame_Alloc(size_t size) {
    frameData_t *fd = &frameData[currentFrame];
    size = ALIGN(size, 16);  // SIMD alignment
    
    if (fd->used + size > fd->size) {
        ri.Error(ERR_DROP, "Frame_Alloc: overflow");
    }
    
    void *ptr = fd->base + fd->used;
    fd->used += size;
    return ptr;
}

void Frame_Clear(void) {
    currentFrame = !currentFrame;
    frameData[currentFrame].used = 0;
}
```

#### Parallel Shadow Generation
```c
typedef struct shadowJob_s {
    renderLight_t   *light;
    srfTriangles_t  *triangles;
    shadowVolume_t  *result;
    volatile int    done;
} shadowJob_t;

void R_ParallelShadowGeneration(void) {
    int numJobs = ri.GetNumProcessors();
    shadowJob_t *jobs = Frame_Alloc(sizeof(shadowJob_t) * numJobs);
    
    // Distribute work
    int lightsPerJob = tr_lightSystem.numVisibleLights / numJobs;
    
    for (int i = 0; i < numJobs; i++) {
        jobs[i].light = tr_lightSystem.visibleLights[i * lightsPerJob];
        ri.AddParallelJob(R_GenerateShadowJob, &jobs[i]);
    }
    
    // Wait for completion
    ri.WaitForAllJobs();
}
```

#### SIMD Optimizations
```c
// SSE2 triangle facing calculation
void R_CalcTriangleFacing_SSE2(const vec3_t *verts, const int *indices,
                               const vec3_t lightPos, byte *facing, int numTris) {
    __m128 lightX = _mm_set1_ps(lightPos[0]);
    __m128 lightY = _mm_set1_ps(lightPos[1]);
    __m128 lightZ = _mm_set1_ps(lightPos[2]);
    
    for (int i = 0; i < numTris; i += 4) {
        // Process 4 triangles at once
        __m128 nx = _mm_load_ps(&normals[i*3]);
        __m128 ny = _mm_load_ps(&normals[i*3+1]);
        __m128 nz = _mm_load_ps(&normals[i*3+2]);
        
        // Calculate dot products
        __m128 dots = _mm_add_ps(
            _mm_mul_ps(nx, lightX),
            _mm_add_ps(
                _mm_mul_ps(ny, lightY),
                _mm_mul_ps(nz, lightZ)
            )
        );
        
        // Store facing results
        int mask = _mm_movemask_ps(_mm_cmpgt_ps(dots, _mm_setzero_ps()));
        facing[i+0] = (mask & 1) ? 1 : 0;
        facing[i+1] = (mask & 2) ? 1 : 0;
        facing[i+2] = (mask & 4) ? 1 : 0;
        facing[i+3] = (mask & 8) ? 1 : 0;
    }
}
```

### Performance Targets
- 60 FPS with 100+ dynamic lights
- < 32MB additional memory usage
- 2x improvement over original renderer
- Full backward compatibility

### Final Optimizations
1. **Culling Hierarchy**: Octree/BVH for spatial queries
2. **Batch Optimization**: Minimize state changes
3. **Cache Optimization**: Data structure alignment
4. **GPU Queries**: Occlusion culling
5. **LOD System**: Distance-based quality

## Overall Success Criteria

The complete 10-phase implementation is successful when:

1. ✓ All phases integrated and functional
2. ✓ No visual regressions from original
3. ✓ Performance targets met
4. ✓ Memory usage within bounds
5. ✓ Full mod compatibility maintained
6. ✓ Debug/profiling tools operational
7. ✓ Documentation complete
8. ✓ Extensive testing passed

## Implementation Priority

### High Priority (Core Functionality)
- Phases 1-3: Foundation
- Phase 5: Lighting structures
- Phase 6: Basic per-pixel lighting

### Medium Priority (Visual Enhancement)
- Phase 4: Vulkan optimization
- Phase 7: Scissoring
- Phases 8-9: Shadows

### Low Priority (Polish)
- Phase 10: Final optimizations

This phased approach ensures a functional renderer at each stage while progressively adding advanced features.