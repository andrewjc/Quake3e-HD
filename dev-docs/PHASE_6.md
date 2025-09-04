# Phase 6: Shadow Mapping and GPU-Driven Culling

## Executive Summary

Phase 6 completes the renderer modernization by implementing cascaded shadow mapping (CSM) for directional lights, omnidirectional cube shadow maps for point lights, and GPU-driven occlusion culling using hierarchical Z-buffer (Hi-Z). This phase replaces the CPU-intensive stencil shadow volumes with efficient shadow mapping techniques while implementing GPU-based visibility determination that drastically reduces CPU overhead and draw call counts.

## Architectural Goals

1. **Modern Shadow Techniques**: Replace stencil shadows with shadow mapping
2. **GPU Autonomy**: Move visibility decisions to GPU compute shaders
3. **Bandwidth Efficiency**: Minimize CPU-GPU data transfer
4. **Scalability**: Support hundreds of shadow-casting lights
5. **Quality Options**: Multiple shadow quality levels for performance scaling

## Shadow Mapping System

### Cascaded Shadow Maps (CSM) for Directional Lights

```c
// File: src/engine/renderer/vulkan/vk_shadows.h (NEW FILE)

#define MAX_CASCADE_COUNT 4
#define SHADOW_MAP_SIZE 2048

typedef struct csmData_s {
    // Per-cascade data
    struct {
        mat4_t          viewProjMatrix;
        vec4_t          splitDistances;    // Near/far for this cascade
        VkImageView     depthView;
        VkFramebuffer   framebuffer;
    } cascades[MAX_CASCADE_COUNT];
    
    // Shadow map array
    VkImage             shadowMapArray;
    VkDeviceMemory      shadowMapMemory;
    VkImageView         shadowMapArrayView;
    VkSampler           shadowSampler;
    
    // Rendering
    VkRenderPass        shadowRenderPass;
    VkPipeline          shadowPipeline;
    VkPipelineLayout    shadowPipelineLayout;
    
    // Configuration
    float               cascadeSplitLambda;    // Cascade distribution
    float               shadowDistance;        // Maximum shadow distance
    int                 cascadeCount;          // Active cascades
} csmData_t;

void VK_CalculateCSMSplits(csmData_t *csm, float nearPlane, float farPlane) {
    float lambda = csm->cascadeSplitLambda;
    float ratio = farPlane / nearPlane;
    
    for (int i = 0; i < csm->cascadeCount; i++) {
        float p = (i + 1) / (float)csm->cascadeCount;
        float log = nearPlane * powf(ratio, p);
        float uniform = nearPlane + (farPlane - nearPlane) * p;
        float d = lambda * (log - uniform) + uniform;
        
        csm->cascades[i].splitDistances.x = (i == 0) ? nearPlane : 
                                            csm->cascades[i-1].splitDistances.y;
        csm->cascades[i].splitDistances.y = d;
    }
}

void VK_RenderCSMCascade(int cascade, csmData_t *csm, drawSurf_t **surfaces, int numSurfaces) {
    VkCommandBuffer cmd = vk_commandBuffer;
    
    // Begin shadow render pass
    VkRenderPassBeginInfo rpBegin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = csm->shadowRenderPass,
        .framebuffer = csm->cascades[cascade].framebuffer,
        .renderArea = {
            .extent = { SHADOW_MAP_SIZE, SHADOW_MAP_SIZE }
        },
        .clearValueCount = 1,
        .pClearValues = &(VkClearValue){ .depthStencil = { 1.0f, 0 } }
    };
    
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, csm->shadowPipeline);
    
    // Push cascade view-projection matrix
    vkCmdPushConstants(cmd, csm->shadowPipelineLayout,
                      VK_SHADER_STAGE_VERTEX_BIT, 0, 
                      sizeof(mat4_t), &csm->cascades[cascade].viewProjMatrix);
    
    // Render shadow casters
    for (int i = 0; i < numSurfaces; i++) {
        if (surfaces[i]->material->flags & MATERIAL_NOSHADOWS)
            continue;
            
        VK_BindVertexBuffer(surfaces[i]->vertexBuffer);
        VK_BindIndexBuffer(surfaces[i]->indexBuffer);
        vkCmdDrawIndexed(cmd, surfaces[i]->numIndices, 1, 0, 0, 0);
    }
    
    vkCmdEndRenderPass(cmd);
}
```

