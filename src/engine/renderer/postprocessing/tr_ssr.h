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
// tr_ssr.h - Screen-Space Reflections

#ifndef __TR_SSR_H
#define __TR_SSR_H

#include "../core/tr_local.h"
#include "vulkan/vulkan.h"

// SSR configuration
#define SSR_MAX_STEPS           64
#define SSR_MAX_BINARY_STEPS    16
#define SSR_MAX_DISTANCE        1000.0f
#define SSR_THICKNESS          0.1f

// SSR quality levels
typedef enum {
    SSR_QUALITY_OFF = 0,
    SSR_QUALITY_LOW,
    SSR_QUALITY_MEDIUM,
    SSR_QUALITY_HIGH,
    SSR_QUALITY_ULTRA,
} ssrQuality_t;

// SSR tracing modes
typedef enum {
    SSR_TRACE_LINEAR = 0,
    SSR_TRACE_HIERARCHICAL,
    SSR_TRACE_ADAPTIVE,
} ssrTraceMode_t;

// SSR configuration
typedef struct ssrConfig_s {
    ssrQuality_t    quality;
    ssrTraceMode_t  traceMode;
    
    // Ray marching parameters
    uint32_t        maxSteps;
    uint32_t        binarySearchSteps;
    float           maxDistance;
    float           thickness;
    float           strideZCutoff;
    
    // Quality parameters
    float           roughnessCutoff;
    float           fadeDist;
    float           fadeAngle;
    float           reflectionScale;
    
    // Temporal parameters
    qboolean        useTemporal;
    float           temporalWeight;
    float           temporalClamp;
    
    // Denoising
    qboolean        useDenoising;
    float           denoiseSigma;
    uint32_t        denoiseRadius;
} ssrConfig_t;

// SSR resources
typedef struct ssrResources_s {
    // Reflection buffer
    VkImage         reflectionImage;
    VkImageView     reflectionView;
    VkDeviceMemory  reflectionMemory;
    
    // HiZ buffer for hierarchical tracing
    VkImage         hiZImage;
    VkImageView     hiZView;
    VkDeviceMemory  hiZMemory;
    uint32_t        hiZMipLevels;
    
    // Temporal history
    VkImage         historyImage[2];
    VkImageView     historyView[2];
    VkDeviceMemory  historyMemory[2];
    uint32_t        currentHistory;
    
    // Denoised output
    VkImage         denoisedImage;
    VkImageView     denoisedView;
    VkDeviceMemory  denoisedMemory;
} ssrResources_t;

// SSR pipeline
typedef struct ssrPipeline_s {
    // Compute pipelines
    VkPipeline          tracePipeline;
    VkPipeline          resolvePipeline;
    VkPipeline          denoisePipeline;
    VkPipeline          hiZGenPipeline;
    
    // Pipeline layouts
    VkPipelineLayout    traceLayout;
    VkPipelineLayout    resolveLayout;
    VkPipelineLayout    denoiseLayout;
    VkPipelineLayout    hiZGenLayout;
    
    // Descriptor sets
    VkDescriptorSet     traceDescSet;
    VkDescriptorSet     resolveDescSet;
    VkDescriptorSet     denoiseDescSet;
    VkDescriptorSet     hiZGenDescSet;
} ssrPipeline_t;

// SSR state
typedef struct ssrState_s {
    qboolean        initialized;
    qboolean        enabled;
    
    ssrConfig_t     config;
    ssrResources_t  resources;
    ssrPipeline_t   pipeline;
    
    // Frame data
    mat4_t          viewMatrix;
    mat4_t          projMatrix;
    mat4_t          invProjMatrix;
    mat4_t          prevViewProjMatrix;
    
    // Statistics
    uint32_t        raysTraced;
    uint32_t        raysHit;
    float           averageSteps;
} ssrState_t;

// Global SSR state
extern ssrState_t ssrState;

// Initialization
qboolean R_InitSSR( void );
void R_ShutdownSSR( void );

// Configuration
void R_SetSSRQuality( ssrQuality_t quality );
void R_SetSSRConfig( const ssrConfig_t *config );
void R_GetSSRConfig( ssrConfig_t *config );

// Frame processing
void R_BeginSSRFrame( const viewParms_t *viewParms );
void R_EndSSRFrame( void );

// HiZ generation
void R_GenerateHiZBuffer( VkCommandBuffer cmd, VkImage depthImage );
void R_UpdateHiZMipChain( VkCommandBuffer cmd );

// Ray tracing
void R_TraceSSRReflections( VkCommandBuffer cmd );
void R_ResolveSSRReflections( VkCommandBuffer cmd );

// Denoising
void R_DenoiseSSR( VkCommandBuffer cmd );
void R_ApplySSRTemporal( VkCommandBuffer cmd );

// Integration
void R_ApplySSR( VkCommandBuffer cmd, VkImage targetImage );
void R_CompositeSSR( VkCommandBuffer cmd, VkImage sceneImage );

// Debug
void R_DrawSSRDebug( void );
void R_VisualizeSSRBuffer( VkCommandBuffer cmd );

#endif // __TR_SSR_H