# Phase 4: Unified Vulkan Rendering Pipeline

## Executive Summary  

Phase 4 implements a unified Vulkan rendering pipeline using a single uber-shader that dynamically interprets material expressions from Phase 3. This approach leverages Vulkan's descriptor indexing, bindless textures, and GPU-driven rendering to eliminate pipeline permutations and state changes. The system uses indirect drawing commands and GPU-side culling to maximize parallelism and minimize CPU-GPU synchronization.

## Current State Analysis

### Existing Vulkan Implementation

Based on the codebase analysis:
- **Location**: `src/engine/renderer/vulkan/vk.c`
- **Current approach**: Multiple pipeline permutations for different shader combinations
- **Pipeline management**: Pre-compiled pipelines for various state combinations
- **Shader system**: SPIR-V shaders compiled from GLSL

## GPU-Driven Rendering Architecture

### Indirect Draw System
```c
// File: src/engine/renderer/vulkan/vk_indirect.h (NEW FILE)

// GPU-side draw command
typedef struct {
    VkDrawIndexedIndirectCommand   drawCmd;
    uint32_t                        materialIndex;
    uint32_t                        transformIndex;
    uint32_t                        instanceDataOffset;
} gpuDrawCommand_t;

// GPU culling input
typedef struct {
    vec4_t      boundingSphere;     // xyz = center, w = radius
    uint32_t    drawIndex;          // Index in draw command buffer
    uint32_t    visible;            // Output: 1 if visible
} gpuCullData_t;

// Compute shader for GPU culling
const char* cullComputeShader = R"
#version 460
#extension GL_ARB_gpu_shader_int64 : enable

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) readonly buffer CullDataBuffer {
    gpuCullData_t cullData[];
};

layout(set = 0, binding = 1) buffer DrawCommandBuffer {
    gpuDrawCommand_t drawCommands[];
};

layout(set = 0, binding = 2) uniform CullUniforms {
    mat4 viewProjMatrix;
    vec4 frustumPlanes[6];
    vec3 viewPos;
    uint numDraws;
};

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= numDraws) return;
    
    // Frustum culling
    vec4 sphere = cullData[idx].boundingSphere;
    bool visible = true;
    
    for (int i = 0; i < 6; i++) {
        float dist = dot(frustumPlanes[i].xyz, sphere.xyz) + frustumPlanes[i].w;
        if (dist < -sphere.w) {
            visible = false;
            break;
        }
    }
    
    // Occlusion culling (HiZ test)
    if (visible) {
        // Project sphere to screen space
        vec4 projected = viewProjMatrix * vec4(sphere.xyz, 1.0);
        // HiZ test implementation...
    }
    
    cullData[idx].visible = visible ? 1 : 0;
    
    // Compact draw commands
    if (visible) {
        uint drawIdx = atomicAdd(drawCount, 1);
        compactedDraws[drawIdx] = drawCommands[cullData[idx].drawIndex];
    }
}
";
```

### Bindless Texture System
```c
// Bindless texture management
typedef struct vkBindlessTextures_s {
    VkDescriptorPool            descriptorPool;
    VkDescriptorSetLayout       setLayout;
    VkDescriptorSet             descriptorSet;
    
    // Texture array
    VkImageView                 *imageViews;
    VkSampler                   *samplers;
    uint32_t                    maxTextures;
    uint32_t                    numTextures;
    
    // Free list for texture slots
    uint32_t                    *freeSlots;
    uint32_t                    numFreeSlots;
} vkBindlessTextures_t;

void VK_InitBindlessTextures(void) {
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MAX_BINDLESS_TEXTURES,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = NULL
    };
    
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 1,
        .pBindingFlags = (VkDescriptorBindingFlags[]){
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
        }
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bindingFlags,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 1,
        .pBindings = &binding
    };
    
    vkCreateDescriptorSetLayout(vk_device, &layoutInfo, NULL, &bindlessTextures.setLayout);
}
```

## Implementation Requirements

### 1. Unified Pipeline State