### Omnidirectional Shadow Maps

```c
typedef struct pointShadowMap_s {
    VkImage             cubeMap;
    VkImageView         cubeMapView;
    VkImageView         faceViews[6];      // Individual cube faces
    VkFramebuffer       framebuffers[6];
    
    mat4_t              viewMatrices[6];   // View matrix per face
    mat4_t              projMatrix;        // 90 degree FOV projection
    
    float               nearPlane;
    float               farPlane;
} pointShadowMap_t;

// Cube face directions
static const vec3_t cubeFaceDirections[6][2] = {
    { { 1, 0, 0}, { 0, -1, 0} },  // +X
    { {-1, 0, 0}, { 0, -1, 0} },  // -X
    { { 0, 1, 0}, { 0,  0, 1} },  // +Y
    { { 0,-1, 0}, { 0,  0,-1} },  // -Y
    { { 0, 0, 1}, { 0, -1, 0} },  // +Z
    { { 0, 0,-1}, { 0, -1, 0} }   // -Z
};

void VK_RenderPointShadowMap(renderLight_t *light, pointShadowMap_t *shadowMap) {
    // Update view matrices for light position
    for (int face = 0; face < 6; face++) {
        vec3_t forward = cubeFaceDirections[face][0];
        vec3_t up = cubeFaceDirections[face][1];
        
        Mat4_LookAt(shadowMap->viewMatrices[face], 
                   light->origin, forward, up);
    }
    
    // Render each cube face
    for (int face = 0; face < 6; face++) {
        VK_BeginShadowPass(shadowMap->framebuffers[face]);
        
        // Frustum cull against this face
        frustum_t faceFrustum;
        R_SetupFrustum(&faceFrustum, shadowMap->viewMatrices[face], 
                      shadowMap->projMatrix);
        
        // Render visible surfaces
        interaction_t *inter = light->firstInteraction;
        while (inter) {
            if (!inter->culled && R_SurfaceInFrustum(inter->surface, &faceFrustum)) {
                VK_RenderShadowSurface(inter->surface, 
                                     shadowMap->viewMatrices[face],
                                     shadowMap->projMatrix);
            }
            inter = inter->lightNext;
        }
        
        VK_EndShadowPass();
    }
}
```

## GPU-Driven Occlusion Culling

### Hierarchical Z-Buffer (Hi-Z) Generation

```c
typedef struct hiZBuffer_s {
    VkImage             hiZImage;
    VkImageView         hiZViews[MAX_HIZ_LEVELS];  // Mip level views
    VkSampler           hiZSampler;
    
    int                 numLevels;
    int                 width, height;
    
    // Compute shader for Hi-Z generation
    VkPipeline          buildPipeline;
    VkPipelineLayout    buildPipelineLayout;
    VkDescriptorSet     buildDescriptorSet;
} hiZBuffer_t;

// Compute shader for Hi-Z pyramid construction
const char* hiZBuildCompute = R"(
#version 460
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D depthTexture;
layout(set = 0, binding = 1, r32f) writeonly uniform image2D hiZMip;

layout(push_constant) uniform PushConstants {
    ivec2 srcSize;
    int srcMip;
} pc;

void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(coord, pc.srcSize))) return;
    
    // Sample 4 texels from previous level
    vec2 uv = (vec2(coord) + 0.5) / vec2(pc.srcSize);
    vec4 depths;
    depths.x = textureLod(depthTexture, uv, pc.srcMip).r;
    depths.y = textureLodOffset(depthTexture, uv, pc.srcMip, ivec2(1, 0)).r;
    depths.z = textureLodOffset(depthTexture, uv, pc.srcMip, ivec2(0, 1)).r;
    depths.w = textureLodOffset(depthTexture, uv, pc.srcMip, ivec2(1, 1)).r;
    
    // Store maximum depth (conservative for occlusion)
    float maxDepth = max(max(depths.x, depths.y), max(depths.z, depths.w));
    imageStore(hiZMip, coord, vec4(maxDepth));
}
)";

void VK_BuildHiZPyramid(hiZBuffer_t *hiZ, VkCommandBuffer cmd) {
    // Transition depth buffer for reading
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = vk_depthImage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1
        }
    };
    
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);
    
    // Build Hi-Z pyramid
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZ->buildPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           hiZ->buildPipelineLayout, 0, 1,
                           &hiZ->buildDescriptorSet, 0, NULL);
    
    int width = hiZ->width;
    int height = hiZ->height;
    
    for (int mip = 1; mip < hiZ->numLevels; mip++) {
        width = max(1, width >> 1);
        height = max(1, height >> 1);
        
        struct {
            ivec2 srcSize;
            int srcMip;
        } pushConstants = {
            .srcSize = { width, height },
            .srcMip = mip - 1
        };
        
        vkCmdPushConstants(cmd, hiZ->buildPipelineLayout,
                          VK_SHADER_STAGE_COMPUTE_BIT, 0,
                          sizeof(pushConstants), &pushConstants);
        
        vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
        
        // Memory barrier between mip levels
        VkMemoryBarrier memBarrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
        };
        
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &memBarrier, 0, NULL, 0, NULL);
    }
}
```

