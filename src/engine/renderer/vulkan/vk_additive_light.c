/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

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

#include "vk.h"
#include "../core/tr_local.h"
#include "../tr_light.h"
#include "../tr_interaction.h"
#include "../lighting/tr_light_dynamic.h"

/*
================================================================================
DOOM 3 BFG-Style Additive Multi-Pass Lighting System

This implements the interaction-based lighting inspired by DOOM 3 BFG where:
1. Base pass renders ambient/emissive
2. Each light adds its contribution via additive blending
3. Light-surface interactions are pre-calculated for efficiency
4. Scissor rectangles minimize overdraw
================================================================================
*/

typedef struct {
    // Lighting pipelines
    VkPipeline          ambientPipeline;       // Base ambient pass
    VkPipeline          interactionPipeline;   // Per-light additive pass
    VkPipeline          shadowedPipeline;      // Interaction with shadows
    VkPipelineLayout    lightingPipelineLayout;
    
    // Descriptor sets
    VkDescriptorSetLayout lightDescriptorLayout;
    VkDescriptorPool    lightDescriptorPool;
    VkDescriptorSet     lightDescriptorSets[MAX_RENDER_LIGHTS];
    
    // Light uniform buffers
    VkBuffer            lightUniformBuffer;
    VkDeviceMemory      lightUniformMemory;
    void*               lightUniformMapped;
    
    // Render state
    qboolean            additiveMode;
    int                 currentLight;
    
    // Statistics
    int                 numLightPasses;
    int                 numInteractionsDrawn;
    double              lightingTime;
} additiveLightingSystem_t;

static additiveLightingSystem_t vk_additiveLighting;

// Light uniform data
typedef struct {
    vec4_t  lightPosition;      // xyz = position, w = radius
    vec4_t  lightColor;         // rgb = color, a = intensity
    vec4_t  lightDirection;     // For spot lights
    mat4_t  lightMatrix;        // Light space transform
    vec4_t  attenuation;        // Constant, linear, quadratic, cutoff
    vec4_t  spotParams;         // Inner/outer cone angles
} lightUniforms_t;

/*
===============
VK_InitAdditiveLighting

Initialize the additive lighting system
===============
*/
void VK_InitAdditiveLighting(void) {
    Com_Memset(&vk_additiveLighting, 0, sizeof(vk_additiveLighting));
    
    // Create lighting pipelines
    VK_CreateAmbientPipeline();
    VK_CreateInteractionPipeline();
    VK_CreateShadowedInteractionPipeline();
    
    // Create descriptor set layout for lights
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
        }
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_LEN(bindings),
        .pBindings = bindings
    };
    
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, NULL,
                                        &vk_additiveLighting.lightDescriptorLayout));
    
    // Allocate light uniform buffer
    VkDeviceSize bufferSize = sizeof(lightUniforms_t) * MAX_RENDER_LIGHTS;
    
    VK_CreateBuffer(bufferSize,
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &vk_additiveLighting.lightUniformBuffer,
                   &vk_additiveLighting.lightUniformMemory);
    
    vkMapMemory(vk.device, vk_additiveLighting.lightUniformMemory, 0, bufferSize, 0,
                &vk_additiveLighting.lightUniformMapped);
    
    ri.Printf(PRINT_ALL, "Additive lighting system initialized\n");
}

/*
===============
VK_ShutdownAdditiveLighting

Cleanup additive lighting resources
===============
*/
void VK_ShutdownAdditiveLighting(void) {
    if (vk_additiveLighting.lightUniformMapped) {
        vkUnmapMemory(vk.device, vk_additiveLighting.lightUniformMemory);
    }
    
    if (vk_additiveLighting.lightUniformBuffer) {
        vkDestroyBuffer(vk.device, vk_additiveLighting.lightUniformBuffer, NULL);
    }
    
    if (vk_additiveLighting.lightUniformMemory) {
        vkFreeMemory(vk.device, vk_additiveLighting.lightUniformMemory, NULL);
    }
    
    if (vk_additiveLighting.ambientPipeline) {
        vkDestroyPipeline(vk.device, vk_additiveLighting.ambientPipeline, NULL);
    }
    
    if (vk_additiveLighting.interactionPipeline) {
        vkDestroyPipeline(vk.device, vk_additiveLighting.interactionPipeline, NULL);
    }
    
    if (vk_additiveLighting.shadowedPipeline) {
        vkDestroyPipeline(vk.device, vk_additiveLighting.shadowedPipeline, NULL);
    }
    
    Com_Memset(&vk_additiveLighting, 0, sizeof(vk_additiveLighting));
}

