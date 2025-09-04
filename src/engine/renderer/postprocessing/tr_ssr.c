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
// tr_ssr.c - Screen-Space Reflections implementation

#include "tr_ssr.h"
#include "../vulkan/vk.h"
#include "../core/tr_common_utils.h"

// Global SSR state
ssrState_t ssrState;

// CVars
cvar_t *r_ssr;
cvar_t *r_ssrQuality;
cvar_t *r_ssrMaxDistance;
cvar_t *r_ssrDebug;

// Quality presets
static const ssrConfig_t ssrQualityPresets[] = {
    // SSR_QUALITY_OFF
    { .quality = SSR_QUALITY_OFF },
    
    // SSR_QUALITY_LOW
    {
        .quality = SSR_QUALITY_LOW,
        .traceMode = SSR_TRACE_LINEAR,
        .maxSteps = 16,
        .binarySearchSteps = 4,
        .maxDistance = 100.0f,
        .thickness = 0.5f,
        .strideZCutoff = 0.1f,
        .roughnessCutoff = 0.8f,
        .fadeDist = 50.0f,
        .fadeAngle = 0.5f,
        .reflectionScale = 0.5f,
        .useTemporal = qfalse,
        .useDenoising = qfalse,
    },
    
    // SSR_QUALITY_MEDIUM
    {
        .quality = SSR_QUALITY_MEDIUM,
        .traceMode = SSR_TRACE_LINEAR,
        .maxSteps = 32,
        .binarySearchSteps = 8,
        .maxDistance = 200.0f,
        .thickness = 0.2f,
        .strideZCutoff = 0.05f,
        .roughnessCutoff = 0.6f,
        .fadeDist = 100.0f,
        .fadeAngle = 0.3f,
        .reflectionScale = 0.7f,
        .useTemporal = qtrue,
        .temporalWeight = 0.9f,
        .useDenoising = qtrue,
        .denoiseSigma = 1.0f,
        .denoiseRadius = 2,
    },
    
    // SSR_QUALITY_HIGH
    {
        .quality = SSR_QUALITY_HIGH,
        .traceMode = SSR_TRACE_HIERARCHICAL,
        .maxSteps = 48,
        .binarySearchSteps = 12,
        .maxDistance = 500.0f,
        .thickness = 0.1f,
        .strideZCutoff = 0.02f,
        .roughnessCutoff = 0.4f,
        .fadeDist = 200.0f,
        .fadeAngle = 0.2f,
        .reflectionScale = 0.9f,
        .useTemporal = qtrue,
        .temporalWeight = 0.95f,
        .temporalClamp = 1.0f,
        .useDenoising = qtrue,
        .denoiseSigma = 0.5f,
        .denoiseRadius = 3,
    },
    
    // SSR_QUALITY_ULTRA
    {
        .quality = SSR_QUALITY_ULTRA,
        .traceMode = SSR_TRACE_HIERARCHICAL,
        .maxSteps = 64,
        .binarySearchSteps = 16,
        .maxDistance = 1000.0f,
        .thickness = 0.05f,
        .strideZCutoff = 0.01f,
        .roughnessCutoff = 0.3f,
        .fadeDist = 500.0f,
        .fadeAngle = 0.1f,
        .reflectionScale = 1.0f,
        .useTemporal = qtrue,
        .temporalWeight = 0.98f,
        .temporalClamp = 0.5f,
        .useDenoising = qtrue,
        .denoiseSigma = 0.25f,
        .denoiseRadius = 4,
    },
};

