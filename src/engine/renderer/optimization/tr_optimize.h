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
// tr_optimize.h - Render Optimization System

#ifndef __TR_OPTIMIZE_H
#define __TR_OPTIMIZE_H

#include "../core/tr_local.h"

typedef enum {
    OPT_LEVEL_OFF = 0,
    OPT_LEVEL_CONSERVATIVE,
    OPT_LEVEL_BALANCED,
    OPT_LEVEL_AGGRESSIVE,
    OPT_LEVEL_MAXIMUM
} optimizationLevel_t;

typedef enum {
    OPT_TECHNIQUE_BATCHING = 0,
    OPT_TECHNIQUE_INSTANCING,
    OPT_TECHNIQUE_CULLING,
    OPT_TECHNIQUE_LOD,
    OPT_TECHNIQUE_TEXTURE_STREAMING,
    OPT_TECHNIQUE_SHADER_VARIANTS,
    OPT_TECHNIQUE_DYNAMIC_RESOLUTION,
    OPT_TECHNIQUE_VARIABLE_RATE_SHADING,
    OPT_TECHNIQUE_MESH_OPTIMIZATION,
    OPT_TECHNIQUE_CACHE_OPTIMIZATION
} optimizationTechnique_t;

typedef struct optimizationSettings_s {
    optimizationLevel_t    level;
    qboolean               autoBatching;
    qboolean               autoInstancing;
    qboolean               aggressiveCulling;
    qboolean               dynamicLOD;
    qboolean               textureStreaming;
    qboolean               shaderOptimization;
    qboolean               dynamicResolution;
    float                  resolutionScale;
    uint32_t               maxDrawCalls;
    uint32_t               maxTriangles;
    uint32_t               maxTextureSize;
} optimizationSettings_t;

typedef struct renderOptimizer_s {
    qboolean               initialized;
    optimizationSettings_t settings;
    float                  currentFPS;
    float                  targetFPS;
    uint32_t               framesSinceAdjust;
    qboolean               needsAdjustment;
} renderOptimizer_t;

extern renderOptimizer_t renderOptimizer;

// Initialization
qboolean R_InitRenderOptimization( void );
void R_ShutdownRenderOptimization( void );

// Optimization control
void R_SetOptimizationLevel( optimizationLevel_t level );
void R_EnableOptimizationTechnique( optimizationTechnique_t technique, qboolean enable );
void R_AutoOptimize( float currentFPS, float targetFPS );

// Batching optimization
void R_OptimizeDrawBatches( void );
uint32_t R_MergeSimilarDrawCalls( void );
void R_SortDrawCallsByState( void );

// Instancing optimization
void R_ConvertToInstanced( void );
uint32_t R_FindInstanceableMeshes( void );

// Culling optimization
void R_OptimizeCulling( void );
void R_UpdateCullingThresholds( void );

// LOD optimization
void R_OptimizeLODSelection( void );
void R_AdjustLODBias( float bias );

// Texture optimization
void R_OptimizeTextureUsage( void );
void R_CompressUnusedTextures( void );

// Shader optimization
void R_OptimizeShaderVariants( void );
void R_PrecompileCommonVariants( void );

// Dynamic resolution
void R_UpdateDynamicResolution( void );
float R_CalculateOptimalResolution( void );

// Cache optimization
void R_OptimizeCacheUsage( void );
void R_ReorderDataForCache( void );

// Analysis
void R_AnalyzeRenderPerformance( void );
void R_SuggestOptimizations( void );

#endif // __TR_OPTIMIZE_H