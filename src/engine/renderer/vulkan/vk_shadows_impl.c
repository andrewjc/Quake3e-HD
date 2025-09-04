/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Implementation of missing shadow system functions
These functions are not implemented in vk_shadows.c
===========================================================================
*/

#include "vk.h"
#include "vk_shadows.h"
#include "../core/tr_local.h"
#include "../lighting/tr_light.h"
#include "../lighting/tr_light_dynamic.h"

// External references
extern refimport_t ri;
extern trGlobals_t tr;
extern shadowSystem_t vk_shadowSystem;
extern cvar_t *r_znear;
extern cvar_t *r_shadows;
extern cvar_t *r_shadowMapSize;
extern cvar_t *r_shadowCascadeCount;
extern cvar_t *r_shadowDistance;
extern cvar_t *r_shadowSoftness;
extern cvar_t *r_shadowBias;
extern cvar_t *r_shadowFilter;

// Define missing REF_ flags if not already defined
#ifndef REF_SHADOW_CASTING
#define REF_SHADOW_CASTING      0x0100
#endif
#ifndef REF_DIRECTIONAL_LIGHT
#define REF_DIRECTIONAL_LIGHT   0x0200
#endif
#ifndef REF_SPOT_LIGHT
#define REF_SPOT_LIGHT          0x0400
#endif

// Forward declarations for helper functions
void BuildLightViewMatrix(mat4_t result, const vec3_t eye, const vec3_t center, const vec3_t up);
void BuildOrthographicMatrix(mat4_t result, float left, float right, float bottom, float top, float nearPlane, float farPlane);
void CalculateFrustumCorners(vec3_t corners[8], const mat4_t invViewProj);
void ExtractFrustumPlanes(cplane_t planes[5], const mat4_t viewProj);
void Mat4_Copy(const mat4_t src, mat4_t dst);
void Mat4_Multiply(const mat4_t a, const mat4_t b, mat4_t out);
void Mat4_Inverse(const mat4_t m, mat4_t out);
void Mat4_Identity(mat4_t m);
void Mat4_TransformVec4(const mat4_t m, const vec4_t in, vec4_t out);

// Helper function to find appropriate memory type
uint32_t VK_FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(vk.physical_device, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    ri.Error(ERR_FATAL, "Failed to find suitable memory type!");
    return 0;
}

// Missing shadow system functions

void VK_ResetShadowMaps(void) {
    // Free point shadow maps
    for (int i = 0; i < vk_shadowSystem.numPointShadows; i++) {
        VK_FreePointShadowMap(&vk_shadowSystem.pointShadows[i]);
    }
    vk_shadowSystem.numPointShadows = 0;
    
    // Free spot shadow maps
    for (int i = 0; i < vk_shadowSystem.numSpotShadows; i++) {
        VK_FreeSpotShadowMap(&vk_shadowSystem.spotShadows[i]);
    }
    vk_shadowSystem.numSpotShadows = 0;
    
    // Reset CSM
    if (vk_shadowSystem.csm.shadowMapArray != VK_NULL_HANDLE) {
        vkDestroyImage(vk.device, vk_shadowSystem.csm.shadowMapArray, NULL);
        vk_shadowSystem.csm.shadowMapArray = VK_NULL_HANDLE;
    }
    
    if (vk_shadowSystem.csm.shadowMapMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vk.device, vk_shadowSystem.csm.shadowMapMemory, NULL);
        vk_shadowSystem.csm.shadowMapMemory = VK_NULL_HANDLE;
    }
    
    // Destroy framebuffers and image views
    for (int i = 0; i < MAX_CASCADE_COUNT; i++) {
        if (vk_shadowSystem.csm.cascades[i].framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(vk.device, vk_shadowSystem.csm.cascades[i].framebuffer, NULL);
            vk_shadowSystem.csm.cascades[i].framebuffer = VK_NULL_HANDLE;
        }
        if (vk_shadowSystem.csm.cascades[i].depthView != VK_NULL_HANDLE) {
            vkDestroyImageView(vk.device, vk_shadowSystem.csm.cascades[i].depthView, NULL);
            vk_shadowSystem.csm.cascades[i].depthView = VK_NULL_HANDLE;
        }
    }
}

