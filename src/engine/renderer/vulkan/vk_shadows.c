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
#include "vk_shadows.h"
#include "../core/tr_local.h"
#include "../lighting/tr_light.h"

// Global shadow system
shadowSystem_t vk_shadowSystem;

// CVars
cvar_t *r_shadows;
cvar_t *r_shadowMapSize;
cvar_t *r_shadowCascadeCount;
cvar_t *r_shadowDistance;
cvar_t *r_shadowSoftness;
cvar_t *r_shadowBias;
cvar_t *r_shadowFilter;

// Helper functions
static void Mat4_LookAt(mat4_t result, const vec3_t eye, const vec3_t center, const vec3_t up);
static void Mat4_Ortho(mat4_t result, float left, float right, float bottom, float top, float nearVal, float farVal);
static void Mat4_Perspective(mat4_t result, float fovy, float aspect, float nearVal, float farVal);
static void CalculateFrustumCorners(vec3_t corners[8], const mat4_t viewProjInv);

/*
===============
VK_InitShadowSystem

Initialize the shadow mapping system
===============
*/
void VK_InitShadowSystem(void) {
    Com_Memset(&vk_shadowSystem, 0, sizeof(vk_shadowSystem));
    
    // Register CVars
    r_shadows = ri.Cvar_Get("r_shadows", "2", CVAR_ARCHIVE);
    r_shadowMapSize = ri.Cvar_Get("r_shadowMapSize", "2048", CVAR_ARCHIVE);
    r_shadowCascadeCount = ri.Cvar_Get("r_shadowCascadeCount", "4", CVAR_ARCHIVE);
    r_shadowDistance = ri.Cvar_Get("r_shadowDistance", "1000", CVAR_ARCHIVE);
    r_shadowSoftness = ri.Cvar_Get("r_shadowSoftness", "1.0", CVAR_ARCHIVE);
    r_shadowBias = ri.Cvar_Get("r_shadowBias", "0.005", CVAR_ARCHIVE);
    r_shadowFilter = ri.Cvar_Get("r_shadowFilter", "2", CVAR_ARCHIVE);
    
    // Set defaults
    vk_shadowSystem.maxShadowMaps = MAX_SHADOW_MAPS;
    vk_shadowSystem.shadowMapSize = r_shadowMapSize->integer;
    vk_shadowSystem.shadowBias = r_shadowBias->value;
    vk_shadowSystem.shadowSoftness = r_shadowSoftness->value;
    vk_shadowSystem.pcfKernelSize = r_shadowFilter->integer;
    
    // Initialize CSM
    VK_SetupCSM(&vk_shadowSystem.csm, r_shadowCascadeCount->integer, r_shadowDistance->value);
    
    // Create depth-only render pass
    VK_CreateDepthOnlyRenderPass();
    
    // Create shadow pipelines
    VK_CreateShadowPipelines();
    
    // Allocate shadow map pool
    VK_AllocateShadowMapPool();
    
    ri.Printf(PRINT_ALL, "Shadow system initialized: %dx%d maps, %d cascades\n",
              vk_shadowSystem.shadowMapSize, vk_shadowSystem.shadowMapSize,
              vk_shadowSystem.csm.cascadeCount);
}

/*
===============
VK_ShutdownShadowSystem

Cleanup shadow system resources
===============
*/
void VK_ShutdownShadowSystem(void) {
    // Free all shadow maps
    for (int i = 0; i < vk_shadowSystem.numPointShadows; i++) {
        VK_FreePointShadowMap(&vk_shadowSystem.pointShadows[i]);
    }
    
    for (int i = 0; i < vk_shadowSystem.numSpotShadows; i++) {
        VK_FreeSpotShadowMap(&vk_shadowSystem.spotShadows[i]);
    }
    
    // Destroy CSM resources
    VK_DestroyCSM(&vk_shadowSystem.csm);
    
    // Destroy pipelines
    if (vk_shadowSystem.depthOnlyPipeline) {
        vkDestroyPipeline(vk.device, vk_shadowSystem.depthOnlyPipeline, NULL);
    }
    if (vk_shadowSystem.cubeMapPipeline) {
        vkDestroyPipeline(vk.device, vk_shadowSystem.cubeMapPipeline, NULL);
    }
    if (vk_shadowSystem.shadowPipelineLayout) {
        vkDestroyPipelineLayout(vk.device, vk_shadowSystem.shadowPipelineLayout, NULL);
    }
    
    // Destroy render passes
    if (vk_shadowSystem.depthOnlyRenderPass) {
        vkDestroyRenderPass(vk.device, vk_shadowSystem.depthOnlyRenderPass, NULL);
    }
    if (vk_shadowSystem.cubeMapRenderPass) {
        vkDestroyRenderPass(vk.device, vk_shadowSystem.cubeMapRenderPass, NULL);
    }
    
    // Free shadow map pool
    VK_FreeShadowMapPool();
    
    Com_Memset(&vk_shadowSystem, 0, sizeof(vk_shadowSystem));
}

