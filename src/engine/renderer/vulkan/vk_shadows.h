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

#ifndef VK_SHADOWS_H
#define VK_SHADOWS_H

#include "vk.h"
#include "../lighting/tr_light.h"
#include "../core/tr_common.h"

#define MAX_CASCADE_COUNT 4
#define SHADOW_MAP_SIZE 2048
#define MAX_SHADOW_MAPS 32
#define CUBE_MAP_SIZE 512

// Forward declarations
typedef struct renderLight_s renderLight_t;
typedef struct drawSurf_s drawSurf_t;

// Cascaded Shadow Map data
typedef struct csmData_s {
    // Per-cascade data
    struct {
        mat4_t          viewProjMatrix;
        vec4_t          splitDistances;    // Near/far for this cascade
        VkImageView     depthView;
        VkFramebuffer   framebuffer;
        cplane_t        frustum[5];        // Cascade frustum for culling
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
    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorSet     descriptorSet;
    
    // Configuration
    float               cascadeSplitLambda;    // Cascade distribution (0.5-0.95)
    float               shadowDistance;        // Maximum shadow distance
    int                 cascadeCount;          // Active cascades
    int                 resolution;            // Shadow map resolution
    
    // Statistics
    int                 numShadowCasters;
    int                 numCascadeDrawCalls;
} csmData_t;

// Point light shadow map data
typedef struct pointShadowMap_s {
    VkImage             cubeMap;
    VkDeviceMemory      cubeMapMemory;
    VkImageView         cubeMapView;
    VkImageView         faceViews[6];      // Individual cube faces
    VkFramebuffer       framebuffers[6];
    
    mat4_t              viewMatrices[6];   // View matrix per face
    mat4_t              projMatrix;        // 90 degree FOV projection
    
    float               nearPlane;
    float               farPlane;
    int                 resolution;
    
    // Reference to the light
    renderLight_t       *light;
    int                 lastUpdateFrame;
    qboolean            needsUpdate;
} pointShadowMap_t;

// Spot light shadow map data
typedef struct spotShadowMap_s {
    VkImage             depthImage;
    VkDeviceMemory      depthMemory;
    VkImageView         depthView;
    VkFramebuffer       framebuffer;
    
    mat4_t              viewProjMatrix;
    float               nearPlane;
    float               farPlane;
    int                 resolution;
    
    // Reference to the light
    renderLight_t       *light;
    int                 lastUpdateFrame;
    qboolean            needsUpdate;
} spotShadowMap_t;

// Shadow system manager
typedef struct shadowSystem_s {
    // Cascaded shadows for sun/directional lights
    csmData_t           csm;
    qboolean            csmEnabled;
    
    // Point light shadows
    pointShadowMap_t    pointShadows[MAX_SHADOW_MAPS];
    int                 numPointShadows;
    
    // Spot light shadows
    spotShadowMap_t     spotShadows[MAX_SHADOW_MAPS];
    int                 numSpotShadows;
    
    // Shadow render passes
    VkRenderPass        depthOnlyRenderPass;
    VkRenderPass        cubeMapRenderPass;
    
    // Shadow pipelines
    VkPipeline          depthOnlyPipeline;
    VkPipeline          cubeMapPipeline;
    VkPipelineLayout    shadowPipelineLayout;
    
    // Descriptor sets for shadow sampling
    VkDescriptorSetLayout shadowSamplerLayout;
    VkDescriptorPool    descriptorPool;
    
    // Shadow map pool
    VkImage             shadowMapPool[MAX_SHADOW_MAPS * 2];
    VkDeviceMemory      shadowMapPoolMemory;
    int                 shadowMapPoolIndex;
    
    // Configuration
    int                 maxShadowMaps;
    int                 shadowMapSize;
    float               shadowBias;
    float               shadowSoftness;
    int                 pcfKernelSize;
    
    // Statistics
    int                 totalShadowMaps;
    int                 activeShadowMaps;
    double              shadowRenderTime;
} shadowSystem_t;

// Function declarations

// System initialization
void VK_InitShadowSystem(void);
void VK_ShutdownShadowSystem(void);
void VK_ResetShadowMaps(void);

// CSM functions
void VK_SetupCSM(csmData_t *csm, int cascadeCount, float shadowDistance);
void VK_CalculateCSMSplits(csmData_t *csm, float nearPlane, float farPlane);
void VK_UpdateCSMMatrices(csmData_t *csm, const vec3_t lightDir, const mat4_t viewMatrix, const mat4_t projMatrix);
void VK_RenderCSMCascade(int cascade, csmData_t *csm, drawSurf_t **surfaces, int numSurfaces);
void VK_RenderCSM(csmData_t *csm);

// Point shadow functions
pointShadowMap_t* VK_AllocatePointShadowMap(renderLight_t *light);
void VK_FreePointShadowMap(pointShadowMap_t *shadow);
void VK_RenderPointShadowMap(renderLight_t *light, pointShadowMap_t *shadowMap);
void VK_UpdatePointShadowMatrices(pointShadowMap_t *shadow);

// Spot shadow functions
spotShadowMap_t* VK_AllocateSpotShadowMap(renderLight_t *light);
void VK_FreeSpotShadowMap(spotShadowMap_t *shadow);
void VK_RenderSpotShadowMap(renderLight_t *light, spotShadowMap_t *shadowMap);
void VK_UpdateSpotShadowMatrix(spotShadowMap_t *shadow);

// Shadow rendering
void VK_BeginShadowPass(VkFramebuffer framebuffer, int width, int height);
void VK_EndShadowPass(void);
void VK_RenderShadowSurface(drawSurf_t *surface, const mat4_t viewProjMatrix);
qboolean VK_SurfaceInFrustum(drawSurf_t *surface, const cplane_t frustum[5]);
void VK_UpdateLightShadowMap(renderLight_t *light);

// Shadow map management
VkImage VK_AllocateShadowMapImage(int width, int height, VkFormat format, VkImageUsageFlags usage);
void VK_FreeShadowMapImage(VkImage image);
VkImageView VK_CreateShadowMapView(VkImage image, VkFormat format, VkImageViewType viewType, uint32_t baseLayer, uint32_t layerCount);

// Shadow sampling
VkDescriptorSet VK_GetShadowDescriptorSet(void);
void VK_BindShadowMaps(VkCommandBuffer cmd, VkPipelineLayout layout);
void VK_SetShadowUniforms(const mat4_t *shadowMatrices, const vec4_t *cascadeSplits, int numCascades);

// Configuration
void VK_SetShadowMapSize(int size);
void VK_SetShadowDistance(float distance);
void VK_SetShadowSoftness(float softness);
void VK_SetShadowBias(float bias);

// Debug
void VK_DrawShadowFrustum(const cplane_t frustum[5], const vec4_t color);
void VK_DrawCSMSplits(const csmData_t *csm);
void VK_GetShadowStats(int *numMaps, int *memoryUsed, double *renderTime);

// CSM update function
void VK_UpdateCSM(struct viewParms_s *viewParms);

// Helper functions for matrix operations and CSM
void BuildLightViewMatrix(mat4_t result, const vec3_t eye, const vec3_t center, const vec3_t up);
void BuildOrthographicMatrix(mat4_t result, float left, float right, float bottom, float top, float nearPlane, float farPlane);
void CalculateFrustumCorners(vec3_t corners[8], const mat4_t invViewProj);
void ExtractFrustumPlanes(cplane_t planes[5], const mat4_t viewProj);

// Global shadow system
extern shadowSystem_t vk_shadowSystem;

// CVars
extern cvar_t *r_shadows;
extern cvar_t *r_shadowMapSize;
extern cvar_t *r_shadowCascadeCount;
extern cvar_t *r_shadowDistance;
extern cvar_t *r_shadowSoftness;
extern cvar_t *r_shadowBias;
extern cvar_t *r_shadowFilter;

#endif // VK_SHADOWS_H