// Shadow pass management
void VK_BeginShadowPass(VkFramebuffer framebuffer, int width, int height) {
    VkRenderPassBeginInfo renderPassBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = vk_shadowSystem.depthOnlyRenderPass,
        .framebuffer = framebuffer,
        .renderArea = {
            .offset = {0, 0},
            .extent = {width, height}
        },
        .clearValueCount = 1,
        .pClearValues = &(VkClearValue){
            .depthStencil = {1.0f, 0}
        }
    };
    
    vkCmdBeginRenderPass(vk.cmd->command_buffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // Set viewport
    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)width,
        .height = (float)height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(vk.cmd->command_buffer, 0, 1, &viewport);
    
    // Set scissor
    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = {width, height}
    };
    vkCmdSetScissor(vk.cmd->command_buffer, 0, 1, &scissor);
    
    // Bind shadow pipeline
    if (vk_shadowSystem.depthOnlyPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, 
                          vk_shadowSystem.depthOnlyPipeline);
    }
}

void VK_EndShadowPass(void) {
    vkCmdEndRenderPass(vk.cmd->command_buffer);
}

void VK_RenderShadowSurface(drawSurf_t *surface, const mat4_t viewProjMatrix) {
    if (!surface || !surface->surface) {
        return;
    }
    
    // Update push constants with the view-projection matrix
    vkCmdPushConstants(vk.cmd->command_buffer, vk_shadowSystem.shadowPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4_t), viewProjMatrix);
    
    // The actual surface rendering would depend on the surface type
    // and how the vertex data is stored in this engine
    // For now, just increment the counter
    vk_shadowSystem.csm.numShadowCasters++;
}

qboolean VK_SurfaceInFrustum(drawSurf_t *surf, const cplane_t frustum[5]) {
    if (!surf || !frustum) {
        return qfalse;
    }
    
    // Simple AABB vs frustum test
    // CSM uses 5 planes (no far plane)
    for (int i = 0; i < 5; i++) {
        const cplane_t *plane = &frustum[i];
        
        // Find the vertex furthest in the direction of the plane normal
        vec3_t vp;
        for (int j = 0; j < 3; j++) {
            if (plane->normal[j] >= 0) {
                vp[j] = surf->bounds[1][j];
            } else {
                vp[j] = surf->bounds[0][j];
            }
        }
        
        // If this vertex is behind the plane, the whole box is outside
        if (DotProduct(vp, plane->normal) - plane->dist < 0) {
            return qfalse;
        }
    }
    
    return qtrue;
}

// Point shadow functions
pointShadowMap_t* VK_AllocatePointShadowMap(renderLight_t *light) {
    if (vk_shadowSystem.numPointShadows >= MAX_SHADOW_MAPS) {
        return NULL;
    }
    
    pointShadowMap_t *shadow = &vk_shadowSystem.pointShadows[vk_shadowSystem.numPointShadows++];
    Com_Memset(shadow, 0, sizeof(pointShadowMap_t));
    
    shadow->light = light;
    shadow->resolution = CUBE_MAP_SIZE;
    shadow->nearPlane = 1.0f;
    shadow->farPlane = light->radius;
    shadow->needsUpdate = qtrue;
    
    // Create cube map image
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = {
            .width = shadow->resolution,
            .height = shadow->resolution,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 6,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
    };
    
    VK_CHECK(vkCreateImage(vk.device, &imageInfo, NULL, &shadow->cubeMap));
    
    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(vk.device, shadow->cubeMap, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = VK_FindMemoryType(memRequirements.memoryTypeBits, 
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &shadow->cubeMapMemory));
    vkBindImageMemory(vk.device, shadow->cubeMap, shadow->cubeMapMemory, 0);
    
    // Create image views for each cube face
    for (int face = 0; face < 6; face++) {
        VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = shadow->cubeMap,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .levelCount = 1,
                .baseMipLevel = 0,
                .layerCount = 1,
                .baseArrayLayer = face
            }
        };
        
        VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &shadow->faceViews[face]));
        
        // Create framebuffer for this face
        VkFramebufferCreateInfo fbInfo = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = vk_shadowSystem.depthOnlyRenderPass,
            .attachmentCount = 1,
            .pAttachments = &shadow->faceViews[face],
            .width = shadow->resolution,
            .height = shadow->resolution,
            .layers = 1
        };
        
        VK_CHECK(vkCreateFramebuffer(vk.device, &fbInfo, NULL, &shadow->framebuffers[face]));
    }
    
    // Create cube map view for sampling
    VkImageViewCreateInfo cubeViewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = shadow->cubeMap,
        .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .baseMipLevel = 0,
            .layerCount = 6,
            .baseArrayLayer = 0
        }
    };
    
    VK_CHECK(vkCreateImageView(vk.device, &cubeViewInfo, NULL, &shadow->cubeMapView));
    
    return shadow;
}

