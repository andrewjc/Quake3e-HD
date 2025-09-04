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
// tr_volumetric.c - Volumetric Rendering implementation

#include "tr_volumetric.h"
#include "../vulkan/vk.h"

// Global volumetric state
volumetricState_t volumetricState;

// CVars
cvar_t *r_volumetric;
cvar_t *r_volumetricQuality;
cvar_t *r_volumetricDensity;
cvar_t *r_volumetricDebug;

// Quality presets
static const volumetricConfig_t volumetricQualityPresets[] = {
    // VOLUMETRIC_QUALITY_OFF
    { .quality = VOLUMETRIC_QUALITY_OFF },
    
    // VOLUMETRIC_QUALITY_LOW
    {
        .quality = VOLUMETRIC_QUALITY_LOW,
        .gridSizeX = 80,
        .gridSizeY = 45,
        .gridSizeZ = 64,
        .depthSlicing = 1.05f,
        .scatteringCoeff = 0.01f,
        .absorptionCoeff = 0.001f,
        .anisotropy = 0.8f,
        .density = 0.05f,
        .marchSteps = 32,
        .marchStepSize = 1.0f,
        .useTemporal = qfalse,
        .enableShadows = qfalse,
    },
    
    // VOLUMETRIC_QUALITY_MEDIUM
    {
        .quality = VOLUMETRIC_QUALITY_MEDIUM,
        .gridSizeX = 160,
        .gridSizeY = 90,
        .gridSizeZ = 128,
        .depthSlicing = 1.03f,
        .scatteringCoeff = 0.02f,
        .absorptionCoeff = 0.002f,
        .anisotropy = 0.7f,
        .density = 0.1f,
        .marchSteps = 64,
        .marchStepSize = 0.5f,
        .useTemporal = qtrue,
        .temporalBlend = 0.9f,
        .enableShadows = qtrue,
        .shadowSamples = 8,
        .shadowDensity = 0.5f,
    },
    
    // VOLUMETRIC_QUALITY_HIGH
    {
        .quality = VOLUMETRIC_QUALITY_HIGH,
        .gridSizeX = 240,
        .gridSizeY = 135,
        .gridSizeZ = 192,
        .depthSlicing = 1.02f,
        .scatteringCoeff = 0.03f,
        .absorptionCoeff = 0.003f,
        .anisotropy = 0.6f,
        .density = 0.15f,
        .marchSteps = 96,
        .marchStepSize = 0.33f,
        .useTemporal = qtrue,
        .temporalBlend = 0.95f,
        .enableShadows = qtrue,
        .shadowSamples = 16,
        .shadowDensity = 0.75f,
    },
    
    // VOLUMETRIC_QUALITY_ULTRA
    {
        .quality = VOLUMETRIC_QUALITY_ULTRA,
        .gridSizeX = 320,
        .gridSizeY = 180,
        .gridSizeZ = 256,
        .depthSlicing = 1.01f,
        .scatteringCoeff = 0.04f,
        .absorptionCoeff = 0.004f,
        .anisotropy = 0.5f,
        .density = 0.2f,
        .marchSteps = 128,
        .marchStepSize = 0.25f,
        .useTemporal = qtrue,
        .temporalBlend = 0.98f,
        .enableShadows = qtrue,
        .shadowSamples = 32,
        .shadowDensity = 1.0f,
    },
};

/*
================
R_InitVolumetric

Initialize Volumetric Rendering
================
*/
qboolean R_InitVolumetric( void ) {
    Com_Memset( &volumetricState, 0, sizeof( volumetricState ) );
    
    // Register CVars
    r_volumetric = ri.Cvar_Get( "r_volumetric", "0", CVAR_ARCHIVE );
    r_volumetricQuality = ri.Cvar_Get( "r_volumetricQuality", "2", CVAR_ARCHIVE );
    r_volumetricDensity = ri.Cvar_Get( "r_volumetricDensity", "1.0", CVAR_ARCHIVE );
    r_volumetricDebug = ri.Cvar_Get( "r_volumetricDebug", "0", CVAR_CHEAT );
    
    // Set default quality
    R_SetVolumetricQuality( VOLUMETRIC_QUALITY_MEDIUM );
    
    volumetricState.initialized = qtrue;
    volumetricState.enabled = r_volumetric->integer ? qtrue : qfalse;
    
    ri.Printf( PRINT_ALL, "Volumetric rendering initialized\n" );
    return qtrue;
}

/*
================
R_ShutdownVolumetric

Shutdown Volumetric Rendering
================
*/
void R_ShutdownVolumetric( void ) {
    if ( !volumetricState.initialized ) {
        return;
    }
    
    Com_Memset( &volumetricState, 0, sizeof( volumetricState ) );
}

/*
================
R_SetVolumetricQuality

Set volumetric quality preset
================
*/
void R_SetVolumetricQuality( volumetricQuality_t quality ) {
    if ( quality < 0 || quality > VOLUMETRIC_QUALITY_ULTRA ) {
        quality = VOLUMETRIC_QUALITY_MEDIUM;
    }
    
    volumetricState.config = volumetricQualityPresets[quality];
    
    // Apply density scale
    if ( r_volumetricDensity ) {
        volumetricState.config.density *= r_volumetricDensity->value;
    }
}

/*
================
R_SetVolumetricConfig

Set custom volumetric configuration
================
*/
void R_SetVolumetricConfig( const volumetricConfig_t *config ) {
    volumetricState.config = *config;
}

/*
================
R_AddVolumetricLight

Add a volumetric light source
================
*/
void R_AddVolumetricLight( const volumetricLight_t *light ) {
    if ( volumetricState.numLights >= VOLUMETRIC_MAX_LIGHTS ) {
        return;
    }
    
    volumetricState.lights[volumetricState.numLights++] = *light;
}

/*
================
R_ClearVolumetricLights

Clear all volumetric lights
================
*/
void R_ClearVolumetricLights( void ) {
    volumetricState.numLights = 0;
}

/*
================
R_DrawVolumetricDebug

Draw volumetric debug information
================
*/
void R_DrawVolumetricDebug( void ) {
    if ( !r_volumetricDebug->integer || !volumetricState.initialized ) {
        return;
    }
    
    ri.Printf( PRINT_ALL, "Volumetric Debug:\n" );
    ri.Printf( PRINT_ALL, "  Enabled: %s\n", volumetricState.enabled ? "Yes" : "No" );
    ri.Printf( PRINT_ALL, "  Quality: %d\n", volumetricState.config.quality );
    ri.Printf( PRINT_ALL, "  Grid: %dx%dx%d\n", 
              volumetricState.config.gridSizeX,
              volumetricState.config.gridSizeY,
              volumetricState.config.gridSizeZ );
    ri.Printf( PRINT_ALL, "  Lights: %d\n", volumetricState.numLights );
    ri.Printf( PRINT_ALL, "  Avg Density: %.3f\n", volumetricState.averageDensity );
}