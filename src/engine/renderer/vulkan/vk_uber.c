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

#include "../tr_local.h"
#include "vk.h"
#include "vk_shader.h"

/*
================================================================================
Vulkan Uber-Shader Implementation

This file implements the unified shader system that replaces multiple pipeline
permutations with a single flexible uber-shader.
================================================================================
*/

// Global pipelines
vkPipeline_t *vk_uberPipeline = NULL;
vkPipeline_t *vk_shadowPipeline = NULL;
vkPipeline_t *vk_postProcessPipeline = NULL;
vkPipeline_t *vk_skyboxPipeline = NULL;

// Shader modules
VkShaderModule vk_uberVertexShader = VK_NULL_HANDLE;
VkShaderModule vk_uberFragmentShader = VK_NULL_HANDLE;

// Pipeline cache for different blend states
#define MAX_PIPELINE_CACHE 16
static vkPipeline_t pipelineCache[MAX_PIPELINE_CACHE];
static int numCachedPipelines = 0;

/*
================
VK_InitUberShaderSystem

Initialize the uber-shader system
================
*/
void VK_InitUberShaderSystem(void) {
    vkPipelineState_t defaultState;
    
    ri.Printf(PRINT_ALL, "Initializing Vulkan uber-shader system...\n");
    
    // Load shader modules
    vk_uberVertexShader = VK_LoadShaderModule("shaders/uber.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
    vk_uberFragmentShader = VK_LoadShaderModule("shaders/uber.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
    
    if (!vk_uberVertexShader || !vk_uberFragmentShader) {
        ri.Error(ERR_FATAL, "Failed to load uber-shader modules");
    }
    
    // Create main uber-pipeline with default state
    Com_Memset(&defaultState, 0, sizeof(defaultState));
    defaultState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    defaultState.cullMode = VK_CULL_MODE_BACK_BIT;
    defaultState.depthTestEnable = VK_TRUE;
    defaultState.depthWriteEnable = VK_TRUE;
    defaultState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    defaultState.blendEnable = VK_FALSE;
    defaultState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    defaultState.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    defaultState.colorBlendOp = VK_BLEND_OP_ADD;
    defaultState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    defaultState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    defaultState.alphaBlendOp = VK_BLEND_OP_ADD;
    
    vk_uberPipeline = VK_CreateUberPipeline("uber_main", &defaultState);
    
    // Create shadow pipeline
    defaultState.cullMode = VK_CULL_MODE_NONE;
    defaultState.depthWriteEnable = VK_FALSE;
    vk_shadowPipeline = VK_CreateUberPipeline("uber_shadow", &defaultState);
    
    // Create post-process pipeline
    defaultState.depthTestEnable = VK_FALSE;
    defaultState.depthWriteEnable = VK_FALSE;
    vk_postProcessPipeline = VK_CreateUberPipeline("uber_post", &defaultState);
    
    ri.Printf(PRINT_ALL, "Uber-shader system initialized\n");
}

/*
================
VK_ShutdownUberShaderSystem

Shutdown the uber-shader system
================
*/
void VK_ShutdownUberShaderSystem(void) {
    int i;
    
    // Destroy pipelines
    if (vk_uberPipeline) {
        VK_DestroyPipeline(vk_uberPipeline);
        vk_uberPipeline = NULL;
    }
    
    if (vk_shadowPipeline) {
        VK_DestroyPipeline(vk_shadowPipeline);
        vk_shadowPipeline = NULL;
    }
    
    if (vk_postProcessPipeline) {
        VK_DestroyPipeline(vk_postProcessPipeline);
        vk_postProcessPipeline = NULL;
    }
    
    if (vk_skyboxPipeline) {
        VK_DestroyPipeline(vk_skyboxPipeline);
        vk_skyboxPipeline = NULL;
    }
    
    // Destroy cached pipelines
    for (i = 0; i < numCachedPipelines; i++) {
        if (pipelineCache[i].pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk.device, pipelineCache[i].pipeline, NULL);
        }
        if (pipelineCache[i].layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk.device, pipelineCache[i].layout, NULL);
        }
    }
    numCachedPipelines = 0;
    
    // Destroy shader modules
    if (vk_uberVertexShader) {
        VK_DestroyShaderModule(vk_uberVertexShader);
        vk_uberVertexShader = VK_NULL_HANDLE;
    }
    
    if (vk_uberFragmentShader) {
        VK_DestroyShaderModule(vk_uberFragmentShader);
        vk_uberFragmentShader = VK_NULL_HANDLE;
    }
}