void VK_FreePointShadowMap(pointShadowMap_t *shadow) {
    if (!shadow) {
        return;
    }
    
    if (shadow->cubeMapView != VK_NULL_HANDLE) {
        vkDestroyImageView(vk.device, shadow->cubeMapView, NULL);
    }
    
    for (int i = 0; i < 6; i++) {
        if (shadow->framebuffers[i] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(vk.device, shadow->framebuffers[i], NULL);
        }
        if (shadow->faceViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(vk.device, shadow->faceViews[i], NULL);
        }
    }
    
    if (shadow->cubeMap != VK_NULL_HANDLE) {
        vkDestroyImage(vk.device, shadow->cubeMap, NULL);
    }
    
    if (shadow->cubeMapMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vk.device, shadow->cubeMapMemory, NULL);
    }
    
    Com_Memset(shadow, 0, sizeof(pointShadowMap_t));
}

void VK_RenderPointShadowMap(renderLight_t *light, pointShadowMap_t *shadowMap) {
    if (!light || !shadowMap) {
        return;
    }
    
    shadowMap->needsUpdate = qfalse;
    shadowMap->lastUpdateFrame = tr.frameCount;
    
    // Update matrices for all cube faces
    VK_UpdatePointShadowMatrices(shadowMap);
    
    // Render each cube face
    for (int face = 0; face < 6; face++) {
        VK_BeginShadowPass(shadowMap->framebuffers[face], 
                          shadowMap->resolution, shadowMap->resolution);
        
        // Set the view-projection matrix for this face
        mat4_t viewProj;
        Mat4_Multiply(shadowMap->projMatrix, shadowMap->viewMatrices[face], viewProj);
        
        // Render all shadow casting surfaces from this perspective
        // This would iterate through visible surfaces
        
        VK_EndShadowPass();
    }
}

void VK_UpdatePointShadowMatrices(pointShadowMap_t *shadow) {
    if (!shadow || !shadow->light) {
        return;
    }
    
    // Set up projection matrix (90 degree FOV for cube map)
    float aspect = 1.0f;
    float fov = 90.0f * (M_PI / 180.0f);
    
    Com_Memset(shadow->projMatrix, 0, sizeof(mat4_t));
    shadow->projMatrix[0] = 1.0f / tanf(fov * 0.5f);
    shadow->projMatrix[5] = aspect / tanf(fov * 0.5f);
    shadow->projMatrix[10] = -(shadow->farPlane + shadow->nearPlane) / (shadow->farPlane - shadow->nearPlane);
    shadow->projMatrix[11] = -1.0f;
    shadow->projMatrix[14] = -(2.0f * shadow->farPlane * shadow->nearPlane) / (shadow->farPlane - shadow->nearPlane);
    
    // Set up view matrices for each cube face
    vec3_t eye;
    VectorCopy(shadow->light->origin, eye);
    
    // Cube map face directions and up vectors
    vec3_t targets[6] = {
        {1, 0, 0},   // +X
        {-1, 0, 0},  // -X
        {0, 1, 0},   // +Y
        {0, -1, 0},  // -Y
        {0, 0, 1},   // +Z
        {0, 0, -1}   // -Z
    };
    
    vec3_t ups[6] = {
        {0, -1, 0},  // +X
        {0, -1, 0},  // -X
        {0, 0, 1},   // +Y
        {0, 0, -1},  // -Y
        {0, -1, 0},  // +Z
        {0, -1, 0}   // -Z
    };
    
    for (int i = 0; i < 6; i++) {
        vec3_t center;
        VectorAdd(eye, targets[i], center);
        BuildLightViewMatrix(shadow->viewMatrices[i], eye, center, ups[i]);
    }
}