/*
===============
VK_BeginLightingPass

Start the lighting pass - render ambient/base
===============
*/
void VK_BeginLightingPass(void) {
    VkCommandBuffer cmd = vk.cmd;
    
    vk_additiveLighting.numLightPasses = 0;
    vk_additiveLighting.numInteractionsDrawn = 0;
    vk_additiveLighting.lightingTime = ri.Milliseconds();
    
    // Bind ambient pipeline for base pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      vk_additiveLighting.ambientPipeline);
    
    // Set depth test, no depth write for subsequent passes
    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE, // Write depth in ambient pass
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL
    };
}

/*
===============
VK_RenderAmbientPass

Render base ambient/emissive pass
===============
*/
void VK_RenderAmbientPass(drawSurf_t **drawSurfs, int numDrawSurfs) {
    VkCommandBuffer cmd = vk.cmd;
    
    // Render all surfaces with ambient contribution
    for (int i = 0; i < numDrawSurfs; i++) {
        drawSurf_t *surf = drawSurfs[i];
        
        // Skip purely additive surfaces
        if (surf->shader && (surf->shader->stages[0]->stateBits & GLS_SRCBLEND_ONE)) {
            continue;
        }
        
        // Bind surface data
        VK_BindSurfaceDescriptors(surf);
        
        // Set model matrix
        vkCmdPushConstants(cmd, vk_additiveLighting.lightingPipelineLayout,
                          VK_SHADER_STAGE_VERTEX_BIT, 0,
                          sizeof(mat4_t), surf->modelMatrix);
        
        // Draw surface
        VK_DrawSurface(surf);
    }
}

/*
===============
VK_SetupLightScissor

Calculate and set scissor rectangle for light
===============
*/
static void VK_SetupLightScissor(renderLight_t *light) {
    VkCommandBuffer cmd = vk.cmd;
    vec3_t corners[8];
    vec2_t screenMin, screenMax;
    
    // Calculate light volume corners
    for (int i = 0; i < 8; i++) {
        corners[i][0] = (i & 1) ? light->maxs[0] : light->mins[0];
        corners[i][1] = (i & 2) ? light->maxs[1] : light->mins[1];
        corners[i][2] = (i & 4) ? light->maxs[2] : light->mins[2];
    }
    
    // Project to screen space
    screenMin[0] = screenMin[1] = 999999;
    screenMax[0] = screenMax[1] = -999999;
    
    for (int i = 0; i < 8; i++) {
        vec4_t clip;
        R_TransformModelToClip(corners[i], tr.viewParms.world.modelMatrix,
                               tr.viewParms.projectionMatrix, clip);
        
        if (clip[3] <= 0) continue;
        
        float x = (clip[0] / clip[3] * 0.5f + 0.5f) * tr.viewParms.viewportWidth;
        float y = (clip[1] / clip[3] * 0.5f + 0.5f) * tr.viewParms.viewportHeight;
        
        if (x < screenMin[0]) screenMin[0] = x;
        if (y < screenMin[1]) screenMin[1] = y;
        if (x > screenMax[0]) screenMax[0] = x;
        if (y > screenMax[1]) screenMax[1] = y;
    }
    
    // Set scissor
    VkRect2D scissor = {
        .offset = {
            .x = Com_Clamp(0, tr.viewParms.viewportWidth, (int)screenMin[0]),
            .y = Com_Clamp(0, tr.viewParms.viewportHeight, (int)screenMin[1])
        },
        .extent = {
            .width = Com_Clamp(1, tr.viewParms.viewportWidth - scissor.offset.x,
                              (int)(screenMax[0] - screenMin[0])),
            .height = Com_Clamp(1, tr.viewParms.viewportHeight - scissor.offset.y,
                               (int)(screenMax[1] - screenMin[1]))
        }
    };
    
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

/*
===============
VK_UpdateLightUniforms

Update uniform buffer for a light
===============
*/
static void VK_UpdateLightUniforms(renderLight_t *light, int lightIndex) {
    lightUniforms_t *uniforms = (lightUniforms_t*)vk_additiveLighting.lightUniformMapped;
    uniforms += lightIndex;
    
    // Position and radius
    VectorCopy(light->origin, uniforms->lightPosition);
    uniforms->lightPosition[3] = light->radius;
    
    // Color and intensity
    VectorCopy(light->color, uniforms->lightColor);
    uniforms->lightColor[3] = light->intensity;
    
    // Direction for spot lights
    if (light->type == RL_PROJ) {
        VectorSubtract(light->target, light->origin, uniforms->lightDirection);
        VectorNormalize(uniforms->lightDirection);
        uniforms->lightDirection[3] = 1.0f;
        
        // Spot parameters
        uniforms->spotParams[0] = cos(DEG2RAD(light->fovX * 0.5f));
        uniforms->spotParams[1] = cos(DEG2RAD(light->fovY * 0.5f));
    } else {
        VectorClear(uniforms->lightDirection);
        VectorClear(uniforms->spotParams);
    }
    
    // Attenuation
    uniforms->attenuation[0] = light->constant;
    uniforms->attenuation[1] = light->linear;
    uniforms->attenuation[2] = light->quadratic;
    uniforms->attenuation[3] = light->cutoffDistance;
    
    // Light matrix (for projective textures)
    if (light->type == RL_PROJ) {
        Mat4_Copy(light->projectionMatrix, uniforms->lightMatrix);
    } else {
        Mat4_Identity(uniforms->lightMatrix);
    }
}

/*
===============
VK_RenderLightInteractions

Render all interactions for a light (additive pass)
===============
*/
void VK_RenderLightInteractions(renderLight_t *light) {
    VkCommandBuffer cmd = vk.cmd;
    interaction_t *inter;
    int interactionCount = 0;
    
    // Skip if no interactions
    if (!light->firstInteraction) {
        return;
    }
    
    // Update light uniforms
    VK_UpdateLightUniforms(light, vk_additiveLighting.currentLight);
    
    // Setup light scissor to reduce overdraw
    VK_SetupLightScissor(light);
    
    // Enable additive blending
    VkPipelineColorBlendAttachmentState blendState = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };
    
    // Bind interaction pipeline
    qboolean hasShadows = (light->shadowMap != NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      hasShadows ? vk_additiveLighting.shadowedPipeline :
                                  vk_additiveLighting.interactionPipeline);
    
    // Bind light descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           vk_additiveLighting.lightingPipelineLayout,
                           1, 1, &vk_additiveLighting.lightDescriptorSets[vk_additiveLighting.currentLight],
                           0, NULL);
    
    // Depth test but no depth write for additive passes
    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_EQUAL
    };
    
    // Render each interaction
    inter = light->firstInteraction;
    while (inter) {
        if (!inter->culled && inter->receivesLight && !inter->isEmpty) {
            drawSurf_t *surf = (drawSurf_t*)inter->surface;
            
            // Bind surface textures
            VK_BindSurfaceDescriptors(surf);
            
            // Push surface model matrix
            vkCmdPushConstants(cmd, vk_additiveLighting.lightingPipelineLayout,
                              VK_SHADER_STAGE_VERTEX_BIT, 0,
                              sizeof(mat4_t), surf->modelMatrix);
            
            // Draw interaction
            VK_DrawSurface(surf);
            interactionCount++;
        }
        
        inter = inter->lightNext;
    }
    
    vk_additiveLighting.numInteractionsDrawn += interactionCount;
    vk_additiveLighting.numLightPasses++;
}