/*
================
VK_CreateUberPipeline

Create a pipeline with the uber-shader
================
*/
vkPipeline_t* VK_CreateUberPipeline(const char *name, vkPipelineState_t *state) {
    VkGraphicsPipelineCreateInfo pipelineInfo;
    VkPipelineShaderStageCreateInfo shaderStages[2];
    VkPipelineVertexInputStateCreateInfo vertexInputInfo;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly;
    VkPipelineViewportStateCreateInfo viewportState;
    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineMultisampleStateCreateInfo multisampling;
    VkPipelineDepthStencilStateCreateInfo depthStencil;
    VkPipelineColorBlendStateCreateInfo colorBlending;
    VkPipelineColorBlendAttachmentState colorBlendAttachment;
    VkPipelineDynamicStateCreateInfo dynamicState;
    VkPipelineLayoutCreateInfo pipelineLayoutInfo;
    VkPushConstantRange pushConstantRange;
    vkPipeline_t *pipeline;
    VkResult result;
    
    // Vertex input binding
    VkVertexInputBindingDescription vertexBinding = {
        .binding = 0,
        .stride = sizeof(vkVertex_t),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    
    // Vertex input attributes
    VkVertexInputAttributeDescription vertexAttribs[] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vkVertex_t, position) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(vkVertex_t, texCoord0) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(vkVertex_t, texCoord1) },
        { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vkVertex_t, normal) },
        { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vkVertex_t, tangent) },
        { 5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(vkVertex_t, color) }
    };
    
    // Dynamic states
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE
    };
    
    // Allocate pipeline structure
    if (numCachedPipelines >= MAX_PIPELINE_CACHE) {
        ri.Error(ERR_FATAL, "Pipeline cache overflow");
    }
    pipeline = &pipelineCache[numCachedPipelines++];
    Com_Memset(pipeline, 0, sizeof(vkPipeline_t));
    Q_strncpyz(pipeline->name, name, sizeof(pipeline->name));
    Com_Memcpy(&pipeline->state, state, sizeof(vkPipelineState_t));
    
    // Shader stages
    Com_Memset(shaderStages, 0, sizeof(shaderStages));
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vk_uberVertexShader;
    shaderStages[0].pName = "main";
    
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = vk_uberFragmentShader;
    shaderStages[1].pName = "main";
    
    // Vertex input
    Com_Memset(&vertexInputInfo, 0, sizeof(vertexInputInfo));
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &vertexBinding;
    vertexInputInfo.vertexAttributeDescriptionCount = ARRAY_LEN(vertexAttribs);
    vertexInputInfo.pVertexAttributeDescriptions = vertexAttribs;
    
    // Input assembly
    Com_Memset(&inputAssembly, 0, sizeof(inputAssembly));
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = state->topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // Viewport state
    Com_Memset(&viewportState, 0, sizeof(viewportState));
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    
    // Rasterizer
    Com_Memset(&rasterizer, 0, sizeof(rasterizer));
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = state->cullMode;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // Multisampling
    Com_Memset(&multisampling, 0, sizeof(multisampling));
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Depth stencil
    Com_Memset(&depthStencil, 0, sizeof(depthStencil));
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = state->depthTestEnable;
    depthStencil.depthWriteEnable = state->depthWriteEnable;
    depthStencil.depthCompareOp = state->depthCompareOp;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    
    // Color blend attachment
    Com_Memset(&colorBlendAttachment, 0, sizeof(colorBlendAttachment));
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | 
                                         VK_COLOR_COMPONENT_G_BIT | 
                                         VK_COLOR_COMPONENT_B_BIT | 
                                         VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = state->blendEnable;
    colorBlendAttachment.srcColorBlendFactor = state->srcColorBlendFactor;
    colorBlendAttachment.dstColorBlendFactor = state->dstColorBlendFactor;
    colorBlendAttachment.colorBlendOp = state->colorBlendOp;
    colorBlendAttachment.srcAlphaBlendFactor = state->srcAlphaBlendFactor;
    colorBlendAttachment.dstAlphaBlendFactor = state->dstAlphaBlendFactor;
    colorBlendAttachment.alphaBlendOp = state->alphaBlendOp;
    
    // Color blending
    Com_Memset(&colorBlending, 0, sizeof(colorBlending));
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    // Dynamic state
    Com_Memset(&dynamicState, 0, sizeof(dynamicState));
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = ARRAY_LEN(dynamicStates);
    dynamicState.pDynamicStates = dynamicStates;
    
    // Push constant range
    Com_Memset(&pushConstantRange, 0, sizeof(pushConstantRange));
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(vkPushConstants_t);
    
    // Pipeline layout
    VkDescriptorSetLayout descriptorSetLayouts[3];
    uint32_t numDescriptorSetLayouts = 0;
    
    // Add descriptor set layouts as needed
    descriptorSetLayouts[numDescriptorSetLayouts++] = vk.set_layout_sampler;
    if (vk.set_layout_uniform != VK_NULL_HANDLE) {
        descriptorSetLayouts[numDescriptorSetLayouts++] = vk.set_layout_uniform;
    }
    if (vk.set_layout_storage != VK_NULL_HANDLE) {
        descriptorSetLayouts[numDescriptorSetLayouts++] = vk.set_layout_storage;
    }
    
    Com_Memset(&pipelineLayoutInfo, 0, sizeof(pipelineLayoutInfo));
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = numDescriptorSetLayouts;
    pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    
    result = vkCreatePipelineLayout(vk.device, &pipelineLayoutInfo, NULL, &pipeline->layout);
    if (result != VK_SUCCESS) {
        ri.Error(ERR_FATAL, "Failed to create pipeline layout: %d", result);
    }
    
    // Create graphics pipeline
    Com_Memset(&pipelineInfo, 0, sizeof(pipelineInfo));
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipeline->layout;
    pipelineInfo.renderPass = vk.render_pass.main;
    pipelineInfo.subpass = 0;
    
    result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, 
                                      &pipelineInfo, NULL, &pipeline->pipeline);
    if (result != VK_SUCCESS) {
        ri.Error(ERR_FATAL, "Failed to create graphics pipeline: %d", result);
    }
    
    pipeline->renderPass = vk.render_pass.main;
    
    ri.Printf(PRINT_ALL, "Created uber-pipeline: %s\n", name);
    return pipeline;
}