// Spot shadow functions
spotShadowMap_t* VK_AllocateSpotShadowMap(renderLight_t *light) {
    if (vk_shadowSystem.numSpotShadows >= MAX_SHADOW_MAPS) {
        return NULL;
    }
    
    spotShadowMap_t *shadow = &vk_shadowSystem.spotShadows[vk_shadowSystem.numSpotShadows++];
    Com_Memset(shadow, 0, sizeof(spotShadowMap_t));
    
    shadow->light = light;
    shadow->resolution = SHADOW_MAP_SIZE;
    shadow->nearPlane = 1.0f;
    shadow->farPlane = light->radius;
    shadow->needsUpdate = qtrue;
    
    // Create depth image
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = {
            .width = shadow->resolution,
            .height = shadow->resolution,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    
    VK_CHECK(vkCreateImage(vk.device, &imageInfo, NULL, &shadow->depthImage));
    
    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(vk.device, shadow->depthImage, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = VK_FindMemoryType(memRequirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    VK_CHECK(vkAllocateMemory(vk.device, &allocInfo, NULL, &shadow->depthMemory));
    vkBindImageMemory(vk.device, shadow->depthImage, shadow->depthMemory, 0);
    
    // Create depth image view
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = shadow->depthImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .baseMipLevel = 0,
            .layerCount = 1,
            .baseArrayLayer = 0
        }
    };
    
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &shadow->depthView));
    
    // Create framebuffer
    VkFramebufferCreateInfo fbInfo = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = vk_shadowSystem.depthOnlyRenderPass,
        .attachmentCount = 1,
        .pAttachments = &shadow->depthView,
        .width = shadow->resolution,
        .height = shadow->resolution,
        .layers = 1
    };
    
    VK_CHECK(vkCreateFramebuffer(vk.device, &fbInfo, NULL, &shadow->framebuffer));
    
    return shadow;
}

void VK_FreeSpotShadowMap(spotShadowMap_t *shadow) {
    if (!shadow) {
        return;
    }
    
    if (shadow->framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(vk.device, shadow->framebuffer, NULL);
    }
    
    if (shadow->depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(vk.device, shadow->depthView, NULL);
    }
    
    if (shadow->depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(vk.device, shadow->depthImage, NULL);
    }
    
    if (shadow->depthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vk.device, shadow->depthMemory, NULL);
    }
    
    Com_Memset(shadow, 0, sizeof(spotShadowMap_t));
}

void VK_RenderSpotShadowMap(renderLight_t *light, spotShadowMap_t *shadowMap) {
    if (!light || !shadowMap) {
        return;
    }
    
    shadowMap->needsUpdate = qfalse;
    shadowMap->lastUpdateFrame = tr.frameCount;
    
    // Update the view-projection matrix for the spot light
    VK_UpdateSpotShadowMatrix(shadowMap);
    
    VK_BeginShadowPass(shadowMap->framebuffer, 
                      shadowMap->resolution, shadowMap->resolution);
    
    // Render all shadow casting surfaces from the light's perspective
    // This would iterate through visible surfaces
    
    VK_EndShadowPass();
}

void VK_UpdateSpotShadowMatrix(spotShadowMap_t *shadow) {
    if (!shadow || !shadow->light) {
        return;
    }
    
    // Build view matrix looking along the light direction
    // For spot lights, we need to determine the direction from light properties
    vec3_t center;
    vec3_t direction = {0, 0, -1}; // Default direction
    // If the light has target info, use that
    VectorAdd(shadow->light->origin, direction, center);
    
    vec3_t up = {0, 0, 1};
    if (fabs(DotProduct(direction, up)) > 0.99f) {
        VectorSet(up, 1, 0, 0);
    }
    
    mat4_t viewMatrix;
    BuildLightViewMatrix(viewMatrix, shadow->light->origin, center, up);
    
    // Build perspective projection matrix for spot light
    // Assuming cone angle is stored somewhere in light structure
    float fov = 60.0f * (M_PI / 180.0f); // Default 60 degree cone
    float aspect = 1.0f;
    
    mat4_t projMatrix;
    Com_Memset(projMatrix, 0, sizeof(mat4_t));
    projMatrix[0] = 1.0f / (aspect * tanf(fov * 0.5f));
    projMatrix[5] = 1.0f / tanf(fov * 0.5f);
    projMatrix[10] = -(shadow->farPlane + shadow->nearPlane) / (shadow->farPlane - shadow->nearPlane);
    projMatrix[11] = -1.0f;
    projMatrix[14] = -(2.0f * shadow->farPlane * shadow->nearPlane) / (shadow->farPlane - shadow->nearPlane);
    
    // Combine view and projection
    Mat4_Multiply(projMatrix, viewMatrix, shadow->viewProjMatrix);
}