```c
// Single pipeline for all materials
typedef struct vkUnifiedPipeline_s {
    VkPipeline                  pipeline;
    VkPipelineLayout            layout;
    
    // Descriptor sets
    VkDescriptorSetLayout       perFrameLayout;    // Set 0: Frame data
    VkDescriptorSetLayout       perViewLayout;     // Set 1: View data  
    VkDescriptorSetLayout       bindlessLayout;    // Set 2: Bindless textures
    VkDescriptorSetLayout       materialLayout;    // Set 3: Material data
    
    // Push constants for quick updates
    VkPushConstantRange         pushConstantRange;
} vkUnifiedPipeline_t;

// Per-frame descriptor set (Set 0)
typedef struct vkFrameData_s {
    VkBuffer        timeBuffer;         // Time, frame number, etc.
    VkBuffer        lightBuffer;        // All lights in scene
    VkBuffer        globalParamsBuffer; // Global material parameters
} vkFrameData_t;

// Per-view descriptor set (Set 1)  
typedef struct vkViewData_s {
    VkBuffer        viewMatrixBuffer;
    VkBuffer        projMatrixBuffer;
    VkBuffer        viewParamsBuffer;   // FOV, near/far, viewport
    VkBuffer        frustumBuffer;      // Frustum planes
} vkViewData_t;
```

### 2. Uber-Shader Architecture

```c
// File: src/engine/renderer/vulkan/vk_shader.h (NEW FILE)

// Uber-shader configuration passed via push constants
typedef struct uberShaderConfig_s {
    // Feature flags (32 bits)
    uint32_t    features;
    
    // Texture binding info
    uint32_t    textureFlags;
    
    // Lighting configuration
    uint32_t    lightingMode;
    
    // Reserved for future use
    uint32_t    reserved;
} uberShaderConfig_t;

// Feature flag bits
#define UBER_FEATURE_LIGHTMAP           0x00000001
#define UBER_FEATURE_VERTEX_COLOR       0x00000002
#define UBER_FEATURE_ALPHA_TEST         0x00000004
#define UBER_FEATURE_RGBGEN_WAVE        0x00000008
#define UBER_FEATURE_ALPHAGEN_WAVE      0x00000010
#define UBER_FEATURE_TCGEN_ENVIRONMENT  0x00000020
#define UBER_FEATURE_TCMOD_TRANSFORM    0x00000040
#define UBER_FEATURE_TCMOD_TURB         0x00000080
#define UBER_FEATURE_FOG                0x00000100
#define UBER_FEATURE_NORMALMAP          0x00000200
#define UBER_FEATURE_SPECULARMAP        0x00000400
#define UBER_FEATURE_GLOWMAP            0x00000800
#define UBER_FEATURE_DETAIL             0x00001000
#define UBER_FEATURE_MULTITEXTURE       0x00002000

// Lighting modes
typedef enum {
    LIGHTING_NONE = 0,
    LIGHTING_IDENTITY = 1,
    LIGHTING_VERTEX = 2,
    LIGHTING_DIFFUSE = 3,
    LIGHTING_SPECULAR = 4,
    LIGHTING_PBR = 5
} uberLightingMode_t;

// Push constant structure
typedef struct vkPushConstants_s {
    // Uber-shader configuration
    uberShaderConfig_t  config;
    
    // Transform matrices
    mat4_t              mvpMatrix;
    mat4_t              modelMatrix;
    mat4_t              normalMatrix;
    
    // Material parameters
    vec4_t              baseColor;
    vec4_t              specularColor;
    float               specularExponent;
    float               alphaTestValue;
    float               time;
    float               portalRange;
    
    // Texture coordinate modifications
    vec4_t              tcModParams[2];    // scroll, scale, rotate params
    
    // Wave parameters
    vec4_t              rgbWaveParams;     // frequency, amplitude, phase, bias
    vec4_t              alphaWaveParams;
    
    // Fog parameters
    vec4_t              fogColor;
    vec2_t              fogParams;         // density, range
    
    // Lighting parameters (for future phases)
    vec4_t              lightPosition;
    vec4_t              lightColor;
    float               lightRadius;
    
} vkPushConstants_t;
```

### 2. Unified Pipeline Creation