### GPU Occlusion Culling

```c
// Compute shader for occlusion culling
const char* occlusionCullCompute = R"(
#version 460

layout(local_size_x = 64) in;

struct BoundingBox {
    vec4 min;  // xyz = min, w = unused
    vec4 max;  // xyz = max, w = unused
};

layout(set = 0, binding = 0) readonly buffer InputBounds {
    BoundingBox bounds[];
} inputBounds;

layout(set = 0, binding = 1) buffer VisibilityBuffer {
    uint visible[];
} visibility;

layout(set = 0, binding = 2) uniform sampler2D hiZBuffer;

layout(set = 0, binding = 3) uniform CullData {
    mat4 viewProjMatrix;
    vec2 viewportSize;
    uint objectCount;
    float znear;
} cullData;

bool isVisible(vec3 minCorner, vec3 maxCorner) {
    // Transform AABB corners to clip space
    vec4 corners[8];
    corners[0] = cullData.viewProjMatrix * vec4(minCorner.x, minCorner.y, minCorner.z, 1.0);
    corners[1] = cullData.viewProjMatrix * vec4(maxCorner.x, minCorner.y, minCorner.z, 1.0);
    corners[2] = cullData.viewProjMatrix * vec4(minCorner.x, maxCorner.y, minCorner.z, 1.0);
    corners[3] = cullData.viewProjMatrix * vec4(maxCorner.x, maxCorner.y, minCorner.z, 1.0);
    corners[4] = cullData.viewProjMatrix * vec4(minCorner.x, minCorner.y, maxCorner.z, 1.0);
    corners[5] = cullData.viewProjMatrix * vec4(maxCorner.x, minCorner.y, maxCorner.z, 1.0);
    corners[6] = cullData.viewProjMatrix * vec4(minCorner.x, maxCorner.y, maxCorner.z, 1.0);
    corners[7] = cullData.viewProjMatrix * vec4(maxCorner.x, maxCorner.y, maxCorner.z, 1.0);
    
    // Find screen space bounding box
    vec2 minScreen = vec2(1.0);
    vec2 maxScreen = vec2(-1.0);
    float minZ = 1.0;
    
    for (int i = 0; i < 8; i++) {
        if (corners[i].w > 0.0) {
            vec3 ndc = corners[i].xyz / corners[i].w;
            minScreen = min(minScreen, ndc.xy);
            maxScreen = max(maxScreen, ndc.xy);
            minZ = min(minZ, ndc.z);
        }
    }
    
    // Check if completely outside screen
    if (maxScreen.x < -1.0 || minScreen.x > 1.0 ||
        maxScreen.y < -1.0 || minScreen.y > 1.0) {
        return false;
    }
    
    // Convert to texture coordinates
    vec2 uvMin = minScreen * 0.5 + 0.5;
    vec2 uvMax = maxScreen * 0.5 + 0.5;
    
    // Calculate appropriate Hi-Z mip level
    vec2 size = (uvMax - uvMin) * cullData.viewportSize;
    float mipLevel = ceil(log2(max(size.x, size.y)));
    
    // Sample Hi-Z buffer
    float occluderZ = textureLod(hiZBuffer, (uvMin + uvMax) * 0.5, mipLevel).r;
    
    // Conservative test
    return minZ <= occluderZ;
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= cullData.objectCount) return;
    
    BoundingBox bbox = inputBounds.bounds[idx];
    visibility.visible[idx] = isVisible(bbox.min.xyz, bbox.max.xyz) ? 1 : 0;
}
)";
```