/*
================
VK_DestroyPipeline

Destroy a pipeline
================
*/
void VK_DestroyPipeline(vkPipeline_t *pipeline) {
    if (!pipeline) {
        return;
    }
    
    if (pipeline->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk.device, pipeline->pipeline, NULL);
        pipeline->pipeline = VK_NULL_HANDLE;
    }
    
    if (pipeline->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk.device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
    }
}

/*
================
VK_BindPipeline

Bind a pipeline to the command buffer
================
*/
void VK_BindPipeline(vkPipeline_t *pipeline) {
    if (!pipeline || !pipeline->pipeline) {
        return;
    }
    
    vkCmdBindPipeline(vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
}

/*
================
VK_LoadShaderModule

Load a SPIR-V shader module
================
*/
VkShaderModule VK_LoadShaderModule(const char *filename, VkShaderStageFlagBits stage) {
    VkShaderModuleCreateInfo createInfo;
    VkShaderModule module;
    void *code;
    int codeSize;
    VkResult result;
    
    // Load SPIR-V file
    codeSize = ri.FS_ReadFile(filename, &code);
    if (codeSize <= 0) {
        ri.Printf(PRINT_WARNING, "Failed to load shader: %s\n", filename);
        return VK_NULL_HANDLE;
    }
    
    // Create shader module
    Com_Memset(&createInfo, 0, sizeof(createInfo));
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = codeSize;
    createInfo.pCode = (const uint32_t*)code;
    
    result = vkCreateShaderModule(vk.device, &createInfo, NULL, &module);
    ri.FS_FreeFile(code);
    
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "Failed to create shader module: %s\n", filename);
        return VK_NULL_HANDLE;
    }
    
    return module;
}