/*
===============
VK_SetupCSM

Setup cascaded shadow mapping
===============
*/
void VK_SetupCSM(csmData_t *csm, int cascadeCount, float shadowDistance) {
    csm->cascadeCount = Com_Clamp(1, MAX_CASCADE_COUNT, cascadeCount);
    csm->shadowDistance = shadowDistance;
    csm->cascadeSplitLambda = 0.75f; // Good balance between linear and logarithmic
    csm->resolution = vk_shadowSystem.shadowMapSize;
    
    // Create shadow map array
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = {
            .width = csm->resolution,
            .height = csm->resolution,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = csm->cascadeCount,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    
    VK_CHECK(vkCreateImage(vk.device, &imageInfo, NULL, &csm->shadowMapArray));
    
    // Allocate memory
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vk.device, csm->shadowMapArray, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = VK_FindMemoryType(memReqs.memoryTypeBits, 
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &csm->shadowMapMemory));
    VK_CHECK(vkBindImageMemory(vk.device, csm->shadowMapArray, csm->shadowMapMemory, 0));
    
    // Create array view for shader sampling
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = csm->shadowMapArray,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = csm->cascadeCount
        }
    };
    
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &csm->shadowMapArrayView));
    
    // Create per-cascade views and framebuffers
    for (int i = 0; i < csm->cascadeCount; i++) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.subresourceRange.baseArrayLayer = i;
        viewInfo.subresourceRange.layerCount = 1;
        
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &csm->cascades[i].depthView));
        
        // Create framebuffer
        VkFramebufferCreateInfo fbInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = vk_shadowSystem.depthOnlyRenderPass,
            .attachmentCount = 1,
            .pAttachments = &csm->cascades[i].depthView,
            .width = csm->resolution,
            .height = csm->resolution,
            .layers = 1
        };
        
        VK_CHECK(vkCreateFramebuffer(vk.device, &fbInfo, NULL, &csm->cascades[i].framebuffer));
    }
    
    // Create shadow sampler
    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        .compareEnable = VK_TRUE,
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .maxAnisotropy = 1.0f
    };
    
    VK_CHECK(vkCreateSampler(vk.device, &samplerInfo, NULL, &csm->shadowSampler));
}

/*
===============
VK_CalculateCSMSplits

Calculate cascade split distances
===============
*/
void VK_CalculateCSMSplits(csmData_t *csm, float nearPlane, float farPlane) {
    float lambda = csm->cascadeSplitLambda;
    float ratio = farPlane / nearPlane;
    float clipRange = farPlane - nearPlane;
    
    // Calculate split distances using practical split scheme
    for (int i = 0; i < csm->cascadeCount; i++) {
        float p = (i + 1) / (float)csm->cascadeCount;
        float log = nearPlane * powf(ratio, p);
        float uniform = nearPlane + clipRange * p;
        float d = lambda * (log - uniform) + uniform;
        
        csm->cascades[i].splitDistances[0] = (i == 0) ? nearPlane : 
                                            csm->cascades[i-1].splitDistances[1];
        csm->cascades[i].splitDistances[1] = d;
    }
    
    // Ensure last cascade reaches shadow distance
    if (csm->shadowDistance < farPlane) {
        csm->cascades[csm->cascadeCount - 1].splitDistances[1] = csm->shadowDistance;
    }
}