```c
// File: src/engine/renderer/vulkan/vk_pipeline.c (MODIFY)

typedef struct vkPipeline_s {
    VkPipeline          pipeline;
    VkPipelineLayout    layout;
    VkRenderPass        renderPass;
    
    // Pipeline state
    VkPrimitiveTopology topology;
    VkCullModeFlags     cullMode;
    VkBool32            depthTestEnable;
    VkBool32            depthWriteEnable;
    VkCompareOp         depthCompareOp;
    VkBool32            blendEnable;
    VkBlendFactor       srcColorBlendFactor;
    VkBlendFactor       dstColorBlendFactor;
    VkBlendOp           colorBlendOp;
    
} vkPipeline_t;

// Single uber-pipeline for base pass
static vkPipeline_t uberPipeline;
static vkPipeline_t shadowPipeline;
static vkPipeline_t postProcessPipeline;

void VK_CreateUberPipeline(void) {
    VkGraphicsPipelineCreateInfo pipelineInfo = {0};
    
    // Load uber-shaders
    VkShaderModule vertShader = VK_LoadShader("shaders/uber.vert.spv");
    VkShaderModule fragShader = VK_LoadShader("shaders/uber.frag.spv");
    
    // Shader stages
    VkPipelineShaderStageCreateInfo shaderStages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertShader,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragShader,
            .pName = "main"
        }
    };
    
    // Vertex input - support all possible attributes
    VkVertexInputBindingDescription vertexBinding = {
        .binding = 0,
        .stride = sizeof(drawVert_t),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    
    VkVertexInputAttributeDescription vertexAttribs[] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(drawVert_t, xyz) },      // position
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(drawVert_t, st) },         // texcoord0
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(drawVert_t, lightmap) },    // texcoord1
        { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(drawVert_t, normal) },   // normal
        { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(drawVert_t, tangent) }, // tangent
        { 5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(drawVert_t, color) },      // color
    };
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertexBinding,
        .vertexAttributeDescriptionCount = ARRAY_LEN(vertexAttribs),
        .pVertexAttributeDescriptions = vertexAttribs
    };
    
    // Dynamic state - we'll change these at runtime
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE
    };
    
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ARRAY_LEN(dynamicStates),
        .pDynamicStates = dynamicStates
    };
    
    // Create pipeline layout with push constants
    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(vkPushConstants_t)
    };
    
    VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = vk.numDescriptorSetLayouts,
        .pSetLayouts = vk.descriptorSetLayouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange
    };
    
    vkCreatePipelineLayout(vk.device, &layoutInfo, NULL, &uberPipeline.layout);
    
    // ... rest of pipeline creation ...
    
    pipelineInfo.layout = uberPipeline.layout;
    pipelineInfo.renderPass = vk.renderPass;
    
    vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, 
                             NULL, &uberPipeline.pipeline);
}
```

### 3. Uber-Shader GLSL Implementation

```glsl
// File: shaders/glsl/uber.vert

#version 450
#extension GL_ARB_separate_shader_objects : enable

// Push constants
layout(push_constant) uniform PushConstants {
    uvec4   config;         // features, textureFlags, lightingMode, reserved
    mat4    mvpMatrix;
    mat4    modelMatrix;
    mat4    normalMatrix;
    vec4    baseColor;
    vec4    tcModParams[2];
    vec4    rgbWaveParams;
    vec4    alphaWaveParams;
} pc;

// Vertex attributes
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord0;
layout(location = 2) in vec2 inTexCoord1;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in vec4 inColor;

// Outputs
layout(location = 0) out vec2 fragTexCoord0;
layout(location = 1) out vec2 fragTexCoord1;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec4 fragColor;
layout(location = 4) out vec3 fragWorldPos;
layout(location = 5) out vec3 fragTangent;
layout(location = 6) out vec3 fragBitangent;

// Feature test macros
#define HAS_FEATURE(f) ((pc.config.x & (f)) != 0u)
#define UBER_FEATURE_TCMOD_TRANSFORM 0x00000040u
#define UBER_FEATURE_NORMALMAP 0x00000200u

void main() {
    gl_Position = pc.mvpMatrix * vec4(inPosition, 1.0);
    
    // Pass through texture coordinates
    fragTexCoord0 = inTexCoord0;
    fragTexCoord1 = inTexCoord1;
    
    // Apply texture coordinate modifications if enabled
    if (HAS_FEATURE(UBER_FEATURE_TCMOD_TRANSFORM)) {
        vec2 tcMod = fragTexCoord0;
        // Apply scroll
        tcMod += pc.tcModParams[0].xy * pc.tcModParams[1].w; // time
        // Apply scale
        tcMod *= pc.tcModParams[0].zw;
        fragTexCoord0 = tcMod;
    }
    
    // Transform normal to world space
    fragNormal = normalize((pc.normalMatrix * vec4(inNormal, 0.0)).xyz);
    
    // Setup tangent space if using normal mapping
    if (HAS_FEATURE(UBER_FEATURE_NORMALMAP)) {
        fragTangent = normalize((pc.modelMatrix * vec4(inTangent.xyz, 0.0)).xyz);
        fragBitangent = cross(fragNormal, fragTangent) * inTangent.w;
    }
    
    // Pass through vertex color
    fragColor = inColor * pc.baseColor;
    
    // Calculate world position for lighting
    fragWorldPos = (pc.modelMatrix * vec4(inPosition, 1.0)).xyz;
}
```

