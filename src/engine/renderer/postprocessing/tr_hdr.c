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
// tr_hdr.c - HDR Rendering Pipeline implementation

#include "tr_hdr.h"
#include "../vulkan/vk.h"
#include "../core/tr_common_utils.h"
#include <float.h>

// Global HDR state
hdrState_t hdrState;

// CVars
cvar_t *r_hdr;
cvar_t *r_hdrExposure;
cvar_t *r_hdrAutoExposure;
cvar_t *r_hdrBloom;
cvar_t *r_hdrTonemapper;
cvar_t *r_hdrDebug;

/*
================
R_InitHDR

Initialize HDR rendering
================
*/
qboolean R_InitHDR( void ) {
    Com_Memset( &hdrState, 0, sizeof( hdrState ) );
    
    // Register CVars
    r_hdr = ri.Cvar_Get( "r_hdr", "1", CVAR_ARCHIVE );
    r_hdrExposure = ri.Cvar_Get( "r_hdrExposure", "1.0", CVAR_ARCHIVE );
    r_hdrAutoExposure = ri.Cvar_Get( "r_hdrAutoExposure", "1", CVAR_ARCHIVE );
    r_hdrBloom = ri.Cvar_Get( "r_hdrBloom", "1", CVAR_ARCHIVE );
    r_hdrTonemapper = ri.Cvar_Get( "r_hdrTonemapper", "2", CVAR_ARCHIVE );
    r_hdrDebug = ri.Cvar_Get( "r_hdrDebug", "0", CVAR_CHEAT );
    
    // Set default configuration
    hdrState.config.enabled = r_hdr->integer ? qtrue : qfalse;
    hdrState.config.tonemapper = TONEMAP_ACES;
    hdrState.config.exposureValue = 1.0f;
    hdrState.config.exposureCompensation = 0.0f;
    hdrState.config.autoExposure = r_hdrAutoExposure->integer ? qtrue : qfalse;
    hdrState.config.adaptationRate = 2.0f;
    hdrState.config.minLuminance = 0.001f;
    hdrState.config.maxLuminance = 10000.0f;
    hdrState.config.keyValue = 0.18f;
    
    // Bloom configuration
    hdrState.config.bloomEnabled = r_hdrBloom->integer ? qtrue : qfalse;
    hdrState.config.bloomThreshold = 1.0f;
    hdrState.config.bloomIntensity = 0.5f;
    hdrState.config.bloomRadius = 1.0f;
    hdrState.config.bloomLevels = HDR_BLOOM_LEVELS;
    
    // Color grading defaults
    hdrState.config.saturation = 1.0f;
    hdrState.config.contrast = 1.0f;
    VectorSet( hdrState.config.colorBalance, 1.0f, 1.0f, 1.0f );
    VectorSet( hdrState.config.lift, 0.0f, 0.0f, 0.0f );
    VectorSet( hdrState.config.gamma, 1.0f, 1.0f, 1.0f );
    VectorSet( hdrState.config.gain, 1.0f, 1.0f, 1.0f );
    
    // Initialize current exposure
    hdrState.currentExposure = 1.0f;
    hdrState.targetExposure = 1.0f;
    hdrState.avgLuminance = 0.18f;
    
    hdrState.initialized = qtrue;
    
    ri.Printf( PRINT_ALL, "HDR rendering initialized\n" );
    return qtrue;
}

/*
================
R_ShutdownHDR

Shutdown HDR rendering
================
*/
void R_ShutdownHDR( void ) {
    if ( !hdrState.initialized ) {
        return;
    }
    
    Com_Memset( &hdrState, 0, sizeof( hdrState ) );
}

/*
================
R_SetHDRConfig

Set HDR configuration
================
*/
void R_SetHDRConfig( const hdrConfig_t *config ) {
    hdrState.config = *config;
}

/*
================
R_GetHDRConfig

Get current HDR configuration
================
*/
void R_GetHDRConfig( hdrConfig_t *config ) {
    *config = hdrState.config;
}