## Tiled Forward Rendering

### Tile Classification

```c
#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 256

typedef struct tiledLightingData_s {
    // Tile data
    VkBuffer            tileLightIndices;      // Light indices per tile
    VkBuffer            tileLightCount;        // Light count per tile
    
    // Z-prepass for tile depth bounds
    VkPipeline          zPrepassPipeline;
    VkRenderPass        zPrepassRenderPass;
    
    // Tile classification compute
    VkPipeline          tileClassifyPipeline;
    VkPipelineLayout    tileClassifyLayout;
    
    int                 tilesX, tilesY;
} tiledLightingData_t;

// Compute shader for tile light assignment
const char* tileClassifyCompute = R"(
#version 460

layout(local_size_x = TILE_SIZE, local_size_y = TILE_SIZE) in;

layout(set = 0, binding = 0) uniform sampler2D depthBuffer;

layout(set = 0, binding = 1) readonly buffer LightBuffer {
    vec4 position;      // xyz = pos, w = radius
    vec4 color;         // rgb = color, a = intensity
} lights[];

layout(set = 0, binding = 2) buffer TileLightIndices {
    uint indices[];
} tileLightIndices;

layout(set = 0, binding = 3) buffer TileLightCount {
    uint count[];
} tileLightCount;

shared uint tileMinZ;
shared uint tileMaxZ;
shared uint tileLightCount;
shared uint tileLightIndices[MAX_LIGHTS_PER_TILE];

void main() {
    ivec2 tileID = ivec2(gl_WorkGroupID.xy);
    ivec2 pixelPos = ivec2(gl_GlobalInvocationID.xy);
    
    // Initialize shared memory
    if (gl_LocalInvocationIndex == 0) {
        tileMinZ = 0xFFFFFFFF;
        tileMaxZ = 0;
        tileLightCount = 0;
    }
    
    barrier();
    
    // Find tile depth bounds
    float depth = texelFetch(depthBuffer, pixelPos, 0).r;
    uint depthInt = floatBitsToUint(depth);
    
    atomicMin(tileMinZ, depthInt);
    atomicMax(tileMaxZ, depthInt);
    
    barrier();
    
    float minZ = uintBitsToFloat(tileMinZ);
    float maxZ = uintBitsToFloat(tileMaxZ);
    
    // Cull lights (one thread per light)
    uint lightIndex = gl_LocalInvocationIndex;
    while (lightIndex < numLights) {
        vec4 lightPos = lights[lightIndex].position;
        
        // Check if light affects tile
        if (lightIntersectsTile(lightPos, minZ, maxZ, tileID)) {
            uint slot = atomicAdd(tileLightCount, 1);
            if (slot < MAX_LIGHTS_PER_TILE) {
                tileLightIndices[slot] = lightIndex;
            }
        }
        
        lightIndex += TILE_SIZE * TILE_SIZE;
    }
    
    barrier();
    
    // Write results (one thread)
    if (gl_LocalInvocationIndex == 0) {
        uint tileIndex = tileID.y * tilesX + tileID.x;
        uint offset = tileIndex * MAX_LIGHTS_PER_TILE;
        
        tileLightCount.count[tileIndex] = min(tileLightCount, MAX_LIGHTS_PER_TILE);
        
        for (uint i = 0; i < tileLightCount; i++) {
            tileLightIndices.indices[offset + i] = tileLightIndices[i];
        }
    }
}
)";
```

## Performance Optimizations

### Variable Rate Shading (VRS)