```glsl
// File: shaders/glsl/uber.frag

#version 450
#extension GL_ARB_separate_shader_objects : enable

// Push constants (shared with vertex shader)
layout(push_constant) uniform PushConstants {
    uvec4   config;
    // ... matrices ...
    vec4    baseColor;
    vec4    specularColor;
    float   specularExponent;
    float   alphaTestValue;
    // ... other parameters ...
} pc;

// Texture samplers
layout(set = 0, binding = 0) uniform sampler2D colorMap;
layout(set = 0, binding = 1) uniform sampler2D lightMap;
layout(set = 0, binding = 2) uniform sampler2D normalMap;
layout(set = 0, binding = 3) uniform sampler2D specularMap;
layout(set = 0, binding = 4) uniform sampler2D glowMap;

// Inputs from vertex shader
layout(location = 0) in vec2 fragTexCoord0;
layout(location = 1) in vec2 fragTexCoord1;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in vec3 fragWorldPos;
layout(location = 5) in vec3 fragTangent;
layout(location = 6) in vec3 fragBitangent;

// Output
layout(location = 0) out vec4 outColor;

// Feature test macros
#define HAS_FEATURE(f) ((pc.config.x & (f)) != 0u)
#define UBER_FEATURE_LIGHTMAP 0x00000001u
#define UBER_FEATURE_VERTEX_COLOR 0x00000002u
#define UBER_FEATURE_ALPHA_TEST 0x00000004u
#define UBER_FEATURE_NORMALMAP 0x00000200u
#define UBER_FEATURE_SPECULARMAP 0x00000400u

void main() {
    vec4 color = texture(colorMap, fragTexCoord0);
    
    // Apply vertex color if enabled
    if (HAS_FEATURE(UBER_FEATURE_VERTEX_COLOR)) {
        color *= fragColor;
    } else {
        color *= pc.baseColor;
    }
    
    // Apply lightmap if enabled
    if (HAS_FEATURE(UBER_FEATURE_LIGHTMAP)) {
        vec3 lightmap = texture(lightMap, fragTexCoord1).rgb;
        color.rgb *= lightmap;
    }
    
    // Alpha test if enabled
    if (HAS_FEATURE(UBER_FEATURE_ALPHA_TEST)) {
        if (color.a < pc.alphaTestValue) {
            discard;
        }
    }
    
    // Normal mapping if enabled
    vec3 normal = fragNormal;
    if (HAS_FEATURE(UBER_FEATURE_NORMALMAP)) {
        vec3 normalMapSample = texture(normalMap, fragTexCoord0).rgb * 2.0 - 1.0;
        mat3 TBN = mat3(fragTangent, fragBitangent, fragNormal);
        normal = normalize(TBN * normalMapSample);
    }
    
    // Basic lighting calculation (will be enhanced in Phase 6)
    uint lightingMode = (pc.config.y & 0xFFu);
    if (lightingMode > 0u) {
        // Simple directional light for now
        vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
        float NdotL = max(dot(normal, lightDir), 0.0);
        color.rgb *= mix(0.5, 1.0, NdotL); // Simple ambient + diffuse
    }
    
    outColor = color;
}
```

### 4. Backend Material Processing