/*
================
R_InitSSR

Initialize Screen-Space Reflections
================
*/
qboolean R_InitSSR( void ) {
    Com_Memset( &ssrState, 0, sizeof( ssrState ) );
    
    // Register CVars
    r_ssr = ri.Cvar_Get( "r_ssr", "0", CVAR_ARCHIVE );
    r_ssrQuality = ri.Cvar_Get( "r_ssrQuality", "2", CVAR_ARCHIVE );
    r_ssrMaxDistance = ri.Cvar_Get( "r_ssrMaxDistance", "500", CVAR_ARCHIVE );
    r_ssrDebug = ri.Cvar_Get( "r_ssrDebug", "0", CVAR_CHEAT );
    
    // Set default quality
    R_SetSSRQuality( SSR_QUALITY_MEDIUM );
    
    ssrState.initialized = qtrue;
    ssrState.enabled = r_ssr->integer ? qtrue : qfalse;
    
    ri.Printf( PRINT_ALL, "SSR initialized\n" );
    return qtrue;
}

/*
================
R_ShutdownSSR

Shutdown Screen-Space Reflections
================
*/
void R_ShutdownSSR( void ) {
    if ( !ssrState.initialized ) {
        return;
    }
    
    Com_Memset( &ssrState, 0, sizeof( ssrState ) );
}

/*
================
R_SetSSRQuality

Set SSR quality preset
================
*/
void R_SetSSRQuality( ssrQuality_t quality ) {
    if ( quality < 0 || quality > SSR_QUALITY_ULTRA ) {
        quality = SSR_QUALITY_MEDIUM;
    }
    
    ssrState.config = ssrQualityPresets[quality];
    
    // Apply custom max distance if set
    if ( r_ssrMaxDistance ) {
        ssrState.config.maxDistance = r_ssrMaxDistance->value;
    }
}

/*
================
R_SetSSRConfig

Set custom SSR configuration
================
*/
void R_SetSSRConfig( const ssrConfig_t *config ) {
    ssrState.config = *config;
}

/*
================
R_GetSSRConfig

Get current SSR configuration
================
*/
void R_GetSSRConfig( ssrConfig_t *config ) {
    *config = ssrState.config;
}

/*
================
R_BeginSSRFrame

Begin SSR frame processing
================
*/
void R_BeginSSRFrame( const viewParms_t *viewParms ) {
    if ( !ssrState.initialized || !ssrState.enabled ) {
        return;
    }
    
    // Store matrices for ray tracing
    MatrixCopy( viewParms->world.modelMatrix, ssrState.viewMatrix );
    MatrixCopy( viewParms->projectionMatrix, ssrState.projMatrix );
    MatrixInverse( ssrState.projMatrix, ssrState.invProjMatrix );
    
    // Clear statistics
    ssrState.raysTraced = 0;
    ssrState.raysHit = 0;
    ssrState.averageSteps = 0;
}

/*
================
R_EndSSRFrame

End SSR frame processing
================
*/
void R_EndSSRFrame( void ) {
    if ( !ssrState.initialized || !ssrState.enabled ) {
        return;
    }
    
    // Store current view-proj as previous for temporal
    MatrixMultiply( ssrState.projMatrix, ssrState.viewMatrix, ssrState.prevViewProjMatrix );
    
    // Swap history buffers
    ssrState.resources.currentHistory = 1 - ssrState.resources.currentHistory;
}

/*
================
R_DrawSSRDebug

Draw SSR debug information
================
*/
void R_DrawSSRDebug( void ) {
    if ( !r_ssrDebug->integer || !ssrState.initialized ) {
        return;
    }
    
    float hitRate = ssrState.raysTraced > 0 ? 
                   (float)ssrState.raysHit / ssrState.raysTraced * 100.0f : 0.0f;
    
    ri.Printf( PRINT_ALL, "SSR Debug:\n" );
    ri.Printf( PRINT_ALL, "  Enabled: %s\n", ssrState.enabled ? "Yes" : "No" );
    ri.Printf( PRINT_ALL, "  Quality: %d\n", ssrState.config.quality );
    ri.Printf( PRINT_ALL, "  Rays Traced: %d\n", ssrState.raysTraced );
    ri.Printf( PRINT_ALL, "  Hit Rate: %.1f%%\n", hitRate );
    ri.Printf( PRINT_ALL, "  Avg Steps: %.1f\n", ssrState.averageSteps );
}