```c
typedef struct vrsData_s {
    VkImage             shadingRateImage;
    VkImageView         shadingRateView;
    
    // Shading rate patterns
    VkExtent2D          tileSize;
    
    // Compute shader for rate calculation
    VkPipeline          calculateRatePipeline;
} vrsData_t;

void VK_SetupVariableRateShading(VkCommandBuffer cmd, vrsData_t *vrs) {
    // Calculate shading rates based on:
    // - Screen space derivatives
    // - Motion vectors  
    // - Depth discontinuities
    // - Material properties
    
    VkImageViewShadingRateImageNV shadingRateImage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_SHADING_RATE_IMAGE_NV,
        .shadingRateImage = vrs->shadingRateView,
        .shadingRatePalette = {
            .shadingRatePaletteEntryCount = 4,
            .pShadingRatePaletteEntries = (VkShadingRatePaletteEntryNV[]){
                VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_PIXEL_NV,
                VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_2X2_PIXELS_NV,
                VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_4X4_PIXELS_NV,
                VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_8X8_PIXELS_NV
            }
        }
    };
    
    vkCmdSetShadingRateImageNV(cmd, &shadingRateImage);
}
```

### Mesh Shaders for GPU-Driven Geometry

```c
// Mesh shader for GPU-driven primitive culling
const char* meshShader = R"(
#version 460
#extension GL_NV_mesh_shader : require

layout(local_size_x = 32) in;
layout(max_vertices = 64, max_primitives = 126) out;
layout(triangles) out;

taskPayloadSharedEXT MeshPayload {
    uint meshletIndices[32];
    uint meshletCount;
} payload;

layout(set = 0, binding = 0) readonly buffer MeshletBuffer {
    Meshlet meshlets[];
};

layout(location = 0) out vec3 fragNormal[];
layout(location = 1) out vec2 fragTexCoord[];

void main() {
    uint meshletIndex = payload.meshletIndices[gl_WorkGroupID.x];
    Meshlet meshlet = meshlets[meshletIndex];
    
    // Cull back-facing meshlets
    if (dot(meshlet.coneNormal, viewDir) < meshlet.coneAngle) {
        return;
    }
    
    // Output vertices and primitives
    SetMeshOutputsEXT(meshlet.vertexCount, meshlet.primitiveCount);
    
    // Process vertices in parallel
    for (uint i = gl_LocalInvocationID.x; i < meshlet.vertexCount; i += 32) {
        uint vertexIndex = meshlet.vertices[i];
        gl_MeshVerticesEXT[i].gl_Position = mvpMatrix * vertices[vertexIndex].position;
        fragNormal[i] = vertices[vertexIndex].normal;
        fragTexCoord[i] = vertices[vertexIndex].texCoord;
    }
    
    // Output primitives
    for (uint i = gl_LocalInvocationID.x; i < meshlet.primitiveCount; i += 32) {
        gl_PrimitiveIndicesEXT[i * 3 + 0] = meshlet.indices[i * 3 + 0];
        gl_PrimitiveIndicesEXT[i * 3 + 1] = meshlet.indices[i * 3 + 1];
        gl_PrimitiveIndicesEXT[i * 3 + 2] = meshlet.indices[i * 3 + 2];
    }
}
)";
```

## Memory Management

### GPU Memory Allocation Strategy

```c
typedef struct gpuMemoryPool_s {
    VkDeviceMemory      memory;
    VkDeviceSize        size;
    VkDeviceSize        used;
    VkDeviceSize        alignment;
    
    // Sub-allocators
    struct {
        VkBuffer        buffer;
        VkDeviceSize    offset;
        VkDeviceSize    size;
        void*           mapped;
    } uniformPool, vertexPool, indexPool, storagePool;
    
    // Memory type
    uint32_t            memoryTypeIndex;
    VkMemoryPropertyFlags properties;
} gpuMemoryPool_t;

typedef struct gpuMemoryManager_s {
    // Memory pools
    gpuMemoryPool_t     deviceLocal;       // GPU-only memory
    gpuMemoryPool_t     hostVisible;       // CPU-GPU shared
    gpuMemoryPool_t     hostCached;        // Cached for readback
    
    // Allocation tracking
    struct gpuAllocation_s {
        VkBuffer        buffer;
        VkDeviceMemory  memory;
        VkDeviceSize    offset;
        VkDeviceSize    size;
        int             poolIndex;
    } *allocations;
    int                 numAllocations;
    
    // Statistics
    VkDeviceSize        totalAllocated;
    VkDeviceSize        totalUsed;
    int                 allocationCount;
} gpuMemoryManager_t;
```