```c
// File: src/engine/renderer/vulkan/vk_shade.c (MODIFY)

void VK_SetupMaterialStage(material_t *material, int stageNum) {
    materialStage_t *stage = &material->stages[stageNum];
    vkPushConstants_t pc;
    
    Com_Memset(&pc, 0, sizeof(pc));
    
    // Setup feature flags based on stage
    if (stage->bundle[0].isLightmap) {
        pc.config.features |= UBER_FEATURE_LIGHTMAP;
    }
    
    if (stage->rgbGen == CGEN_VERTEX) {
        pc.config.features |= UBER_FEATURE_VERTEX_COLOR;
    }
    
    if (stage->stateBits & GLS_ALPHATEST_ENABLE) {
        pc.config.features |= UBER_FEATURE_ALPHA_TEST;
        pc.alphaTestValue = 0.5f; // Default, could be from material
    }
    
    if (stage->normalMap) {
        pc.config.features |= UBER_FEATURE_NORMALMAP;
    }
    
    if (stage->specularMap) {
        pc.config.features |= UBER_FEATURE_SPECULARMAP;
    }
    
    // Setup texture coordinate modifications
    if (stage->numTexMods > 0) {
        pc.config.features |= UBER_FEATURE_TCMOD_TRANSFORM;
        VK_SetupTexMods(stage, pc.tcModParams);
    }
    
    // Setup wave parameters
    if (stage->rgbGen == CGEN_WAVEFORM) {
        pc.config.features |= UBER_FEATURE_RGBGEN_WAVE;
        VK_SetupWave(&stage->rgbWave, &pc.rgbWaveParams);
    }
    
    if (stage->alphaGen == AGEN_WAVEFORM) {
        pc.config.features |= UBER_FEATURE_ALPHAGEN_WAVE;
        VK_SetupWave(&stage->alphaWave, &pc.alphaWaveParams);
    }
    
    // Setup lighting mode
    pc.config.lightingMode = VK_GetLightingMode(stage);
    
    // Setup colors
    Vector4Copy(stage->constantColor, pc.baseColor);
    Vector4Copy(material->specularColor, pc.specularColor);
    pc.specularExponent = material->specularExponent;
    
    // Setup matrices
    VK_SetupMatrices(&pc);
    
    // Update push constants
    vkCmdPushConstants(vk.commandBuffer, uberPipeline.layout,
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, sizeof(vkPushConstants_t), &pc);
}

void VK_DrawSurfaces(drawSurf_t **drawSurfs, int numSurfs) {
    material_t *currentMaterial = NULL;
    int currentEntity = -1;
    
    // Bind uber-pipeline once
    vkCmdBindPipeline(vk.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, 
                     uberPipeline.pipeline);
    
    for (int i = 0; i < numSurfs; i++) {
        drawSurf_t *surf = drawSurfs[i];
        material_t *material = (material_t*)surf->material;
        
        // Check for material change
        if (material != currentMaterial) {
            currentMaterial = material;
            
            // Process each stage
            for (int s = 0; s < material->numStages; s++) {
                VK_SetupMaterialStage(material, s);
                
                // Bind textures for this stage
                VK_BindStageTextures(&material->stages[s]);
                
                // Setup blend state if needed
                if (s > 0 || material->stages[s].blendSrc != -1) {
                    VK_SetupBlendState(&material->stages[s]);
                }
                
                // Draw the surface
                VK_DrawSurface(surf->surface);
            }
        }
        
        // Check for entity change
        if (surf->entityNum != currentEntity) {
            currentEntity = surf->entityNum;
            VK_SetupEntity(currentEntity);
        }
    }
}
```

### 5. Descriptor Set Management

```c
// File: src/engine/renderer/vulkan/vk_descriptors.c (NEW FILE)

typedef struct vkDescriptorManager_s {
    VkDescriptorPool            pool;
    VkDescriptorSetLayout       textureLayout;
    VkDescriptorSetLayout       uniformLayout;
    
    // Descriptor set cache
    VkDescriptorSet            *textureSets;
    int                         numTextureSets;
    int                         maxTextureSets;
    
} vkDescriptorManager_t;

static vkDescriptorManager_t descriptorManager;

void VK_InitDescriptorManager(void) {
    // Create descriptor pool
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 }
    };
    
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1100,
        .poolSizeCount = ARRAY_LEN(poolSizes),
        .pPoolSizes = poolSizes
    };
    
    vkCreateDescriptorPool(vk.device, &poolInfo, NULL, &descriptorManager.pool);
    
    // Create texture descriptor set layout
    VkDescriptorSetLayoutBinding textureBindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_LEN(textureBindings),
        .pBindings = textureBindings
    };
    
    vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL, 
                               &descriptorManager.textureLayout);
}

VkDescriptorSet VK_GetTextureDescriptorSet(materialStage_t *stage) {
    // Hash stage textures to find cached set
    uint32_t hash = VK_HashStageTextures(stage);
    
    // Look for existing set
    for (int i = 0; i < descriptorManager.numTextureSets; i++) {
        if (descriptorManager.textureSetHashes[i] == hash) {
            return descriptorManager.textureSets[i];
        }
    }
    
    // Create new descriptor set
    VkDescriptorSet set = VK_AllocateDescriptorSet();
    
    // Update with stage textures
    VkDescriptorImageInfo imageInfos[5] = {0};
    VkWriteDescriptorSet writes[5] = {0};
    int numWrites = 0;
    
    // Color map
    if (stage->bundle[0].image[0]) {
        imageInfos[numWrites] = (VkDescriptorImageInfo){
            .sampler = VK_GetSampler(stage->bundle[0].image[0]),
            .imageView = VK_GetImageView(stage->bundle[0].image[0]),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        writes[numWrites] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfos[numWrites]
        };
        numWrites++;
    }
    
    // Lightmap
    if (stage->bundle[1].image[0]) {
        // Similar setup for binding 1
    }
    
    // Normal map
    if (stage->normalMap) {
        // Similar setup for binding 2
    }
    
    vkUpdateDescriptorSets(vk.device, numWrites, writes, 0, NULL);
    
    // Cache the set
    VK_CacheDescriptorSet(set, hash);
    
    return set;
}
```