/*
================
VK_DestroyShaderModule

Destroy a shader module
================
*/
void VK_DestroyShaderModule(VkShaderModule module) {
    if (module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(vk.device, module, NULL);
    }
}

/*
================
VK_ConfigureUberShader

Configure uber-shader features based on material stage
================
*/
void VK_ConfigureUberShader(const materialStage_t *stage, uberShaderConfig_t *config) {
    Com_Memset(config, 0, sizeof(uberShaderConfig_t));
    
    // Set feature flags
    if (stage->bundle[0].isLightmap || stage->bundle[1].isLightmap) {
        config->features |= UBER_FEATURE_LIGHTMAP;
    }
    
    if (stage->rgbGen == CGEN_VERTEX) {
        config->features |= UBER_FEATURE_VERTEX_COLOR;
    }
    
    if (stage->stateBits & GLS_ALPHATEST_ENABLE) {
        config->features |= UBER_FEATURE_ALPHA_TEST;
    }
    
    if (stage->rgbGen == CGEN_WAVEFORM) {
        config->features |= UBER_FEATURE_RGBGEN_WAVE;
    }
    
    if (stage->alphaGen == AGEN_WAVEFORM) {
        config->features |= UBER_FEATURE_ALPHAGEN_WAVE;
    }
    
    if (stage->bundle[0].tcGen == TCGEN_ENVIRONMENT_MAPPED) {
        config->features |= UBER_FEATURE_TCGEN_ENVIRONMENT;
    }
    
    if (stage->numTexMods > 0) {
        config->features |= UBER_FEATURE_TCMOD_TRANSFORM;
    }
    
    if (stage->normalMap) {
        config->features |= UBER_FEATURE_NORMALMAP;
    }
    
    if (stage->specularMap) {
        config->features |= UBER_FEATURE_SPECULARMAP;
    }
    
    if (stage->glowMap) {
        config->features |= UBER_FEATURE_GLOWMAP;
    }
    
    if (stage->isDetail) {
        config->features |= UBER_FEATURE_DETAIL;
    }
    
    // Set texture flags
    if (stage->bundle[0].image[0]) {
        config->textureFlags |= TEXTURE_FLAG_DIFFUSE;
    }
    
    if (stage->bundle[1].image[0]) {
        config->textureFlags |= TEXTURE_FLAG_LIGHTMAP;
    }
    
    if (stage->normalMap) {
        config->textureFlags |= TEXTURE_FLAG_NORMAL;
    }
    
    if (stage->specularMap) {
        config->textureFlags |= TEXTURE_FLAG_SPECULAR;
    }
    
    // Set lighting mode
    config->lightingMode = VK_GetLightingMode(stage);
}

/*
================
VK_GetLightingMode

Determine lighting mode from material stage
================
*/
uint32_t VK_GetLightingMode(const materialStage_t *stage) {
    if (!stage->active) {
        return LIGHTING_NONE;
    }
    
    if (stage->rgbGen == CGEN_IDENTITY) {
        return LIGHTING_IDENTITY;
    }
    
    if (stage->rgbGen == CGEN_VERTEX || stage->rgbGen == CGEN_EXACT_VERTEX) {
        return LIGHTING_VERTEX;
    }
    
    if (stage->rgbGen == CGEN_LIGHTING_DIFFUSE) {
        return LIGHTING_DIFFUSE;
    }
    
    if (stage->lighting == SL_SPECULAR) {
        return LIGHTING_SPECULAR;
    }
    
    return LIGHTING_NONE;
}

