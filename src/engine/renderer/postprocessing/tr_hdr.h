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
// tr_hdr.h - HDR Rendering Pipeline

#ifndef __TR_HDR_H
#define __TR_HDR_H

#include "../core/tr_local.h"
#include "vulkan/vulkan.h"

// HDR configuration
#define HDR_LUMINANCE_BINS      256
#define HDR_ADAPTATION_RATE     2.0f
#define HDR_MIN_LUMINANCE       0.001f
#define HDR_MAX_LUMINANCE       10000.0f
#define HDR_BLOOM_LEVELS        6

// Tone mapping operators
typedef enum {
    TONEMAP_REINHARD = 0,
    TONEMAP_REINHARD_EXTENDED,
    TONEMAP_ACES,
    TONEMAP_UNCHARTED2,
    TONEMAP_EXPOSURE,
    TONEMAP_FILMIC,
} tonemapOperator_t;

// HDR configuration
typedef struct hdrConfig_s {
    qboolean            enabled;
    tonemapOperator_t   tonemapper;
    
    // Exposure parameters
    float               exposureValue;
    float               exposureCompensation;
    qboolean            autoExposure;
    float               adaptationRate;
    float               minLuminance;
    float               maxLuminance;
    float               keyValue;
    
    // Bloom parameters
    qboolean            bloomEnabled;
    float               bloomThreshold;
    float               bloomIntensity;
    float               bloomRadius;
    uint32_t            bloomLevels;
    
    // Color grading
    float               saturation;
    float               contrast;
    vec3_t              colorBalance;
    vec3_t              lift;
    vec3_t              gamma;
    vec3_t              gain;
} hdrConfig_t;

// HDR resources
typedef struct hdrResources_s {
    // HDR render target
    VkImage             hdrImage;
    VkImageView         hdrView;
    VkDeviceMemory      hdrMemory;
    VkFormat            hdrFormat;
    
    // Luminance histogram
    VkBuffer            histogramBuffer;
    VkDeviceMemory      histogramMemory;
    
    // Average luminance
    VkBuffer            avgLuminanceBuffer[2];
    VkDeviceMemory      avgLuminanceMemory[2];
    uint32_t            currentLuminance;
    
    // Bloom mip chain
    VkImage             bloomImage;
    VkImageView         bloomViews[HDR_BLOOM_LEVELS];
    VkDeviceMemory      bloomMemory;
    
    // LUT for color grading
    VkImage             lutImage;
    VkImageView         lutView;
    VkDeviceMemory      lutMemory;
} hdrResources_t;

// HDR pipeline
typedef struct hdrPipeline_s {
    // Compute pipelines
    VkPipeline          histogramPipeline;
    VkPipeline          avgLuminancePipeline;
    VkPipeline          brightPassPipeline;
    VkPipeline          bloomDownPipeline;
    VkPipeline          bloomUpPipeline;
    VkPipeline          tonemapPipeline;
    
    // Pipeline layouts
    VkPipelineLayout    histogramLayout;
    VkPipelineLayout    avgLuminanceLayout;
    VkPipelineLayout    brightPassLayout;
    VkPipelineLayout    bloomDownLayout;
    VkPipelineLayout    bloomUpLayout;
    VkPipelineLayout    tonemapLayout;
    
    // Descriptor sets
    VkDescriptorSet     histogramDescSet;
    VkDescriptorSet     avgLuminanceDescSet;
    VkDescriptorSet     brightPassDescSet;
    VkDescriptorSet     bloomDescSets[HDR_BLOOM_LEVELS];
    VkDescriptorSet     tonemapDescSet;
} hdrPipeline_t;

// HDR state
typedef struct hdrState_s {
    qboolean        initialized;
    hdrConfig_t     config;
    hdrResources_t  resources;
    hdrPipeline_t   pipeline;
    
    // Current frame data
    float           currentExposure;
    float           targetExposure;
    float           avgLuminance;
    float           deltaTime;
    
    // Statistics
    float           minLuminanceFrame;
    float           maxLuminanceFrame;
    uint32_t        overexposedPixels;
    uint32_t        underexposedPixels;
} hdrState_t;

// Global HDR state
extern hdrState_t hdrState;

// Initialization
qboolean R_InitHDR( void );
void R_ShutdownHDR( void );

// Configuration
void R_SetHDRConfig( const hdrConfig_t *config );
void R_GetHDRConfig( hdrConfig_t *config );
void R_SetTonemapper( tonemapOperator_t tonemapper );

// Frame processing
void R_BeginHDRFrame( void );
void R_EndHDRFrame( void );

// Exposure control
void R_CalculateAutoExposure( VkCommandBuffer cmd );
void R_UpdateExposure( float deltaTime );
void R_SetManualExposure( float ev );

// Bloom
void R_ExtractBrightPass( VkCommandBuffer cmd );
void R_GenerateBloomMips( VkCommandBuffer cmd );
void R_CompositeBloom( VkCommandBuffer cmd );

// Tone mapping
void R_ApplyToneMapping( VkCommandBuffer cmd, VkImage sourceImage, VkImage destImage );
void R_ApplyColorGrading( VkCommandBuffer cmd );

// LUT generation
void R_GenerateColorLUT( VkCommandBuffer cmd );
void R_LoadColorLUT( const char *filename );

// Debug
void R_DrawHDRDebug( void );
void R_VisualizeHistogram( VkCommandBuffer cmd );
void R_ShowExposureZones( VkCommandBuffer cmd );

#endif // __TR_HDR_H