## Debug and Profiling Tools

### GPU Timing and Statistics

```c
typedef struct gpuProfiler_s {
    // Timestamp queries
    VkQueryPool         timestampPool;
    uint32_t            timestampCount;
    
    // Pipeline statistics
    VkQueryPool         statisticsPool;
    
    // Per-frame metrics
    struct {
        double          shadowMapTime;
        double          zPrepassTime;
        double          opaquePassTime;
        double          transparentPassTime;
        double          postProcessTime;
        
        uint64_t        verticesProcessed;
        uint64_t        primitivesRendered;
        uint64_t        fragmentsShaded;
        uint64_t        computeInvocations;
    } metrics;
} gpuProfiler_t;

void VK_BeginGPUTimer(gpuProfiler_t *profiler, const char *label) {
    uint32_t queryIndex = profiler->timestampCount++;
    vkCmdWriteTimestamp(vk_commandBuffer, 
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       profiler->timestampPool, queryIndex * 2);
}

void VK_EndGPUTimer(gpuProfiler_t *profiler, const char *label) {
    uint32_t queryIndex = profiler->timestampCount - 1;
    vkCmdWriteTimestamp(vk_commandBuffer,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       profiler->timestampPool, queryIndex * 2 + 1);
}
```

## Success Criteria

Phase 6 is complete when:

1. ✓ Cascaded shadow mapping implemented for directional lights
2. ✓ Cube shadow maps working for point lights  
3. ✓ GPU-driven occlusion culling with Hi-Z operational
4. ✓ Tiled forward rendering for efficient multi-light scenarios
5. ✓ Variable rate shading for performance optimization
6. ✓ Mesh shaders integrated (if hardware supports)
7. ✓ Shadow quality options (low/medium/high/ultra)
8. ✓ No visual quality regression vs stencil shadows
9. ✓ 50%+ performance improvement in shadow rendering
10. ✓ Support for 100+ shadow-casting lights

## Performance Targets

- **Shadow Map Generation**: < 2ms for 4 cascades at 2K resolution
- **Point Light Shadows**: < 1ms per light for cube maps
- **GPU Culling**: 80%+ reduction in draw calls via occlusion culling
- **Tiled Lighting**: Support 1000+ lights with < 5ms overhead
- **Memory Usage**: < 500MB for all shadow maps
- **Scalability**: Maintain 60 FPS with 20 shadow-casting lights

## Integration Points

### Dependencies from Previous Phases
- Phase 1: Command buffer system for parallel shadow map generation
- Phase 2: Portal visibility for light culling
- Phase 3: Material system for shadow-related properties
- Phase 4: Unified pipeline for shadow map rendering
- Phase 5: Interaction system for shadow caster determination

### Configuration CVARs
```c
// Shadow quality
cvar_t *r_shadowMapSize;           // Shadow map resolution
cvar_t *r_shadowCascadeCount;      // Number of CSM cascades
cvar_t *r_shadowDistance;          // Maximum shadow distance
cvar_t *r_shadowSoftness;          // PCF kernel size
cvar_t *r_shadowBias;              // Depth bias to prevent acne

// GPU culling
cvar_t *r_gpuCulling;              // Enable GPU-driven culling
cvar_t *r_hiZOcclusion;            // Use Hi-Z occlusion culling
cvar_t *r_frustumCullGPU;          // GPU frustum culling

// Tiled rendering
cvar_t *r_tiledLighting;           // Enable tiled forward rendering
cvar_t *r_maxLightsPerTile;        // Maximum lights per tile

// Advanced features
cvar_t *r_variableRateShading;     // Enable VRS
cvar_t *r_meshShaders;             // Use mesh shaders if available
```

## Risk Mitigation

1. **Hardware Compatibility**: Provide fallbacks for older GPUs
2. **Shadow Acne**: Implement robust bias and normal offset
3. **Performance Scaling**: Multiple quality presets
4. **Memory Pressure**: Dynamic shadow map allocation
5. **Temporal Stability**: Frame-to-frame coherence for shadows

This completes the 6-phase renderer modernization plan, transforming Quake 3's renderer into a state-of-the-art Vulkan-based system inspired by DOOM 3 BFG's architecture.