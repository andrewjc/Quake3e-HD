/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

This file is part of Quake3e-HD.

Quake3e-HD is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Quake3e-HD is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake3e-HD; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
===========================================================================
*/
// tr_volumetric.h - Volumetric Rendering System

#ifndef __TR_VOLUMETRIC_H
#define __TR_VOLUMETRIC_H

#include "../core/tr_local.h"
#include "vulkan/vulkan.h"

// Volumetric configuration
#define VOLUMETRIC_GRID_SIZE_X      160
#define VOLUMETRIC_GRID_SIZE_Y      90
#define VOLUMETRIC_GRID_SIZE_Z      128
#define VOLUMETRIC_MAX_LIGHTS       128
#define VOLUMETRIC_HISTORY_SAMPLES  8

// Volumetric quality
typedef enum {
    VOLUMETRIC_QUALITY_OFF = 0,
    VOLUMETRIC_QUALITY_LOW,
    VOLUMETRIC_QUALITY_MEDIUM,
    VOLUMETRIC_QUALITY_HIGH,
    VOLUMETRIC_QUALITY_ULTRA,
} volumetricQuality_t;

// Volumetric light type
typedef enum {
    VOLUMETRIC_LIGHT_DIRECTIONAL = 0,
    VOLUMETRIC_LIGHT_POINT,
    VOLUMETRIC_LIGHT_SPOT,
} volumetricLightType_t;

// Volumetric light structure
typedef struct volumetricLight_s {
    volumetricLightType_t   type;
    vec3_t                  position;
    vec3_t                  direction;
    vec3_t                  color;
    float                   intensity;
    float                   range;
    float                   innerCone;
    float                   outerCone;
    qboolean                castShadows;
} volumetricLight_t;

// Volumetric configuration
typedef struct volumetricConfig_s {
    volumetricQuality_t quality;
    
    // Grid parameters
    uint32_t        gridSizeX;
    uint32_t        gridSizeY;
    uint32_t        gridSizeZ;
    float           depthSlicing;
    
    // Scattering parameters
    float           scatteringCoeff;
    float           absorptionCoeff;
    float           anisotropy;
    float           density;
    
    // Quality settings
    uint32_t        marchSteps;
    float           marchStepSize;
    qboolean        useTemporal;
    float           temporalBlend;
    
    // Shadow parameters
    qboolean        enableShadows;
    uint32_t        shadowSamples;
    float           shadowDensity;
} volumetricConfig_t;

// Volumetric resources
typedef struct volumetricResources_s {
    // 3D scattering volume
    VkImage         scatteringVolume;
    VkImageView     scatteringView;
    VkDeviceMemory  scatteringMemory;
    
    // 3D density volume
    VkImage         densityVolume;
    VkImageView     densityView;
    VkDeviceMemory  densityMemory;
    
    // Accumulated lighting
    VkImage         lightingVolume;
    VkImageView     lightingView;
    VkDeviceMemory  lightingMemory;
    
    // Temporal history
    VkImage         historyVolume[2];
    VkImageView     historyView[2];
    VkDeviceMemory  historyMemory[2];
    uint32_t        currentHistory;
    
    // Light buffer
    VkBuffer        lightBuffer;
    VkDeviceMemory  lightMemory;
} volumetricResources_t;

// Volumetric pipeline
typedef struct volumetricPipeline_s {
    VkPipeline          densityPipeline;
    VkPipeline          scatteringPipeline;
    VkPipeline          integrationPipeline;
    VkPipeline          compositePipeline;
    
    VkPipelineLayout    densityLayout;
    VkPipelineLayout    scatteringLayout;
    VkPipelineLayout    integrationLayout;
    VkPipelineLayout    compositeLayout;
    
    VkDescriptorSet     densityDescSet;
    VkDescriptorSet     scatteringDescSet;
    VkDescriptorSet     integrationDescSet;
    VkDescriptorSet     compositeDescSet;
} volumetricPipeline_t;

// Volumetric state
typedef struct volumetricState_s {
    qboolean            initialized;
    qboolean            enabled;
    
    volumetricConfig_t  config;
    volumetricResources_t resources;
    volumetricPipeline_t pipeline;
    
    // Lights
    volumetricLight_t   lights[VOLUMETRIC_MAX_LIGHTS];
    uint32_t            numLights;
    
    // Frame data
    mat4_t              viewMatrix;
    mat4_t              projMatrix;
    mat4_t              invViewProjMatrix;
    vec3_t              viewOrigin;
    
    // Statistics
    float               averageDensity;
    uint32_t            lightsProcessed;
} volumetricState_t;

// Global volumetric state
extern volumetricState_t volumetricState;

// Initialization
qboolean R_InitVolumetric( void );
void R_ShutdownVolumetric( void );

// Configuration
void R_SetVolumetricQuality( volumetricQuality_t quality );
void R_SetVolumetricConfig( const volumetricConfig_t *config );

// Light management
void R_AddVolumetricLight( const volumetricLight_t *light );
void R_ClearVolumetricLights( void );
void R_UpdateVolumetricLights( VkCommandBuffer cmd );

// Rendering
void R_RenderVolumetricFog( VkCommandBuffer cmd );
void R_CompositeVolumetricFog( VkCommandBuffer cmd, VkImage targetImage );

// Density injection
void R_InjectVolumetricDensity( VkCommandBuffer cmd );
void R_UpdateDensityVolume( VkCommandBuffer cmd );

// Scattering
void R_ComputeVolumetricScattering( VkCommandBuffer cmd );
void R_IntegrateVolumetricLighting( VkCommandBuffer cmd );

// Temporal
void R_ApplyVolumetricTemporal( VkCommandBuffer cmd );
void R_UpdateVolumetricHistory( VkCommandBuffer cmd );

// Debug
void R_DrawVolumetricDebug( void );
void R_VisualizeVolumetricSlices( VkCommandBuffer cmd );

#endif // __TR_VOLUMETRIC_H