// CSM update function
void VK_UpdateCSM(struct viewParms_s *viewParms) {
    if (!viewParms || !vk_shadowSystem.csmEnabled) {
        return;
    }
    
    csmData_t *csm = &vk_shadowSystem.csm;
    vec3_t sunDir;
    qboolean foundSun = qfalse;
    
    // Search for sun light in scene
    for (int i = 0; i < tr.refdef.num_dlights; i++) {
        if (VectorLength(tr.refdef.dlights[i].dir) > 0.001f) {
            VectorCopy(tr.refdef.dlights[i].dir, sunDir);
            VectorNormalize(sunDir);
            foundSun = qtrue;
            break;
        }
    }
    
    if (!foundSun) {
        // Default sun direction
        VectorSet(sunDir, 0.5f, 0.5f, -0.866f);
    }
    
    // Calculate cascade splits
    float nearPlane = r_znear->value;
    float farPlane = viewParms->zFar;
    VK_CalculateCSMSplits(csm, nearPlane, farPlane);
    
    // Update cascade matrices
    mat4_t viewMatrix, projMatrix;
    Com_Memcpy(viewMatrix, viewParms->world.modelMatrix, sizeof(mat4_t));
    Com_Memcpy(projMatrix, viewParms->projectionMatrix, sizeof(mat4_t));
    
    VK_UpdateCSMMatrices(csm, sunDir, viewMatrix, projMatrix);
    
    // Calculate frustum for each cascade
    for (int i = 0; i < csm->cascadeCount; i++) {
        float nearSplit = csm->cascades[i].splitDistances[0];
        float farSplit = csm->cascades[i].splitDistances[1];
        
        // Build projection matrix for this cascade split
        mat4_t cascadeProj;
        Com_Memcpy(cascadeProj, projMatrix, sizeof(mat4_t));
        
        // Adjust near/far planes
        cascadeProj[10] = -(farSplit + nearSplit) / (farSplit - nearSplit);
        cascadeProj[14] = -(2.0f * farSplit * nearSplit) / (farSplit - nearSplit);
        
        // Combine view and projection
        mat4_t viewProj;
        Mat4_Multiply(cascadeProj, viewMatrix, viewProj);
        
        // Calculate inverse
        mat4_t invViewProj;
        Mat4_Inverse(viewProj, invViewProj);
        
        // Calculate frustum corners
        vec3_t frustumCorners[8];
        CalculateFrustumCorners(frustumCorners, invViewProj);
        
        // Calculate bounding sphere
        vec3_t cascadeCenter = {0, 0, 0};
        for (int j = 0; j < 8; j++) {
            VectorAdd(cascadeCenter, frustumCorners[j], cascadeCenter);
        }
        VectorScale(cascadeCenter, 1.0f / 8.0f, cascadeCenter);
        
        float radius = 0;
        for (int j = 0; j < 8; j++) {
            float dist = Distance(frustumCorners[j], cascadeCenter);
            if (dist > radius) {
                radius = dist;
            }
        }
        
        // Build light view matrix
        vec3_t lightPos;
        VectorMA(cascadeCenter, -radius, sunDir, lightPos);
        
        vec3_t up = {0, 0, 1};
        if (fabs(DotProduct(sunDir, up)) > 0.99f) {
            VectorSet(up, 1, 0, 0);
        }
        
        BuildLightViewMatrix(csm->cascades[i].viewProjMatrix, lightPos, cascadeCenter, up);
        
        // Create orthographic projection
        mat4_t lightProj;
        BuildOrthographicMatrix(lightProj, -radius, radius, -radius, radius, 0.1f, radius * 2.0f);
        
        // Combine
        Mat4_Multiply(lightProj, csm->cascades[i].viewProjMatrix, csm->cascades[i].viewProjMatrix);
        
        // Extract frustum planes
        ExtractFrustumPlanes(csm->cascades[i].frustum, csm->cascades[i].viewProjMatrix);
    }
}

// Configuration functions
void VK_SetShadowMapSize(int size) {
    vk_shadowSystem.shadowMapSize = size;
}

void VK_SetShadowDistance(float distance) {
    vk_shadowSystem.csm.shadowDistance = distance;
}

void VK_SetShadowSoftness(float softness) {
    vk_shadowSystem.shadowSoftness = softness;
}

void VK_SetShadowBias(float bias) {
    vk_shadowSystem.shadowBias = bias;
}

// Shadow sampling functions
VkDescriptorSet VK_GetShadowDescriptorSet(void) {
    // Descriptor set would be managed elsewhere
    return VK_NULL_HANDLE;
}

void VK_BindShadowMaps(VkCommandBuffer cmd, VkPipelineLayout layout) {
    // Descriptor set binding would be implemented when descriptor management is added
    // For now, this is a placeholder
    (void)cmd;
    (void)layout;
}

void VK_SetShadowUniforms(const mat4_t *shadowMatrices, const vec4_t *cascadeSplits, int numCascades) {
    // This would update uniform buffers or push constants with shadow matrices
    // Implementation depends on how the renderer handles uniforms
}

// Debug functions
void VK_DrawShadowFrustum(const cplane_t frustum[5], const vec4_t color) {
    // Would draw debug lines for frustum visualization
}