### 6. Performance Optimizations

```c
// File: src/engine/renderer/vulkan/vk_batch.c (NEW FILE)

// Indirect draw for batching
typedef struct vkDrawBatch_s {
    VkDrawIndexedIndirectCommand   cmd;
    uint32_t                        materialId;
    uint32_t                        entityId;
} vkDrawBatch_t;

static vkDrawBatch_t drawBatches[MAX_DRAW_BATCHES];
static int numDrawBatches;

void VK_BatchDrawSurfaces(drawSurf_t **drawSurfs, int numSurfs) {
    numDrawBatches = 0;
    
    // Build batches
    for (int i = 0; i < numSurfs; i++) {
        drawSurf_t *surf = drawSurfs[i];
        
        // Check if we can batch with previous
        if (numDrawBatches > 0) {
            vkDrawBatch_t *lastBatch = &drawBatches[numDrawBatches - 1];
            
            if (lastBatch->materialId == surf->material->index &&
                lastBatch->entityId == surf->entityNum) {
                // Extend batch
                lastBatch->cmd.indexCount += surf->numIndexes;
                continue;
            }
        }
        
        // Start new batch
        vkDrawBatch_t *batch = &drawBatches[numDrawBatches++];
        batch->cmd.indexCount = surf->numIndexes;
        batch->cmd.instanceCount = 1;
        batch->cmd.firstIndex = surf->firstIndex;
        batch->cmd.vertexOffset = surf->firstVertex;
        batch->cmd.firstInstance = 0;
        batch->materialId = surf->material->index;
        batch->entityId = surf->entityNum;
    }
    
    // Execute batches
    VK_ExecuteBatches();
}

void VK_ExecuteBatches(void) {
    uint32_t currentMaterial = -1;
    uint32_t currentEntity = -1;
    
    for (int i = 0; i < numDrawBatches; i++) {
        vkDrawBatch_t *batch = &drawBatches[i];
        
        // Update state if changed
        if (batch->materialId != currentMaterial) {
            VK_SetupMaterial(batch->materialId);
            currentMaterial = batch->materialId;
        }
        
        if (batch->entityId != currentEntity) {
            VK_SetupEntity(batch->entityId);
            currentEntity = batch->entityId;
        }
        
        // Draw
        vkCmdDrawIndexedIndirect(vk.commandBuffer, vk.indirectBuffer,
                                i * sizeof(VkDrawIndexedIndirectCommand), 
                                1, sizeof(VkDrawIndexedIndirectCommand));
    }
}
```

## Shader Compilation Pipeline

### GLSL to SPIR-V Compilation

```bash
# Compile script: compile_shaders.sh
#!/bin/bash

GLSLC=glslangValidator
OUTDIR=shaders/spirv

# Compile uber-shaders
$GLSLC -V shaders/glsl/uber.vert -o $OUTDIR/uber.vert.spv
$GLSLC -V shaders/glsl/uber.frag -o $OUTDIR/uber.frag.spv

# Compile specialized shaders
$GLSLC -V shaders/glsl/shadow.vert -o $OUTDIR/shadow.vert.spv
$GLSLC -V shaders/glsl/shadow.frag -o $OUTDIR/shadow.frag.spv

$GLSLC -V shaders/glsl/post.vert -o $OUTDIR/post.vert.spv
$GLSLC -V shaders/glsl/post.frag -o $OUTDIR/post.frag.spv
```