/*
===============
VK_UpdateCSMMatrices

Update cascade view-projection matrices
===============
*/
void VK_UpdateCSMMatrices(csmData_t *csm, const vec3_t lightDir, 
                         const mat4_t viewMatrix, const mat4_t projMatrix) {
    vec3_t frustumCorners[8];
    vec3_t cascadeCenter;
    vec3_t cascadeExtents;
    mat4_t lightView;
    mat4_t lightProj;
    
    // For each cascade
    for (int cascade = 0; cascade < csm->cascadeCount; cascade++) {
        float nearSplit = csm->cascades[cascade].splitDistances[0];
        float farSplit = csm->cascades[cascade].splitDistances[1];
        
        // Calculate frustum corners for this cascade
        mat4_t cascadeProj;
        Mat4_Perspective(cascadeProj, tr.refdef.fov_y, 
                        (float)tr.refdef.width / (float)tr.refdef.height,
                        nearSplit, farSplit);
        
        mat4_t invViewProj;
        Mat4_Multiply(cascadeProj, viewMatrix, invViewProj);
        Mat4_Inverse(invViewProj, invViewProj);
        
        CalculateFrustumCorners(frustumCorners, invViewProj);
        
        // Calculate cascade bounding sphere
        VectorClear(cascadeCenter);
        for (int i = 0; i < 8; i++) {
            VectorAdd(cascadeCenter, frustumCorners[i], cascadeCenter);
        }
        VectorScale(cascadeCenter, 1.0f / 8.0f, cascadeCenter);
        
        float radius = 0;
        for (int i = 0; i < 8; i++) {
            float dist = Distance(frustumCorners[i], cascadeCenter);
            if (dist > radius) {
                radius = dist;
            }
        }
        
        // Snap to texel grid to reduce shimmer
        float texelsPerUnit = csm->resolution / (radius * 2.0f);
        mat4_t snapMatrix;
        Mat4_Identity(snapMatrix);
        snapMatrix[12] = floor(cascadeCenter[0] * texelsPerUnit) / texelsPerUnit;
        snapMatrix[13] = floor(cascadeCenter[1] * texelsPerUnit) / texelsPerUnit;
        snapMatrix[14] = floor(cascadeCenter[2] * texelsPerUnit) / texelsPerUnit;
        
        // Create light view matrix
        vec3_t lightPos;
        VectorMA(cascadeCenter, -radius * 2.0f, lightDir, lightPos);
        Mat4_LookAt(lightView, lightPos, cascadeCenter, (vec3_t){0, 1, 0});
        
        // Create light projection matrix (orthographic)
        Mat4_Ortho(lightProj, -radius, radius, -radius, radius, 0.1f, radius * 4.0f);
        
        // Combine matrices
        Mat4_Multiply(lightProj, lightView, csm->cascades[cascade].viewProjMatrix);
        
        // Setup frustum for culling
        R_SetupFrustumFromMatrix(&csm->cascades[cascade].frustum, 
                                csm->cascades[cascade].viewProjMatrix);
    }
}

/*
===============
VK_RenderCSMCascade

Render a single cascade
===============
*/
void VK_RenderCSMCascade(int cascade, csmData_t *csm, drawSurf_t **surfaces, int numSurfaces) {
    VkCommandBuffer cmd = vk.cmd;
    
    // Begin shadow render pass
    VkClearValue clearValue;
    clearValue.depthStencil.depth = 1.0f;
    clearValue.depthStencil.stencil = 0;
    
    VkRenderPassBeginInfo rpBegin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vk_shadowSystem.depthOnlyRenderPass,
        .framebuffer = csm->cascades[cascade].framebuffer,
        .renderArea = {
            .extent = { csm->resolution, csm->resolution }
        },
        .clearValueCount = 1,
        .pClearValues = &clearValue
    };
    
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    
    // Set viewport
    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)csm->resolution,
        .height = (float)csm->resolution,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = {csm->resolution, csm->resolution}
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    
    // Bind shadow pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, 
                      vk_shadowSystem.depthOnlyPipeline);
    
    // Push cascade view-projection matrix
    vkCmdPushConstants(cmd, vk_shadowSystem.shadowPipelineLayout,
                      VK_SHADER_STAGE_VERTEX_BIT, 0, 
                      sizeof(mat4_t), &csm->cascades[cascade].viewProjMatrix);
    
    // Render shadow casters
    int drawCalls = 0;
    for (int i = 0; i < numSurfaces; i++) {
        drawSurf_t *surf = surfaces[i];
        
        // Skip non-shadow casting surfaces
        if (!surf->shader || (surf->shader->surfaceFlags & SURF_NOSHADOWS)) {
            continue;
        }
        
        // Frustum cull against cascade
        if (!VK_SurfaceInFrustum(surf, &csm->cascades[cascade].frustum)) {
            continue;
        }
        
        // Render surface
        VK_RenderShadowSurface(surf, csm->cascades[cascade].viewProjMatrix);
        drawCalls++;
    }
    
    vkCmdEndRenderPass(cmd);
    
    csm->numCascadeDrawCalls += drawCalls;
}

/*
===============
VK_RenderCSM

Render all cascades
===============
*/
void VK_RenderCSM(csmData_t *csm) {
    if (!csm->cascadeCount || !r_shadows->integer) {
        return;
    }
    
    double startTime = ri.Milliseconds();
    
    // Reset statistics
    csm->numShadowCasters = 0;
    csm->numCascadeDrawCalls = 0;
    
    // Get sun direction from first directional light
    vec3_t sunDir = {0, 0, -1}; // Default sun direction
    
    // Find directional light
    extern lightSystem_t tr_lightSystem;
    for (int i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        renderLight_t *light = tr_lightSystem.visibleLights[i];
        if (light->type == RL_DIRECTIONAL) {
            VectorCopy(light->target, sunDir);
            VectorNormalize(sunDir);
            break;
        }
    }
    
    // Calculate cascade splits
    VK_CalculateCSMSplits(csm, r_znear->value, tr.viewParms.zFar);
    
    // Update cascade matrices
    VK_UpdateCSMMatrices(csm, sunDir, tr.viewParms.world.modelMatrix, 
                        tr.viewParms.projectionMatrix);
    
    // Render each cascade
    for (int i = 0; i < csm->cascadeCount; i++) {
        VK_RenderCSMCascade(i, csm, tr.refdef.drawSurfs, tr.refdef.numDrawSurfs);
    }
    
    // Transition shadow maps for sampling
    VK_TransitionShadowMapsForSampling();
    
    vk_shadowSystem.shadowRenderTime += ri.Milliseconds() - startTime;
}