/*
================
VK_SetupMaterialPushConstants

Setup push constants for a material stage
================
*/
void VK_SetupMaterialPushConstants(const material_t *material, int stageNum, vkPushConstants_t *pc) {
    const materialStage_t *stage;
    int i;
    
    if (!material || stageNum >= material->numStages) {
        return;
    }
    
    stage = &material->stages[stageNum];
    Com_Memset(pc, 0, sizeof(vkPushConstants_t));
    
    // Configure uber-shader
    VK_ConfigureUberShader(stage, &pc->config);
    
    // Setup matrices (would come from backend state)
    // TODO: Calculate MVP matrix from view and projection matrices
    // Com_Memcpy(pc->mvpMatrix, backEnd.viewParms.modelViewProjectionMatrix, sizeof(mat4_t));
    // Com_Memcpy(pc->modelMatrix, backEnd.or.modelMatrix, sizeof(mat4_t));
    
    // Calculate normal matrix (transpose of inverse model matrix)
    // Simplified: just copy model matrix for now
    // Com_Memcpy(pc->normalMatrix, backEnd.or.modelMatrix, sizeof(mat4_t));
    
    // Material colors
    for (i = 0; i < 4; i++) {
        pc->baseColor[i] = stage->constantColor[i] / 255.0f;
    }
    
    if (material->specularColor) {
        VectorCopy(material->specularColor, pc->specularColor);
        pc->specularColor[3] = 1.0f;
    } else {
        VectorSet(pc->specularColor, 1.0f, 1.0f, 1.0f);
        pc->specularColor[3] = 1.0f;
    }
    
    pc->specularExponent = material->specularExponent;
    pc->alphaTestValue = 0.5f;  // Default alpha test threshold
    pc->currentTime = backEnd.refdef.floatTime;
    pc->portalRange = material->portalRange;
    
    // Setup texture coordinate modifications
    if (stage->numTexMods > 0) {
        for (i = 0; i < stage->numTexMods && i < 2; i++) {
            const texModInfo_t *texMod = &stage->texMods[i];
            
            switch (texMod->type) {
            case TMOD_SCROLL:
                pc->tcModParams[0][0] = texMod->scroll[0];
                pc->tcModParams[0][1] = texMod->scroll[1];
                break;
            case TMOD_SCALE:
                pc->tcModParams[0][2] = texMod->scale[0];
                pc->tcModParams[0][3] = texMod->scale[1];
                break;
            case TMOD_ROTATE:
                pc->tcModParams[1][0] = texMod->rotateSpeed;
                break;
            case TMOD_TURBULENT:
                pc->tcModParams[1][1] = texMod->wave.amplitude;
                pc->tcModParams[1][2] = texMod->wave.frequency;
                pc->tcModParams[1][3] = texMod->wave.phase;
                break;
            }
        }
        pc->tcModParams[1][3] = pc->currentTime;  // Time in w component
    }
    
    // Setup wave parameters
    if (stage->rgbGen == CGEN_WAVEFORM) {
        VK_SetupWaveParams(&stage->rgbWave, pc->rgbWaveParams);
    }
    
    if (stage->alphaGen == AGEN_WAVEFORM) {
        VK_SetupWaveParams(&stage->alphaWave, pc->alphaWaveParams);
    }
    
    // Fog parameters (if applicable)
    if (tess.fogNum) {
        fog_t *fog = tr.world->fogs + tess.fogNum;
        VectorCopy(fog->color, pc->fogColor);
        pc->fogColor[3] = 1.0f;
        pc->fogParams[0] = 1.0f;  // TODO: Calculate proper fog density
        pc->fogParams[1] = fog->parms.depthForOpaque;
    }
}

/*
================
VK_SetupWaveParams

Setup wave parameters for push constants
================
*/
void VK_SetupWaveParams(const waveForm_t *wave, vec4_t params) {
    params[0] = wave->frequency;
    params[1] = wave->amplitude;
    params[2] = wave->phase;
    params[3] = wave->base;
}

/*
================
VK_UpdatePushConstants

Update push constants in command buffer
================
*/
void VK_UpdatePushConstants(const vkPushConstants_t *pc) {
    vkCmdPushConstants(vk.cmd->command_buffer, 
                      vk_uberPipeline->layout,
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, 
                      sizeof(vkPushConstants_t), 
                      pc);
}