void VK_DrawCSMSplits(const csmData_t *csm) {
    // Would draw debug visualization of cascade splits
}

void VK_GetShadowStats(int *numMaps, int *memoryUsed, double *renderTime) {
    if (numMaps) {
        *numMaps = vk_shadowSystem.totalShadowMaps;
    }
    if (memoryUsed) {
        *memoryUsed = vk_shadowSystem.totalShadowMaps * vk_shadowSystem.shadowMapSize * vk_shadowSystem.shadowMapSize * 4;
    }
    if (renderTime) {
        *renderTime = vk_shadowSystem.shadowRenderTime;
    }
}

// Shadow map management (not in header but needed by implementation)
VkImage VK_AllocateShadowMapImage(int width, int height, VkFormat format, VkImageUsageFlags usage) {
    VkImage image;
    
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    
    VK_CHECK(vkCreateImage(vk.device, &imageInfo, NULL, &image));
    
    return image;
}

void VK_FreeShadowMapImage(VkImage image) {
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(vk.device, image, NULL);
    }
}

VkImageView VK_CreateShadowMapView(VkImage image, VkFormat format, VkImageViewType viewType, uint32_t baseLayer, uint32_t layerCount) {
    VkImageView view;
    
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = viewType,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .baseMipLevel = 0,
            .layerCount = layerCount,
            .baseArrayLayer = baseLayer
        }
    };
    
    VK_CHECK(vkCreateImageView(vk.device, &viewInfo, NULL, &view));
    
    return view;
}

// Helper functions for missing shadow system support
void VK_CreateDepthOnlyRenderPass(void) {
    VkAttachmentDescription depthAttachment = {
        .format = VK_FORMAT_D32_SFLOAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
    };
    
    VkAttachmentReference depthAttachmentRef = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };
    
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pDepthStencilAttachment = &depthAttachmentRef
    };
    
    VkRenderPassCreateInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &depthAttachment,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    
    VK_CHECK(vkCreateRenderPass(vk.device, &renderPassInfo, NULL, &vk_shadowSystem.depthOnlyRenderPass));
}

void VK_CreateShadowPipelines(void) {
    // Create depth-only pipeline for shadow mapping
    // This would create the necessary pipelines with appropriate shaders
    // Implementation depends on shader system
}

void VK_AllocateShadowMapPool(void) {
    // Allocate a pool of shadow maps for reuse
    vk_shadowSystem.shadowMapPoolIndex = 0;
    vk_shadowSystem.maxShadowMaps = MAX_SHADOW_MAPS;
}

void VK_FreeShadowMapPool(void) {
    // Free the shadow map pool
    if (vk_shadowSystem.shadowMapPoolMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vk.device, vk_shadowSystem.shadowMapPoolMemory, NULL);
        vk_shadowSystem.shadowMapPoolMemory = VK_NULL_HANDLE;
    }
    
    for (int i = 0; i < MAX_SHADOW_MAPS * 2; i++) {
        if (vk_shadowSystem.shadowMapPool[i] != VK_NULL_HANDLE) {
            vkDestroyImage(vk.device, vk_shadowSystem.shadowMapPool[i], NULL);
            vk_shadowSystem.shadowMapPool[i] = VK_NULL_HANDLE;
        }
    }
}

void VK_DestroyCSM(csmData_t *csm) {
    if (!csm) {
        return;
    }
    
    // Destroy CSM resources
    for (int i = 0; i < MAX_CASCADE_COUNT; i++) {
        if (csm->cascades[i].framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(vk.device, csm->cascades[i].framebuffer, NULL);
            csm->cascades[i].framebuffer = VK_NULL_HANDLE;
        }
        if (csm->cascades[i].depthView != VK_NULL_HANDLE) {
            vkDestroyImageView(vk.device, csm->cascades[i].depthView, NULL);
            csm->cascades[i].depthView = VK_NULL_HANDLE;
        }
    }
    
    if (csm->shadowMapArrayView != VK_NULL_HANDLE) {
        vkDestroyImageView(vk.device, csm->shadowMapArrayView, NULL);
        csm->shadowMapArrayView = VK_NULL_HANDLE;
    }
    
    if (csm->shadowMapArray != VK_NULL_HANDLE) {
        vkDestroyImage(vk.device, csm->shadowMapArray, NULL);
        csm->shadowMapArray = VK_NULL_HANDLE;
    }
    
    if (csm->shadowMapMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vk.device, csm->shadowMapMemory, NULL);
        csm->shadowMapMemory = VK_NULL_HANDLE;
    }
    
    if (csm->shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(vk.device, csm->shadowSampler, NULL);
        csm->shadowSampler = VK_NULL_HANDLE;
    }
}