/*
===============
Helper Functions
===============
*/

static void Mat4_LookAt(mat4_t result, const vec3_t eye, const vec3_t center, const vec3_t up) {
    vec3_t f, s, u;
    
    VectorSubtract(center, eye, f);
    VectorNormalize(f);
    
    CrossProduct(f, up, s);
    VectorNormalize(s);
    
    CrossProduct(s, f, u);
    
    result[0] = s[0];
    result[4] = s[1];
    result[8] = s[2];
    result[12] = -DotProduct(s, eye);
    
    result[1] = u[0];
    result[5] = u[1];
    result[9] = u[2];
    result[13] = -DotProduct(u, eye);
    
    result[2] = -f[0];
    result[6] = -f[1];
    result[10] = -f[2];
    result[14] = DotProduct(f, eye);
    
    result[3] = 0;
    result[7] = 0;
    result[11] = 0;
    result[15] = 1;
}

static void Mat4_Ortho(mat4_t result, float left, float right, float bottom, 
                       float top, float nearVal, float farVal) {
    float rl = 1.0f / (right - left);
    float tb = 1.0f / (top - bottom);
    float fn = 1.0f / (farVal - nearVal);
    
    Com_Memset(result, 0, sizeof(mat4_t));
    
    result[0] = 2.0f * rl;
    result[5] = 2.0f * tb;
    result[10] = -2.0f * fn;
    result[12] = -(right + left) * rl;
    result[13] = -(top + bottom) * tb;
    result[14] = -(farVal + nearVal) * fn;
    result[15] = 1.0f;
}

static void Mat4_Perspective(mat4_t result, float fovy, float aspect, 
                             float nearVal, float farVal) {
    float f = 1.0f / tan(DEG2RAD(fovy) * 0.5f);
    float fn = 1.0f / (nearVal - farVal);
    
    Com_Memset(result, 0, sizeof(mat4_t));
    
    result[0] = f / aspect;
    result[5] = f;
    result[10] = (farVal + nearVal) * fn;
    result[11] = -1.0f;
    result[14] = 2.0f * farVal * nearVal * fn;
}

static void CalculateFrustumCorners(vec3_t corners[8], const mat4_t viewProjInv) {
    // NDC cube corners
    vec4_t ndcCorners[8] = {
        {-1, -1, -1, 1}, {1, -1, -1, 1}, {1, 1, -1, 1}, {-1, 1, -1, 1},
        {-1, -1, 1, 1}, {1, -1, 1, 1}, {1, 1, 1, 1}, {-1, 1, 1, 1}
    };
    
    // Transform to world space
    for (int i = 0; i < 8; i++) {
        vec4_t worldPos;
        Mat4_TransformVec4(viewProjInv, ndcCorners[i], worldPos);
        VectorScale(worldPos, 1.0f / worldPos[3], corners[i]);
    }
}

/*
===============
VK_UpdateLightShadowMap

Update shadow map for a specific light
Called from the light management system
===============
*/
void VK_UpdateLightShadowMap(renderLight_t *light) {
    if (!light || !light->shadowMap) {
        return;
    }
    
    // Calculate light matrices if needed
    if (light->type == RL_DIRECTIONAL) {
        // For directional lights, we use the CSM system
        // which is already handled by VK_UpdateCSM
        VK_UpdateCSM(&tr.viewParms);
    } else if (light->type == RL_PROJ) {
        // Projected lights use their own shadow maps
        // The matrices are already calculated in the light structure
        // Here we would render to the shadow map using Vulkan
        
        // This would involve:
        // 1. Acquiring a shadow map from the pool
        // 2. Beginning a render pass with the shadow framebuffer
        // 3. Setting viewport and scissor
        // 4. Binding the shadow pipeline
        // 5. Rendering shadow casters visible to the light
        // 6. Ending the render pass
        
        // For now, we mark it as handled
    } else {
        // Point lights would use cube maps
        // Implementation would render 6 faces
    }
    
    // Mark the shadow map as updated
    light->shadowMap->needsUpdate = qfalse;
}