/*
================
R_SetTonemapper

Set tone mapping operator
================
*/
void R_SetTonemapper( tonemapOperator_t tonemapper ) {
    if ( tonemapper < 0 || tonemapper > TONEMAP_FILMIC ) {
        tonemapper = TONEMAP_ACES;
    }
    
    hdrState.config.tonemapper = tonemapper;
}

/*
================
R_BeginHDRFrame

Begin HDR frame processing
================
*/
void R_BeginHDRFrame( void ) {
    if ( !hdrState.initialized || !hdrState.config.enabled ) {
        return;
    }
    
    // Clear frame statistics
    hdrState.minLuminanceFrame = FLT_MAX;
    hdrState.maxLuminanceFrame = -FLT_MAX;
    hdrState.overexposedPixels = 0;
    hdrState.underexposedPixels = 0;
}

/*
================
R_EndHDRFrame

End HDR frame processing
================
*/
void R_EndHDRFrame( void ) {
    if ( !hdrState.initialized || !hdrState.config.enabled ) {
        return;
    }
    
    // Update exposure for next frame
    if ( hdrState.config.autoExposure ) {
        R_UpdateExposure( hdrState.deltaTime );
    }
}

/*
================
R_UpdateExposure

Update auto-exposure based on scene luminance
================
*/
void R_UpdateExposure( float deltaTime ) {
    if ( !hdrState.config.autoExposure ) {
        hdrState.currentExposure = hdrState.config.exposureValue;
        return;
    }
    
    // Calculate target exposure from average luminance
    float targetEV = hdrState.config.keyValue / hdrState.avgLuminance;
    targetEV = CLAMP( targetEV, 
                     hdrState.config.minLuminance, 
                     hdrState.config.maxLuminance );
    
    // Apply exposure compensation
    targetEV *= powf( 2.0f, hdrState.config.exposureCompensation );
    
    // Smooth adaptation
    float adaptRate = hdrState.config.adaptationRate * deltaTime;
    adaptRate = CLAMP( adaptRate, 0.0f, 1.0f );
    
    hdrState.currentExposure = hdrState.currentExposure + 
                              ( targetEV - hdrState.currentExposure ) * adaptRate;
    
    hdrState.deltaTime = deltaTime;
}

/*
================
R_SetManualExposure

Set manual exposure value
================
*/
void R_SetManualExposure( float ev ) {
    hdrState.config.autoExposure = qfalse;
    hdrState.config.exposureValue = ev;
    hdrState.currentExposure = ev;
}

/*
================
R_DrawHDRDebug

Draw HDR debug information
================
*/
void R_DrawHDRDebug( void ) {
    if ( !r_hdrDebug->integer || !hdrState.initialized ) {
        return;
    }
    
    const char *tonemapNames[] = {
        "Reinhard",
        "Reinhard Extended",
        "ACES",
        "Uncharted 2",
        "Exposure",
        "Filmic"
    };
    
    ri.Printf( PRINT_ALL, "HDR Debug:\n" );
    ri.Printf( PRINT_ALL, "  Enabled: %s\n", hdrState.config.enabled ? "Yes" : "No" );
    ri.Printf( PRINT_ALL, "  Tonemapper: %s\n", tonemapNames[hdrState.config.tonemapper] );
    ri.Printf( PRINT_ALL, "  Current Exposure: %.3f\n", hdrState.currentExposure );
    ri.Printf( PRINT_ALL, "  Avg Luminance: %.3f\n", hdrState.avgLuminance );
    ri.Printf( PRINT_ALL, "  Min/Max Lum: %.3f / %.3f\n", 
              hdrState.minLuminanceFrame, hdrState.maxLuminanceFrame );
    ri.Printf( PRINT_ALL, "  Auto-Exposure: %s\n", 
              hdrState.config.autoExposure ? "On" : "Off" );
    ri.Printf( PRINT_ALL, "  Bloom: %s\n", 
              hdrState.config.bloomEnabled ? "On" : "Off" );
}