void VK_TransitionShadowMapsForSampling(void) {
    // Transition shadow maps to shader read-only optimal layout
    // This would use pipeline barriers to transition the image layouts
}

void R_SetupFrustumFromMatrix(cplane_t frustum[6], const mat4_t matrix) {
    // Extract frustum planes from matrix
    // Similar to ExtractFrustumPlanes but for 6 planes
    
    // Left plane
    frustum[0].normal[0] = matrix[3] + matrix[0];
    frustum[0].normal[1] = matrix[7] + matrix[4];
    frustum[0].normal[2] = matrix[11] + matrix[8];
    frustum[0].dist = matrix[15] + matrix[12];
    
    // Right plane
    frustum[1].normal[0] = matrix[3] - matrix[0];
    frustum[1].normal[1] = matrix[7] - matrix[4];
    frustum[1].normal[2] = matrix[11] - matrix[8];
    frustum[1].dist = matrix[15] - matrix[12];
    
    // Bottom plane
    frustum[2].normal[0] = matrix[3] + matrix[1];
    frustum[2].normal[1] = matrix[7] + matrix[5];
    frustum[2].normal[2] = matrix[11] + matrix[9];
    frustum[2].dist = matrix[15] + matrix[13];
    
    // Top plane
    frustum[3].normal[0] = matrix[3] - matrix[1];
    frustum[3].normal[1] = matrix[7] - matrix[5];
    frustum[3].normal[2] = matrix[11] - matrix[9];
    frustum[3].dist = matrix[15] - matrix[13];
    
    // Near plane
    frustum[4].normal[0] = matrix[3] + matrix[2];
    frustum[4].normal[1] = matrix[7] + matrix[6];
    frustum[4].normal[2] = matrix[11] + matrix[10];
    frustum[4].dist = matrix[15] + matrix[14];
    
    // Far plane
    frustum[5].normal[0] = matrix[3] - matrix[2];
    frustum[5].normal[1] = matrix[7] - matrix[6];
    frustum[5].normal[2] = matrix[11] - matrix[10];
    frustum[5].dist = matrix[15] - matrix[14];
    
    // Normalize planes
    for (int i = 0; i < 6; i++) {
        float length = VectorLength(frustum[i].normal);
        if (length > 0) {
            VectorScale(frustum[i].normal, 1.0f / length, frustum[i].normal);
            frustum[i].dist /= length;
        }
    }
}

// Matrix helper functions
void Mat4_Copy(const mat4_t src, mat4_t dst) {
    Com_Memcpy(dst, src, sizeof(mat4_t));
}

void Mat4_Multiply(const mat4_t a, const mat4_t b, mat4_t out) {
    mat4_t temp;
    
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp[i*4 + j] = 0;
            for (int k = 0; k < 4; k++) {
                temp[i*4 + j] += a[i*4 + k] * b[k*4 + j];
            }
        }
    }
    
    Com_Memcpy(out, temp, sizeof(mat4_t));
}

void Mat4_Inverse(const mat4_t m, mat4_t out) {
    // Simplified inverse for view/projection matrices
    mat4_t tmp;
    float det;
    int i, j;
    
    // Calculate determinant (simplified)
    det = m[0] * m[5] * m[10] * m[15] -
          m[0] * m[5] * m[11] * m[14] -
          m[0] * m[6] * m[9] * m[15] +
          m[0] * m[6] * m[11] * m[13];
    
    if (fabs(det) < 0.0001f) {
        // Matrix is singular, return identity
        Mat4_Identity(out);
        return;
    }
    
    // Transpose rotation part
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            tmp[j*4 + i] = m[i*4 + j];
        }
    }
    
    // Transform translation
    tmp[12] = -(m[12] * tmp[0] + m[13] * tmp[4] + m[14] * tmp[8]);
    tmp[13] = -(m[12] * tmp[1] + m[13] * tmp[5] + m[14] * tmp[9]);
    tmp[14] = -(m[12] * tmp[2] + m[13] * tmp[6] + m[14] * tmp[10]);
    
    // Set last row
    tmp[3] = tmp[7] = tmp[11] = 0.0f;
    tmp[15] = 1.0f;
    
    Com_Memcpy(out, tmp, sizeof(mat4_t));
}

