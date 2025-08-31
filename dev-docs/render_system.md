# DOOM 3 BFG Edition Rendering System - Software Design Document

## Executive Summary

The DOOM 3 BFG Edition rendering system represents a sophisticated hybrid forward-rendering pipeline with unified lighting, stencil shadow volumes, and extensive optimization for multi-core processors. This document provides a comprehensive technical guide for understanding and implementing the rendering, lighting, material, and texture systems.

## Table of Contents

1. [System Architecture Overview](#system-architecture-overview)
2. [Core Rendering Pipeline](#core-rendering-pipeline)
3. [Lighting System Design](#lighting-system-design)
4. [Material and Shader System](#material-and-shader-system)
5. [Texture Management System](#texture-management-system)
6. [Shadow Rendering Implementation](#shadow-rendering-implementation)
7. [Memory Management Architecture](#memory-management-architecture)
8. [Optimization Strategies](#optimization-strategies)
9. [Implementation Guidelines](#implementation-guidelines)
10. [Performance Considerations](#performance-considerations)

---

## 1. System Architecture Overview

### 1.1 High-Level Architecture

The DOOM 3 BFG renderer employs a **dual-threaded frontend-backend architecture** with clear separation of concerns:

```
┌─────────────────────────────────────────────────────┐
│                   Game Thread                       │
│  ┌────────────┐  ┌──────────┐  ┌──────────────┐   │
│  │ Game Logic │→ │ Frontend │→ │Command Buffer│   │
│  └────────────┘  └──────────┘  └──────────────┘   │
└─────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────┐
│                  Render Thread                      │
│  ┌──────────────┐  ┌─────────┐  ┌─────────────┐   │
│  │Command Buffer│→ │ Backend │→ │OpenGL Driver│   │
│  └──────────────┘  └─────────┘  └─────────────┘   │
└─────────────────────────────────────────────────────┘
```

### 1.2 Key Components

| Component | Location | Responsibility |
|-----------|----------|----------------|
| **RenderSystem** | `renderer/RenderSystem.h/cpp` | Global renderer management, initialization |
| **RenderWorld** | `renderer/RenderWorld.h/cpp` | Scene graph, portal culling, entity management |
| **Frontend** | `renderer/tr_frontend_*.cpp` | View processing, culling, command generation |
| **Backend** | `renderer/tr_backend_*.cpp` | GPU command execution, state management |
| **Material System** | `renderer/Material.h/cpp` | Material parsing, stage processing |
| **Image Manager** | `renderer/ImageManager.cpp` | Texture loading, compression, caching |
| **Vertex Cache** | `renderer/VertexCache.h/cpp` | Geometry buffer management |
| **Render Programs** | `renderer/RenderProgs*.cpp` | Shader compilation and management |

### 1.3 Data Flow Architecture

```
Entities/Lights → Frontend Processing → Command Buffer → Backend Execution → GPU
                      ↓
                Portal Culling
                Frustum Culling
                Interaction Generation
                Surface Sorting
```

---

## 2. Core Rendering Pipeline

### 2.1 Frame Processing Sequence

```cpp
// Simplified frame processing flow
void idRenderSystemLocal::BeginFrame() {
    // 1. Swap command buffers (double buffering)
    SwapCommandBuffers();
    
    // 2. Start backend rendering of previous frame
    RenderCommands();
    
    // 3. Begin frontend processing for current frame
    R_ToggleSmpFrame();
}

void ProcessRenderView(viewDef_t* viewDef) {
    // Frontend processing stages:
    R_SetupViewMatrix(viewDef);        // Setup projection/view matrices
    R_SetupViewFrustum(viewDef);       // Calculate frustum planes
    R_FlowViewThroughPortals(viewDef); // Portal-based visibility
    R_AddLights(viewDef);              // Process visible lights
    R_AddModels(viewDef);              // Process visible entities
    R_GenerateInteractions(viewDef);   // Light-surface interactions
    R_SortDrawSurfs(viewDef);          // Depth/material sorting
    R_GenerateCommands(viewDef);       // Build command buffer
}
```

### 2.2 Portal-Based Visibility System

The renderer uses a **portal-based visibility system** for efficient culling:

```cpp
struct portal_t {
    int        intoArea;          // Area this portal leads to
    idWinding* winding;           // Portal polygon
    idPlane    plane;             // Portal plane equation
    portal_t*  next;              // Next portal in chain
};

// Portal traversal algorithm
void R_FlowViewThroughPortals(viewDef_t* view) {
    // 1. Start from view's current area
    // 2. Mark area as visible
    // 3. For each portal in area:
    //    - Test portal against view frustum
    //    - If visible, recursively process connected area
    //    - Clip frustum to portal bounds
}
```

### 2.3 Draw Surface Management

```cpp
struct drawSurf_t {
    const srfTriangles_t* frontEndGeo;    // Geometry data
    const idMaterial*     material;        // Material definition
    uint64                sort;            // Sort key for ordering
    const float*          shaderRegisters; // Dynamic parameters
    
    // Sort key encoding (64-bit):
    // Bits 63-55: Material sort order
    // Bits 54-40: Depth value (for opaque)
    // Bits 39-20: Material pointer hash
    // Bits 19-0:  Entity/light index
};
```

### 2.4 Command Buffer Structure

```cpp
enum renderCommand_t {
    RC_NOP,
    RC_DRAW_VIEW_3D,      // Render 3D scene
    RC_DRAW_VIEW_GUI,     // Render 2D GUI
    RC_SET_BUFFER,        // Set render target
    RC_COPY_RENDER,       // Copy render target
    RC_POST_PROCESS,      // Post-processing pass
    RC_SWAP_BUFFERS       // Present frame
};

struct drawSurfsCommand_t {
    renderCommand_t commandId;
    viewDef_t*      viewDef;
    int             numDrawSurfs;
    drawSurf_t**    drawSurfs;
};
```

---

## 3. Lighting System Design

### 3.1 Light Types and Properties

```cpp
struct renderLight_t {
    idMat3        axis;                // Light orientation
    idVec3        origin;              // World position
    
    // Projection parameters
    idRenderMatrix  projectionMatrix;  // Light projection matrix
    idVec3          target;            // Spot light target
    float           lightRadius[3];    // Box light dimensions
    
    // Material properties  
    const idMaterial* shader;          // Light material
    float             shaderParms[8];  // Dynamic parameters
    
    // Shadow properties
    bool           castsShadows;       // Enable shadow casting
    int            shadowLOD;          // Shadow detail level
    
    // Light types (via shader)
    bool           pointLight;         // Omnidirectional
    bool           spotLight;          // Directional cone
    bool           parallelLight;      // Sun/directional
    bool           fogLight;           // Volumetric fog
};
```

### 3.2 Light-Surface Interaction System

The interaction system **pre-calculates light-surface relationships**:

```cpp
class idInteraction {
    // Linked list pointers
    idInteraction* lightNext;        // Next surface for this light
    idInteraction* surfaceNext;      // Next light for this surface
    
    // Cached data
    srfTriangles_t* lightTris;      // Triangles facing light
    srfTriangles_t* shadowTris;     // Shadow volume geometry
    
    // Optimization flags
    bool           staticInteraction; // Can be cached
    bool           frustumCulled;    // Outside light volume
    int            dynamicFrameCount; // Last update frame
};

// Interaction generation process
void R_CreateLightInteractions(idRenderLightLocal* light) {
    // For each entity in light's influence:
    for (entity in light->entityRefs) {
        // For each surface on entity:
        for (surface in entity->surfaces) {
            // Test surface bounds against light volume
            if (R_CullBoxByLight(surface->bounds, light)) {
                continue; // Skip if outside
            }
            
            // Create or update interaction
            interaction = R_GetInteraction(light, surface);
            interaction->Process();
        }
    }
}
```

### 3.3 Light Scissor Rectangle Optimization

```cpp
struct lightScissor_t {
    int x1, y1, x2, y2;  // Screen-space bounds
};

lightScissor_t R_CalcLightScissorRectangle(viewDef_t* view, 
                                           idRenderLightLocal* light) {
    // 1. Transform light bounds to view space
    idBounds viewBounds = light->bounds * view->worldToView;
    
    // 2. Project to screen space
    idBounds projected = R_ProjectBounds(viewBounds, view->projection);
    
    // 3. Convert to pixel coordinates
    lightScissor_t scissor;
    scissor.x1 = projected.min.x * view->viewport.width;
    scissor.y1 = projected.min.y * view->viewport.height;
    scissor.x2 = projected.max.x * view->viewport.width;
    scissor.y2 = projected.max.y * view->viewport.height;
    
    // 4. Clamp to viewport
    scissor.Intersect(view->viewport);
    
    return scissor;
}
```

### 3.4 Unified Lighting Model

```glsl
// Simplified interaction shader
// Vertex shader transforms and projects
// Fragment shader performs lighting calculation:

vec3 InteractionLighting(
    vec3 albedo,           // Diffuse color
    vec3 normal,           // Surface normal
    vec3 specular,         // Specular color
    float gloss,           // Specular power
    vec3 lightVector,      // Light direction
    vec3 viewVector,       // View direction
    vec3 lightColor,       // Light color
    float attenuation      // Distance falloff
) {
    // Diffuse lighting (Lambert)
    float NdotL = max(0.0, dot(normal, lightVector));
    vec3 diffuse = albedo * lightColor * NdotL;
    
    // Specular lighting (Blinn-Phong)
    vec3 halfVector = normalize(lightVector + viewVector);
    float NdotH = max(0.0, dot(normal, halfVector));
    vec3 spec = specular * lightColor * pow(NdotH, gloss);
    
    // Combine with attenuation
    return (diffuse + spec) * attenuation;
}
```

---

## 4. Material and Shader System

### 4.1 Material Definition Structure

```cpp
class idMaterial {
    // Surface properties
    int         materialFlags;       // MATERIAL_* flags
    int         contentFlags;        // CONTENTS_* flags  
    int         surfaceFlags;        // SURF_* flags
    
    // Rendering properties
    float       sort;                // Sort order value
    cullType_t  cullType;            // Front/back/none
    int         coverage;            // Alpha coverage
    
    // Shader stages
    int              numStages;      // Stage count
    shaderStage_t*   stages;         // Stage array
    
    // Expression system
    int              numRegisters;   // Register count
    float*           expressionRegisters; // Dynamic values
    expOp_t*         expressions;    // Expression ops
    
    // Special properties
    decalInfo_t      decalInfo;      // Decal parameters
    float            polygonOffset;  // Z-fighting fix
};
```

### 4.2 Material Stage Processing

```cpp
struct shaderStage_t {
    // Stage type
    stageLighting_t  lighting;       // Lighting mode
    
    // Texture coordinates
    texgen_t         texgen;         // Coordinate generation
    textureMatrix_t  textureMatrix;  // Transform matrix
    
    // Textures
    idImage*         texture;        // Color texture
    idImage*         normalMap;      // Normal map
    idImage*         specularMap;    // Specular map
    
    // Vertex attributes
    stageVertexColor_t vertexColor;  // Vertex color usage
    
    // Render states
    int64            drawStateBits;  // GL state flags
    int              blendSrc;       // Blend source
    int              blendDst;       // Blend destination
};

// Material parsing example
material models/weapons/pistol/pistol_diffuse {
    {
        blend diffuseMap
        map models/weapons/pistol/pistol_d.tga
        alphaTest 0.5
    }
    {
        blend bumpMap
        map models/weapons/pistol/pistol_n.tga
    }
    {
        blend specularMap
        map models/weapons/pistol/pistol_s.tga
    }
}
```

### 4.3 Expression System for Dynamic Materials

```cpp
enum expOpType_t {
    OP_TYPE_ADD,
    OP_TYPE_SUBTRACT,
    OP_TYPE_MULTIPLY,
    OP_TYPE_DIVIDE,
    OP_TYPE_MOD,
    OP_TYPE_TABLE,      // Table lookup
    OP_TYPE_GT,         // Greater than
    OP_TYPE_LT,         // Less than
    OP_TYPE_EQ          // Equal
};

struct expOp_t {
    expOpType_t  opType;        // Operation type
    int          a, b, c;        // Register indices
};

// Expression evaluation
float EvaluateExpression(int index, float time) {
    for (expOp_t& op : expressions) {
        float a = registers[op.a];
        float b = registers[op.b];
        
        switch(op.opType) {
            case OP_TYPE_ADD:
                registers[op.c] = a + b;
                break;
            case OP_TYPE_TABLE:
                registers[op.c] = TableLookup(a, b);
                break;
            // ... other operations
        }
    }
    return registers[index];
}
```

### 4.4 Material Keywords and Effects

| Keyword | Effect | Implementation |
|---------|--------|----------------|
| `translucent` | Alpha blending | Sorted back-to-front |
| `nonsolid` | No collision | Skip physics processing |
| `twosided` | Disable culling | `GL_CULL_FACE` disabled |
| `noShadows` | No shadow casting | Skip shadow volume generation |
| `noSelfShadow` | No self-shadowing | Special shadow state |
| `forceShadows` | Force shadow casting | Override automatic detection |
| `spectrum N` | Light spectrum | Used for prism effects |
| `deform <type>` | Vertex deformation | CPU or GPU vertex modification |
| `sort <value>` | Render order | Controls draw order |

---

## 5. Texture Management System

### 5.1 Image Storage Architecture

```cpp
class idImage {
    // Image properties
    int           width, height;      // Dimensions
    textureType_t type;              // 2D/Cube/3D
    cubeFiles_t   cubeFiles;         // Cubemap faces
    
    // Format information
    textureFormat_t format;          // Internal format
    textureColor_t  colorFormat;     // Color space
    int             numLevels;       // Mipmap levels
    
    // GPU resources
    GLuint          texnum;          // OpenGL texture ID
    
    // Compression
    bool            isCompressed;    // DXT compressed
    int             compressedSize;  // Compressed size
    
    // Memory management
    int             frameUsed;       // Last frame used
    int             bindCount;       // Bind counter
    byte*           cpuData;         // CPU-side copy
    
    // Loading state
    textureUsage_t  usage;          // Usage hint
    imageResidency_t residency;     // Memory residency
};
```

### 5.2 Texture Compression Pipeline

```cpp
// YCoCg-DXT5 compression for color textures
void CompressYCoCgDXT5(const byte* src, byte* dst, 
                       int width, int height) {
    // 1. Convert RGB to YCoCg color space
    for (each pixel) {
        float r = src[0] / 255.0f;
        float g = src[1] / 255.0f;
        float b = src[2] / 255.0f;
        
        float Y  = 0.25f * r + 0.5f * g + 0.25f * b;
        float Co = 0.5f * r - 0.5f * b + 0.5f;
        float Cg = -0.25f * r + 0.5f * g - 0.25f * b + 0.5f;
        
        // Store in modified layout
        dst[0] = Co * 255;  // Alpha channel
        dst[1] = Cg * 255;  // Green channel
        dst[2] = 0;         // Blue unused
        dst[3] = Y * 255;   // Red channel
    }
    
    // 2. Compress 4x4 blocks with DXT5
    CompressDXT5Block(dst, compressed);
}

// Normal map DXT5 compression
void CompressNormalDXT5(const byte* src, byte* dst,
                       int width, int height) {
    // 1. Swizzle components for optimal compression
    for (each pixel) {
        normal.x = src[0];  // Store in alpha
        normal.y = src[1];  // Store in green
        // Z reconstructed in shader
    }
    
    // 2. Compress with DXT5
    CompressDXT5Block(swizzled, compressed);
}
```

### 5.3 Texture Loading and Caching

```cpp
struct textureUsage_t {
    TD_SPECULAR,     // Specular maps → DXT1
    TD_DIFFUSE,      // Diffuse maps → DXT5 YCoCg
    TD_NORMAL,       // Normal maps → DXT5 swizzled
    TD_FONT,         // Font atlases → DXT1 green-alpha
    TD_LIGHT,        // Light textures → Uncompressed
    TD_LOOKUP,       // Lookup tables → Uncompressed
    TD_COVERAGE      // Coverage maps → Alpha only
};

// Automatic format selection
textureFormat_t SelectTextureFormat(textureUsage_t usage) {
    switch(usage) {
        case TD_SPECULAR:
            return FMT_DXT1;
        case TD_DIFFUSE:
            return FMT_DXT5_YCOCG;
        case TD_NORMAL:
            return FMT_DXT5_NORMAL;
        case TD_FONT:
            return FMT_DXT1_ALPHA;
        default:
            return FMT_RGBA8;
    }
}
```

### 5.4 Mipmap Generation

```cpp
void GenerateMipmaps(idImage* image) {
    int width = image->width;
    int height = image->height;
    
    for (int level = 1; level < image->numLevels; level++) {
        int newWidth = max(1, width >> 1);
        int newHeight = max(1, height >> 1);
        
        // Different filters for different texture types
        if (image->usage == TD_NORMAL) {
            // Don't filter normal maps linearly
            DownsampleNormalMap(srcData, dstData, 
                              width, height);
        } else {
            // Gamma-correct filtering for color
            DownsampleWithGamma(srcData, dstData,
                              width, height, 2.2f);
        }
        
        width = newWidth;
        height = newHeight;
    }
}
```

---

## 6. Shadow Rendering Implementation

### 6.1 Shadow Volume Algorithm

```cpp
// Core shadow volume generation
void R_CreateShadowVolume(renderEntity_t* entity,
                         renderLight_t* light,
                         srfTriangles_t* tri) {
    // Phase 1: Determine triangle facing
    byte* facing = (byte*)_alloca(tri->numIndexes / 3);
    R_CalcTriangleFacing(tri, light->origin, facing);
    
    // Phase 2: Find silhouette edges
    silEdge_t* silEdges = (silEdge_t*)_alloca(
        tri->numSilEdges * sizeof(silEdge_t));
    int numSilEdges = R_FindSilhouetteEdges(tri, facing, silEdges);
    
    // Phase 3: Generate shadow volume geometry
    shadowVertex_t* shadowVerts = frameData->AllocVertex(
        numSilEdges * 4);  // 4 verts per edge
    
    for (int i = 0; i < numSilEdges; i++) {
        silEdge_t& edge = silEdges[i];
        
        // Extrude edge to infinity
        vec3 v1 = tri->verts[edge.v1].xyz;
        vec3 v2 = tri->verts[edge.v2].xyz;
        vec3 v3 = v1 - light->origin;
        vec3 v4 = v2 - light->origin;
        
        // Create quad (2 triangles)
        shadowVerts[i*4+0].xyz = v1;
        shadowVerts[i*4+1].xyz = v2;
        shadowVerts[i*4+2].xyz = v2 + v4 * SHADOW_INFINITY;
        shadowVerts[i*4+3].xyz = v1 + v3 * SHADOW_INFINITY;
    }
    
    // Phase 4: Add caps if using Z-fail
    if (r_useZFail) {
        R_AddShadowCaps(tri, facing, light->origin);
    }
}
```

### 6.2 Silhouette Detection

```cpp
struct silEdge_t {
    int v1, v2;           // Vertex indices
    int f1, f2;           // Face indices
};

int R_FindSilhouetteEdges(srfTriangles_t* tri,
                         byte* facing,
                         silEdge_t* silEdges) {
    int numSilEdges = 0;
    
    // Check all edges
    for (int i = 0; i < tri->numSilEdges; i++) {
        silEdge_t* edge = &tri->silEdges[i];
        
        // Silhouette if adjacent faces have different facing
        if (facing[edge->f1] != facing[edge->f2]) {
            // Ensure consistent winding
            if (facing[edge->f1]) {
                silEdges[numSilEdges].v1 = edge->v1;
                silEdges[numSilEdges].v2 = edge->v2;
            } else {
                silEdges[numSilEdges].v1 = edge->v2;
                silEdges[numSilEdges].v2 = edge->v1;
            }
            numSilEdges++;
        }
    }
    
    return numSilEdges;
}
```

### 6.3 Z-Pass vs Z-Fail Implementation

```cpp
// Stencil shadow rendering
void RB_ShadowVolumePass(drawSurf_t* drawSurfs, int numDrawSurfs) {
    // Setup stencil state
    GL_State(GLS_DEPTHMASK | GLS_COLORMASK | 
            GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS);
    GL_Cull(CT_TWO_SIDED);
    
    if (r_useZPass) {
        // Z-Pass (Depth Pass) Method
        // Front faces increment, back faces decrement
        
        GL_StencilOp(GL_KEEP, GL_KEEP, GL_INCR_WRAP);
        GL_Cull(CT_FRONT_SIDED);
        RB_DrawShadowElements(drawSurfs, numDrawSurfs);
        
        GL_StencilOp(GL_KEEP, GL_KEEP, GL_DECR_WRAP);
        GL_Cull(CT_BACK_SIDED);
        RB_DrawShadowElements(drawSurfs, numDrawSurfs);
        
    } else {
        // Z-Fail (Carmack's Reverse) Method
        // Back faces increment, front faces decrement
        
        GL_StencilOp(GL_KEEP, GL_INCR_WRAP, GL_KEEP);
        GL_Cull(CT_BACK_SIDED);
        RB_DrawShadowElements(drawSurfs, numDrawSurfs);
        
        GL_StencilOp(GL_KEEP, GL_DECR_WRAP, GL_KEEP);
        GL_Cull(CT_FRONT_SIDED);
        RB_DrawShadowElements(drawSurfs, numDrawSurfs);
    }
}
```

### 6.4 Multi-threaded Shadow Generation

```cpp
class StaticShadowVolumeJob : public idParallelJob {
    void Run(void* data) {
        shadowGenParms_t* parms = (shadowGenParms_t*)data;
        
        // Process triangles in batches
        const int BATCH_SIZE = 32;
        
        for (int i = parms->startTri; i < parms->endTri; 
             i += BATCH_SIZE) {
            // SIMD processing of 4 triangles at once
            ProcessTriangleBatchSSE2(
                &parms->triangles[i],
                min(BATCH_SIZE, parms->endTri - i),
                parms->lightOrigin,
                parms->facing
            );
        }
        
        // Generate shadow volume from facing data
        GenerateShadowVolumeFromFacing(parms);
    }
};

// Launch parallel shadow jobs
void R_ParallelShadowGeneration(shadowGenParms_t* parms) {
    const int numJobs = idParallelJobManager::GetNumProcessors();
    
    for (int i = 0; i < numJobs; i++) {
        shadowJobs[i].parms = parms[i];
        parallelJobManager->AddJob(&shadowJobs[i]);
    }
    
    parallelJobManager->WaitForAllJobs();
}
```

---

## 7. Memory Management Architecture

### 7.1 Vertex Cache System

```cpp
class idVertexCache {
    // Configuration
    static const int VERTEX_CACHE_FRAME_SIZE = 31 * 1024 * 1024;
    static const int INDEX_CACHE_FRAME_SIZE = 31 * 1024 * 1024;
    static const int JOINT_CACHE_FRAME_SIZE = 256 * 1024;
    
    // Frame-based allocation
    struct frameData_t {
        int    frameNum;
        int    vertexMemUsed;
        int    indexMemUsed;
        int    jointMemUsed;
        byte*  vertexMemory;
        byte*  indexMemory;
        byte*  jointMemory;
    };
    
    frameData_t frames[NUM_FRAME_DATA];
    
    // Static buffer storage
    idBufferObject staticVertexBuffer;
    idBufferObject staticIndexBuffer;
    
    // Handle encoding (64-bit)
    // Bits 63-49: Frame number (15 bits)
    // Bits 48-24: Offset (25 bits)
    // Bits 23-1:  Size (23 bits)
    // Bit 0:      Static flag
};

// Vertex cache allocation
vertCacheHandle_t AllocVertex(int numBytes) {
    frameData_t* frame = &frames[currentFrame];
    
    // Align to 16 bytes for SIMD
    numBytes = ALIGN(numBytes, 16);
    
    if (frame->vertexMemUsed + numBytes > VERTEX_CACHE_FRAME_SIZE) {
        idLib::Error("Vertex cache overflow");
    }
    
    vertCacheHandle_t handle;
    handle.frameNumber = frame->frameNum;
    handle.offset = frame->vertexMemUsed;
    handle.size = numBytes;
    handle.isStatic = false;
    
    frame->vertexMemUsed += numBytes;
    
    return handle;
}
```

### 7.2 Frame Memory Allocation

```cpp
class idFrameData {
    // Memory pools
    struct memoryPool_t {
        byte*  base;
        int    size;
        int    used;
        int    peakUsed;
    };
    
    memoryPool_t pools[FRAME_ALLOC_MAX];
    
    // Type-safe allocation
    template<typename T>
    T* Alloc(int count = 1) {
        int bytes = sizeof(T) * count;
        bytes = ALIGN(bytes, 16);  // SIMD alignment
        
        memoryPool_t& pool = pools[T::POOL_TYPE];
        
        if (pool.used + bytes > pool.size) {
            ResizePool(pool, bytes);
        }
        
        T* ptr = (T*)(pool.base + pool.used);
        pool.used += bytes;
        
        return ptr;
    }
    
    // Frame reset
    void ResetFrame() {
        for (auto& pool : pools) {
            pool.used = 0;
        }
    }
};
```

### 7.3 Buffer Object Management

```cpp
class idBufferObject {
    GLenum    target;       // GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER
    GLuint    buffer;       // OpenGL buffer ID
    int       size;         // Buffer size
    int       offsetInOtherBuffer; // For sub-allocation
    
    // Usage hints
    bufferUsage_t usage;    // Static/Dynamic/Stream
    
    // Memory mapping
    void* MapBuffer(bufferMapMode_t mode) {
        GLenum access;
        switch(mode) {
            case BM_READ:  access = GL_READ_ONLY_ARB; break;
            case BM_WRITE: access = GL_WRITE_ONLY_ARB; break;
            case BM_READ_WRITE: access = GL_READ_WRITE_ARB; break;
        }
        
        void* ptr = glMapBufferARB(target, access);
        
        // Use write-combined memory for streaming
        if (mode == BM_WRITE && usage == BU_STREAM) {
            SetWriteCombined(ptr, size);
        }
        
        return ptr;
    }
    
    // Optimized update
    void Update(const void* data, int updateSize) {
        if (usage == BU_STREAM) {
            // Orphan buffer for streaming data
            glBufferDataARB(target, size, NULL, GL_STREAM_DRAW_ARB);
        }
        glBufferSubDataARB(target, 0, updateSize, data);
    }
};
```

---

## 8. Optimization Strategies

### 8.1 SIMD Optimizations

```cpp
// SSE2 triangle facing calculation
void CalcTriangleFacingSSE2(const idDrawVert* verts,
                            const int* indices,
                            const idVec3& lightOrigin,
                            byte* facing,
                            int numTris) {
    __m128 lightX = _mm_set1_ps(lightOrigin.x);
    __m128 lightY = _mm_set1_ps(lightOrigin.y);
    __m128 lightZ = _mm_set1_ps(lightOrigin.z);
    
    for (int i = 0; i < numTris; i += 4) {
        // Load 4 triangles at once
        __m128 v0x = _mm_load_ps(&verts[indices[i*3+0]].xyz.x);
        __m128 v0y = _mm_load_ps(&verts[indices[i*3+0]].xyz.y);
        __m128 v0z = _mm_load_ps(&verts[indices[i*3+0]].xyz.z);
        
        // Calculate edge vectors
        __m128 e1x = _mm_sub_ps(v1x, v0x);
        __m128 e1y = _mm_sub_ps(v1y, v0y);
        __m128 e1z = _mm_sub_ps(v1z, v0z);
        
        // Cross product for normal
        __m128 nx = _mm_sub_ps(_mm_mul_ps(e1y, e2z), 
                               _mm_mul_ps(e1z, e2y));
        __m128 ny = _mm_sub_ps(_mm_mul_ps(e1z, e2x), 
                               _mm_mul_ps(e1x, e2z));
        __m128 nz = _mm_sub_ps(_mm_mul_ps(e1x, e2y), 
                               _mm_mul_ps(e1y, e2x));
        
        // Light vector
        __m128 lx = _mm_sub_ps(lightX, v0x);
        __m128 ly = _mm_sub_ps(lightY, v0y);
        __m128 lz = _mm_sub_ps(lightZ, v0z);
        
        // Dot product
        __m128 dot = _mm_add_ps(_mm_mul_ps(nx, lx),
                                _mm_add_ps(_mm_mul_ps(ny, ly),
                                          _mm_mul_ps(nz, lz)));
        
        // Store facing results
        int mask = _mm_movemask_ps(_mm_cmpgt_ps(dot, 
                                                 _mm_setzero_ps()));
        facing[i+0] = (mask & 1) ? 1 : 0;
        facing[i+1] = (mask & 2) ? 1 : 0;
        facing[i+2] = (mask & 4) ? 1 : 0;
        facing[i+3] = (mask & 8) ? 1 : 0;
    }
}
```

### 8.2 Culling Optimizations

```cpp
// Hierarchical culling system
class idRenderWorldLocal {
    // Area-based culling
    struct portalArea_t {
        int          areaNum;
        idBounds     bounds;
        portal_t*    portals;
        areaReference_t* entityRefs;
        areaReference_t* lightRefs;
    };
    
    // Fast bounds testing
    bool CullBounds(const idBounds& bounds, 
                    const idFrustum& frustum) {
        // Early rejection with sphere test
        float radius = bounds.GetRadius();
        idVec3 center = bounds.GetCenter();
        
        for (int i = 0; i < 6; i++) {
            float dist = frustum[i].Distance(center);
            if (dist < -radius) {
                return true;  // Fully outside
            }
        }
        
        // Detailed box test only if needed
        return frustum.CullBounds(bounds);
    }
    
    // Occlusion query optimization
    void IssueOcclusionQuery(renderEntity_t* entity) {
        if (!r_useOcclusionQueries.GetBool()) {
            return;
        }
        
        // Render bounding box with occlusion query
        GLuint query = entity->occlusionQuery;
        glBeginQueryARB(GL_SAMPLES_PASSED_ARB, query);
        RB_DrawBounds(entity->bounds);
        glEndQueryARB(GL_SAMPLES_PASSED_ARB);
        
        // Check result next frame to avoid stall
        entity->occlusionQueryFrame = frameCount;
    }
};
```

### 8.3 Sorting and Batching

```cpp
// Sort key generation for optimal batching
uint64 R_GenerateSortKey(const drawSurf_t* surf) {
    uint64 key = 0;
    
    // Opaque surfaces: sort front-to-back
    if (surf->material->Coverage() == MC_OPAQUE) {
        // Bits 63-55: Sort order (lower = earlier)
        key |= uint64(surf->material->GetSort()) << 55;
        
        // Bits 54-40: Depth (for front-to-back)
        float depth = surf->viewDepth;
        int depthInt = idMath::Ftoi(depth * 32767.0f);
        key |= uint64(depthInt & 0x7FFF) << 40;
        
    } else {
        // Translucent: sort back-to-front
        key |= uint64(SS_POST_PROCESS) << 55;
        
        // Inverse depth for back-to-front
        float depth = 100000.0f - surf->viewDepth;
        int depthInt = idMath::Ftoi(depth * 32767.0f);
        key |= uint64(depthInt & 0x7FFF) << 40;
    }
    
    // Bits 39-20: Material pointer (for batching)
    key |= (uint64(surf->material) >> 4) & 0xFFFFF;
    
    // Bits 19-0: Entity/light index
    key |= surf->entityNum & 0xFFFFF;
    
    return key;
}

// Radix sort for draw surfaces
void R_RadixSort(drawSurf_t** surfs, int numSurfs) {
    // 4-pass radix sort on 64-bit keys
    for (int pass = 0; pass < 4; pass++) {
        int shift = pass * 16;
        
        // Count buckets
        int counts[65536] = {0};
        for (int i = 0; i < numSurfs; i++) {
            int bucket = (surfs[i]->sort >> shift) & 0xFFFF;
            counts[bucket]++;
        }
        
        // Prefix sum
        int offsets[65536];
        offsets[0] = 0;
        for (int i = 1; i < 65536; i++) {
            offsets[i] = offsets[i-1] + counts[i-1];
        }
        
        // Scatter
        drawSurf_t** temp = frameAlloc->Alloc<drawSurf_t*>(numSurfs);
        for (int i = 0; i < numSurfs; i++) {
            int bucket = (surfs[i]->sort >> shift) & 0xFFFF;
            temp[offsets[bucket]++] = surfs[i];
        }
        
        memcpy(surfs, temp, numSurfs * sizeof(drawSurf_t*));
    }
}
```

---

## 9. Implementation Guidelines

### 9.1 Renderer Integration

```cpp
// Main render loop integration
void idRenderSystemLocal::RenderView(viewDef_t* viewDef) {
    // 1. Setup view parameters
    R_SetupViewMatrix(viewDef);
    R_SetupProjectionMatrix(viewDef);
    
    // 2. Portal visibility determination
    R_FlowViewThroughPortals(viewDef);
    
    // 3. Add visible entities and lights
    for (auto* entity : viewDef->visibleEntities) {
        R_AddEntitySurfaces(entity, viewDef);
    }
    
    for (auto* light : viewDef->visibleLights) {
        R_AddLightSurfaces(light, viewDef);
        R_CreateLightInteractions(light, viewDef);
    }
    
    // 4. Generate shadow volumes
    if (r_shadows.GetInteger() > 0) {
        R_CreateShadowVolumes(viewDef);
    }
    
    // 5. Sort surfaces for optimal rendering
    R_SortDrawSurfs(viewDef->drawSurfs, viewDef->numDrawSurfs);
    
    // 6. Generate backend commands
    R_GenerateDrawCommands(viewDef);
    
    // 7. Submit to backend
    R_IssueRenderCommands();
}
```

### 9.2 Material System Integration

```cpp
// Material loading and parsing
class idMaterialManager {
    idHashTable<idMaterial*> materials;
    
    idMaterial* LoadMaterial(const char* name) {
        // Check cache
        idMaterial* existing = materials.Get(name);
        if (existing) {
            return existing;
        }
        
        // Parse material file
        idLexer lexer(name);
        idMaterial* mat = new idMaterial();
        
        while (!lexer.EndOfFile()) {
            idToken token;
            lexer.ReadToken(&token);
            
            if (token == "{") {
                // Parse material stage
                ParseMaterialStage(lexer, mat);
            } else {
                // Parse material keyword
                ParseMaterialKeyword(token, lexer, mat);
            }
        }
        
        // Validate and optimize
        mat->Validate();
        mat->OptimizeExpressions();
        
        materials.Set(name, mat);
        return mat;
    }
};
```

### 9.3 Shader Program Management

```cpp
// GLSL shader compilation and linking
class idRenderProgManager {
    struct glslProgram_t {
        GLuint          program;
        GLuint          vertexShader;
        GLuint          fragmentShader;
        int             uniformLocations[RENDERPARM_TOTAL];
        vertexLayout_t  vertexLayout;
    };
    
    glslProgram_t builtinPrograms[BUILTIN_TOTAL];
    
    void CompileBuiltinProgram(builtinProgram_t type) {
        const char* vertexSource = GetBuiltinVertexShader(type);
        const char* fragmentSource = GetBuiltinFragmentShader(type);
        
        GLuint program = glCreateProgram();
        
        // Compile vertex shader
        GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexSource);
        glAttachShader(program, vs);
        
        // Compile fragment shader
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
        glAttachShader(program, fs);
        
        // Bind vertex attributes
        glBindAttribLocation(program, VERTEX_POSITION, "position");
        glBindAttribLocation(program, VERTEX_NORMAL, "normal");
        glBindAttribLocation(program, VERTEX_TANGENT, "tangent");
        glBindAttribLocation(program, VERTEX_COLOR, "color");
        glBindAttribLocation(program, VERTEX_TEXCOORD0, "texcoord0");
        glBindAttribLocation(program, VERTEX_TEXCOORD1, "texcoord1");
        
        // Link program
        glLinkProgram(program);
        
        // Cache uniform locations
        for (int i = 0; i < RENDERPARM_TOTAL; i++) {
            const char* name = GetUniformName(i);
            uniformLocations[i] = glGetUniformLocation(program, name);
        }
        
        builtinPrograms[type].program = program;
    }
};
```

---

## 10. Performance Considerations

### 10.1 Profiling and Metrics

```cpp
struct renderStats_t {
    int c_drawElements;      // Draw calls
    int c_drawIndexes;       // Indices drawn
    int c_drawVertexes;      // Vertices processed
    int c_shadowElements;    // Shadow draw calls
    int c_shadowIndexes;     // Shadow indices
    int c_shadowVertexes;    // Shadow vertices
    int c_interactions;      // Light interactions
    int c_deformedSurfaces;  // Dynamic surfaces
    int c_deformedVertexes;  // Dynamic vertices
    int c_deformedIndexes;   // Dynamic indices
    int c_materialChanges;   // Material switches
    int c_shaderPasses;      // Shader passes
    
    float frontEndMsec;      // Frontend time
    float backEndMsec;       // Backend time
    float shadowMsec;        // Shadow generation
    float gpuMsec;           // GPU time
};
```

### 10.2 Performance Guidelines

| Optimization | Impact | Implementation |
|--------------|--------|----------------|
| **Portal Culling** | High | Pre-compute portal connectivity |
| **Light Scissors** | High | Calculate tight screen bounds |
| **Shadow LOD** | Medium | Distance-based shadow quality |
| **Material Batching** | High | Sort by material to reduce state changes |
| **Vertex Cache** | High | Reuse transformed vertices |
| **SIMD Usage** | High | Process 4 elements simultaneously |
| **Occlusion Queries** | Medium | GPU-based visibility testing |
| **Texture Compression** | High | DXT compression for bandwidth |
| **Multi-threading** | High | Parallel shadow/animation processing |

### 10.3 Memory Budgets

```cpp
// Recommended memory allocations
const int VERTEX_CACHE_SIZE = 32 * 1024 * 1024;     // Per frame
const int INDEX_CACHE_SIZE = 32 * 1024 * 1024;      // Per frame
const int JOINT_CACHE_SIZE = 256 * 1024;            // Skinning
const int STATIC_VERTEX_SIZE = 256 * 1024 * 1024;   // Static geo
const int STATIC_INDEX_SIZE = 128 * 1024 * 1024;    // Static indices
const int TEXTURE_MEMORY = 512 * 1024 * 1024;       // Texture pool
const int SHADOW_MEMORY = 64 * 1024 * 1024;         // Shadow volumes
```

### 10.4 Optimization Checklist

- [ ] **Culling**
  - [ ] Implement hierarchical frustum culling
  - [ ] Use portal-based visibility
  - [ ] Calculate light scissors
  - [ ] Implement occlusion queries
  
- [ ] **Batching**
  - [ ] Sort surfaces by material
  - [ ] Minimize state changes
  - [ ] Combine small draw calls
  - [ ] Use instancing where possible
  
- [ ] **Memory**
  - [ ] Use vertex cache for dynamic geometry
  - [ ] Implement texture compression
  - [ ] Pool frame allocations
  - [ ] Align data for SIMD
  
- [ ] **Shadows**
  - [ ] Pre-compute static shadows
  - [ ] Use shadow LOD system
  - [ ] Parallelize shadow generation
  - [ ] Optimize silhouette detection
  
- [ ] **Shaders**
  - [ ] Minimize uniform updates
  - [ ] Use texture atlases
  - [ ] Optimize shader complexity
  - [ ] Cache compiled programs

---

## Conclusion

The DOOM 3 BFG rendering system represents a mature, highly-optimized forward renderer with unified lighting and stencil shadow volumes. Its architecture emphasizes:

1. **Clear separation** between frontend scene processing and backend GPU execution
2. **Aggressive culling** through portals, frustums, and light scissors
3. **Sophisticated material system** with expression-based animation
4. **Optimized shadow rendering** with parallel processing
5. **Efficient memory management** through frame-based allocation
6. **Extensive SIMD optimization** for performance-critical paths

This design document provides the technical foundation for understanding, maintaining, and extending the DOOM 3 BFG renderer. The system's modular architecture allows for targeted optimizations while maintaining compatibility with the existing content pipeline.

For implementation, focus on the core pipeline first (frontend/backend separation), then layer in optimizations (culling, batching, SIMD) based on profiling data. The material and lighting systems can be extended independently, allowing for visual improvements without major architectural changes.

The renderer's emphasis on CPU-side culling and optimization remains relevant even with modern GPUs, as it minimizes unnecessary GPU work and maintains consistent performance across varying hardware configurations.