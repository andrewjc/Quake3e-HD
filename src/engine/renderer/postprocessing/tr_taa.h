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
// tr_taa.h - Temporal Anti-Aliasing system

#ifndef __TR_TAA_H
#define __TR_TAA_H

#include "../core/tr_local.h"
#include "vulkan/vulkan.h"

// TAA configuration
#define TAA_HISTORY_SAMPLES     8
#define TAA_MAX_VELOCITY        32.0f
#define TAA_SUBPIXEL_JITTER     0.5f
#define TAA_SHARPNESS_DEFAULT   0.5f

// TAA quality modes
typedef enum {
    TAA_QUALITY_OFF = 0,
    TAA_QUALITY_LOW,
    TAA_QUALITY_MEDIUM,
    TAA_QUALITY_HIGH,
    TAA_QUALITY_ULTRA,
} taaQuality_t;

// Jitter patterns for subpixel sampling
typedef enum {
    TAA_JITTER_HALTON = 0,
    TAA_JITTER_R2,
    TAA_JITTER_HAMMERSLEY,
    TAA_JITTER_CUSTOM,
} taaJitterPattern_t;

// TAA configuration structure
typedef struct taaConfig_s {
    taaQuality_t        quality;
    taaJitterPattern_t  jitterPattern;
    
    // Temporal parameters
    float               feedbackMin;           // Minimum history feedback (0.0-1.0)
    float               feedbackMax;           // Maximum history feedback (0.0-1.0)
    float               motionBlending;        // Motion-based blending factor
    float               sharpness;             // Post-TAA sharpening strength
    
    // Velocity parameters
    float               velocityScale;         // Velocity buffer scale
    float               maxVelocity;           // Maximum velocity clamp
    qboolean            useMotionVectors;      // Use motion vectors for reprojection
    
    // Quality settings
    qboolean            useVarianceClipping;   // Variance clipping for ghosting reduction
    qboolean            useYCoCg;             // Use YCoCg color space
    qboolean            useCatmullRom;        // Catmull-Rom filtering for history
    qboolean            useSMAA;              // Combine with SMAA for edges
    
    // Jitter settings
    vec2_t              jitterScale;          // Jitter scale in pixels
    uint32_t            jitterIndex;          // Current jitter sample index
    uint32_t            jitterSamples;        // Number of jitter samples
} taaConfig_t;

// TAA frame data
typedef struct taaFrameData_s {
    // Matrices for reprojection
    mat4_t              viewMatrix;
    mat4_t              projMatrix;
    mat4_t              viewProjMatrix;
    mat4_t              prevViewProjMatrix;
    mat4_t              invViewProjMatrix;
    
    // Jitter offset for current frame
    vec2_t              jitterOffset;
    vec2_t              prevJitterOffset;
    
    // Frame timing
    float               deltaTime;
    uint32_t            frameNumber;
} taaFrameData_t;

// TAA resources
typedef struct taaResources_s {
    // History buffers (double-buffered)
    VkImage             historyImage[2];
    VkImageView         historyView[2];
    VkDeviceMemory      historyMemory[2];
    uint32_t            currentHistory;
    
    // Velocity buffer
    VkImage             velocityImage;
    VkImageView         velocityView;
    VkDeviceMemory      velocityMemory;
    
    // Temporal accumulation buffer
    VkImage             accumulationImage;
    VkImageView         accumulationView;
    VkDeviceMemory      accumulationMemory;
    
    // Neighborhood data for variance clipping
    VkImage             neighborhoodImage;
    VkImageView         neighborhoodView;
    VkDeviceMemory      neighborhoodMemory;
} taaResources_t;

// TAA pipeline
typedef struct taaPipeline_s {
    // Compute pipelines
    VkPipeline          velocityPipeline;
    VkPipeline          resolvePipeline;
    VkPipeline          sharpenPipeline;
    
    // Pipeline layouts
    VkPipelineLayout    velocityLayout;
    VkPipelineLayout    resolveLayout;
    VkPipelineLayout    sharpenLayout;
    
    // Descriptor sets
    VkDescriptorSet     velocityDescSet;
    VkDescriptorSet     resolveDescSet;
    VkDescriptorSet     sharpenDescSet;
    
    // Descriptor set layouts
    VkDescriptorSetLayout velocitySetLayout;
    VkDescriptorSetLayout resolveSetLayout;
    VkDescriptorSetLayout sharpenSetLayout;
} taaPipeline_t;

// TAA state
typedef struct taaState_s {
    qboolean            initialized;
    qboolean            enabled;
    
    taaConfig_t         config;
    taaFrameData_t      frameData;
    taaResources_t      resources;
    taaPipeline_t       pipeline;
    
    // Jitter sequences
    vec2_t             *jitterSequence;
    uint32_t            jitterLength;
    
    // Statistics
    float               averageMotion;
    float               convergenceRate;
    uint32_t            ghostingPixels;
    uint32_t            historyRejections;
} taaState_t;

// Global TAA state
extern taaState_t taaState;

// Initialization and shutdown
qboolean R_InitTAA( void );
void R_ShutdownTAA( void );

// Configuration
void R_SetTAAQuality( taaQuality_t quality );
void R_SetTAAConfig( const taaConfig_t *config );
void R_GetTAAConfig( taaConfig_t *config );

// Frame management
void R_BeginTAAFrame( void );
void R_EndTAAFrame( void );
void R_UpdateTAAMatrices( const viewParms_t *viewParms );

// Jitter generation
void R_GenerateJitterSequence( taaJitterPattern_t pattern, uint32_t samples );
void R_GetTAAJitter( uint32_t frameNumber, vec2_t jitter );
void R_ApplyTAAJitter( mat4_t projMatrix, const vec2_t jitter );

// Velocity buffer generation
void R_GenerateVelocityBuffer( VkCommandBuffer cmd );
void R_UpdateVelocityBuffer( VkCommandBuffer cmd, const drawSurf_t *drawSurf );

// TAA resolve
void R_ResolveTAA( VkCommandBuffer cmd, VkImage sourceImage, VkImage destImage );
void R_ApplyTemporalFilter( VkCommandBuffer cmd );

// History management
void R_UpdateTAAHistory( VkCommandBuffer cmd );
void R_ClearTAAHistory( void );
void R_ValidateHistory( VkCommandBuffer cmd );

// Sharpening
void R_ApplyTAASharpening( VkCommandBuffer cmd, float sharpness );

// Helper functions
void R_ReprojectPixel( const vec2_t pixel, vec2_t prevPixel, const taaFrameData_t *frame );
float R_CalculateConfidence( const vec2_t velocity, float depth );
void R_ClampHistory( vec4_t color, const vec4_t neighborMin, const vec4_t neighborMax );

// Debug
void R_DrawTAADebug( void );
void R_VisualizeTAAVelocity( VkCommandBuffer cmd );
void R_VisualizeTAAHistory( VkCommandBuffer cmd );

// Integration with other systems
void R_TAACombineWithSSR( VkCommandBuffer cmd );
void R_TAACombineWithDOF( VkCommandBuffer cmd );

#endif // __TR_TAA_H