/*
===============
VK_RenderAdditiveLighting

Main entry point for additive lighting
===============
*/
void VK_RenderAdditiveLighting(void) {
    extern lightSystem_t tr_lightSystem;
    
    if (!r_dynamiclight || !r_dynamiclight->integer) {
        return;
    }
    
    double startTime = ri.Milliseconds();
    
    // Begin with ambient pass
    VK_BeginLightingPass();
    VK_RenderAmbientPass(tr.refdef.drawSurfs, tr.refdef.numDrawSurfs);
    
    // Switch to additive blending for light passes
    vk_additiveLighting.additiveMode = qtrue;
    
    // Render each visible light
    for (int i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        renderLight_t *light = tr_lightSystem.visibleLights[i];
        
        // Skip lights with no interactions
        if (!light->firstInteraction || !light->numInteractions) {
            continue;
        }
        
        vk_additiveLighting.currentLight = i;
        
        // Render shadow map if needed
        if (light->flags & LIGHTFLAG_FORCESHADOWS) {
            VK_RenderLightShadowMap(light);
        }
        
        // Render light interactions (additive)
        VK_RenderLightInteractions(light);
    }
    
    // Restore normal blending
    vk_additiveLighting.additiveMode = qfalse;
    
    vk_additiveLighting.lightingTime = ri.Milliseconds() - startTime;
    
    // Debug output
    if (r_showLightCount && r_showLightCount->integer) {
        ri.Printf(PRINT_ALL, "Lights: %d passes, %d interactions, %.2fms\n",
                 vk_additiveLighting.numLightPasses,
                 vk_additiveLighting.numInteractionsDrawn,
                 vk_additiveLighting.lightingTime);
    }
}

/*
===============
VK_GetLightingStats

Get lighting statistics
===============
*/
void VK_GetLightingStats(int *numPasses, int *numInteractions, double *time) {
    if (numPasses) {
        *numPasses = vk_additiveLighting.numLightPasses;
    }
    if (numInteractions) {
        *numInteractions = vk_additiveLighting.numInteractionsDrawn;
    }
    if (time) {
        *time = vk_additiveLighting.lightingTime;
    }
}