void Mat4_Identity(mat4_t m) {
    Com_Memset(m, 0, sizeof(mat4_t));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void Mat4_TransformVec4(const mat4_t m, const vec4_t in, vec4_t out) {
    vec4_t temp;
    
    for (int i = 0; i < 4; i++) {
        temp[i] = 0;
        for (int j = 0; j < 4; j++) {
            temp[i] += m[i*4 + j] * in[j];
        }
    }
    
    Vector4Copy(temp, out);
}

// Helper functions for CSM
void BuildLightViewMatrix(mat4_t result, const vec3_t eye, const vec3_t center, const vec3_t up) {
    vec3_t f, s, u;
    
    VectorSubtract(center, eye, f);
    VectorNormalize(f);
    
    CrossProduct(f, up, s);
    VectorNormalize(s);
    
    CrossProduct(s, f, u);
    
    result[0] = s[0];
    result[4] = s[1];
    result[8] = s[2];
    
    result[1] = u[0];
    result[5] = u[1];
    result[9] = u[2];
    
    result[2] = -f[0];
    result[6] = -f[1];
    result[10] = -f[2];
    
    result[12] = -DotProduct(s, eye);
    result[13] = -DotProduct(u, eye);
    result[14] = DotProduct(f, eye);
    
    result[3] = 0;
    result[7] = 0;
    result[11] = 0;
    result[15] = 1;
}

void BuildOrthographicMatrix(mat4_t result, float left, float right, float bottom, float top, float nearPlane, float farPlane) {
    Com_Memset(result, 0, sizeof(mat4_t));
    
    result[0] = 2.0f / (right - left);
    result[5] = 2.0f / (top - bottom);
    result[10] = -2.0f / (farPlane - nearPlane);
    
    result[12] = -(right + left) / (right - left);
    result[13] = -(top + bottom) / (top - bottom);
    result[14] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    result[15] = 1.0f;
}

void CalculateFrustumCorners(vec3_t corners[8], const mat4_t invViewProj) {
    // NDC corners
    vec4_t ndcCorners[8] = {
        {-1, -1, -1, 1}, {1, -1, -1, 1}, {1, 1, -1, 1}, {-1, 1, -1, 1},
        {-1, -1, 1, 1}, {1, -1, 1, 1}, {1, 1, 1, 1}, {-1, 1, 1, 1}
    };
    
    for (int i = 0; i < 8; i++) {
        vec4_t worldPos;
        Mat4_TransformVec4(invViewProj, ndcCorners[i], worldPos);
        
        // Perspective divide
        if (worldPos[3] != 0) {
            corners[i][0] = worldPos[0] / worldPos[3];
            corners[i][1] = worldPos[1] / worldPos[3];
            corners[i][2] = worldPos[2] / worldPos[3];
        }
    }
}

void ExtractFrustumPlanes(cplane_t planes[5], const mat4_t viewProj) {
    // Extract frustum planes from view-projection matrix
    // CSM only needs 5 planes (no far plane)
    
    // Left plane
    planes[0].normal[0] = viewProj[3] + viewProj[0];
    planes[0].normal[1] = viewProj[7] + viewProj[4];
    planes[0].normal[2] = viewProj[11] + viewProj[8];
    planes[0].dist = viewProj[15] + viewProj[12];
    
    // Right plane
    planes[1].normal[0] = viewProj[3] - viewProj[0];
    planes[1].normal[1] = viewProj[7] - viewProj[4];
    planes[1].normal[2] = viewProj[11] - viewProj[8];
    planes[1].dist = viewProj[15] - viewProj[12];
    
    // Bottom plane
    planes[2].normal[0] = viewProj[3] + viewProj[1];
    planes[2].normal[1] = viewProj[7] + viewProj[5];
    planes[2].normal[2] = viewProj[11] + viewProj[9];
    planes[2].dist = viewProj[15] + viewProj[13];
    
    // Top plane
    planes[3].normal[0] = viewProj[3] - viewProj[1];
    planes[3].normal[1] = viewProj[7] - viewProj[5];
    planes[3].normal[2] = viewProj[11] - viewProj[9];
    planes[3].dist = viewProj[15] - viewProj[13];
    
    // Near plane
    planes[4].normal[0] = viewProj[3] + viewProj[2];
    planes[4].normal[1] = viewProj[7] + viewProj[6];
    planes[4].normal[2] = viewProj[11] + viewProj[10];
    planes[4].dist = viewProj[15] + viewProj[14];
    
    // Normalize planes
    for (int i = 0; i < 5; i++) {
        float length = VectorLength(planes[i].normal);
        if (length > 0) {
            VectorScale(planes[i].normal, 1.0f / length, planes[i].normal);
            planes[i].dist /= length;
        }
    }
}