## Memory Management

### Buffer Management

```c
// File: src/engine/renderer/vulkan/vk_buffers.c (MODIFY)

typedef struct vkBufferManager_s {
    // Vertex/Index buffers
    VkBuffer        vertexBuffer;
    VkBuffer        indexBuffer;
    VkDeviceMemory  vertexMemory;
    VkDeviceMemory  indexMemory;
    
    // Staging buffers for uploads
    VkBuffer        stagingBuffer;
    VkDeviceMemory  stagingMemory;
    void*           stagingMapped;
    
    // Uniform buffers
    VkBuffer        uniformBuffers[MAX_FRAMES_IN_FLIGHT];
    VkDeviceMemory  uniformMemory[MAX_FRAMES_IN_FLIGHT];
    void*           uniformMapped[MAX_FRAMES_IN_FLIGHT];
    
    // Indirect draw buffer
    VkBuffer        indirectBuffer;
    VkDeviceMemory  indirectMemory;
    void*           indirectMapped;
    
} vkBufferManager_t;

void VK_UpdateVertexBuffer(const void *data, size_t size, size_t offset) {
    // Copy to staging buffer
    memcpy((byte*)bufferManager.stagingMapped + offset, data, size);
    
    // Record copy command
    VkBufferCopy copyRegion = {
        .srcOffset = offset,
        .dstOffset = offset,
        .size = size
    };
    
    vkCmdCopyBuffer(vk.commandBuffer, bufferManager.stagingBuffer,
                   bufferManager.vertexBuffer, 1, &copyRegion);
    
    // Memory barrier
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
    };
    
    vkCmdPipelineBarrier(vk.commandBuffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                        0, 1, &barrier, 0, NULL, 0, NULL);
}
```

## Testing and Validation

### Validation Layers

```c
void VK_SetupValidation(void) {
    const char *validationLayers[] = {
        "VK_LAYER_KHRONOS_validation"
    };
    
    // Enable validation in debug builds
#ifdef DEBUG
    VkInstanceCreateInfo createInfo = {
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = validationLayers
    };
#endif
    
    // Setup debug messenger
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = VK_DebugCallback
    };
}

VKAPI_ATTR VkBool32 VKAPI_CALL VK_DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {
    
    ri.Printf(PRINT_WARNING, "Vulkan: %s\n", pCallbackData->pMessage);
    return VK_FALSE;
}
```

## Performance Metrics

```c
typedef struct vkPerfStats_s {
    uint32_t    numPipelineChanges;
    uint32_t    numDescriptorSetChanges;
    uint32_t    numPushConstantUpdates;
    uint32_t    numDrawCalls;
    uint32_t    numTriangles;
    double      gpuTime;
} vkPerfStats_t;

void VK_GatherPerformanceStats(vkPerfStats_t *stats) {
    // Query GPU timestamps
    vkCmdWriteTimestamp(vk.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       vk.queryPool, 0);
    
    // ... rendering ...
    
    vkCmdWriteTimestamp(vk.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       vk.queryPool, 1);
    
    // Get results
    uint64_t timestamps[2];
    vkGetQueryPoolResults(vk.device, vk.queryPool, 0, 2,
                         sizeof(timestamps), timestamps, sizeof(uint64_t),
                         VK_QUERY_RESULT_64_BIT);
    
    stats->gpuTime = (timestamps[1] - timestamps[0]) * vk.timestampPeriod / 1000000.0;
}
```

## Success Criteria

Phase 4 is complete when:

1. ✓ Uber-shader pipeline implemented
2. ✓ All material features supported
3. ✓ Single pipeline for base pass
4. ✓ Push constants properly configured
5. ✓ Descriptor sets managed efficiently
6. ✓ No visual regressions
7. ✓ Performance improved
8. ✓ Vulkan validation clean

## Dependencies for Next Phases

### Phase 5 (Dynamic Lighting) Requirements
- Push constant space for light parameters
- Multiple draw passes support

### Phase 6 (Additive Lighting) Requirements
- Blend state configuration
- Multi-pass rendering infrastructure

This unified Vulkan backend provides the GPU infrastructure needed for advanced rendering features while significantly reducing pipeline complexity and state changes.