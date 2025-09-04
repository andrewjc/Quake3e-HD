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
// tr_postprocess.c - Post-Processing Framework implementation

#include "tr_postprocess.h"
#include "../vulkan/vk.h"

// Global post-processing state
postProcessState_t postProcessState;

// CVars
cvar_t *r_postProcess;
cvar_t *r_postProcessDebug;
cvar_t *r_dof;
cvar_t *r_motionBlur;
cvar_t *r_chromaticAberration;
cvar_t *r_vignette;
cvar_t *r_filmGrain;
cvar_t *r_ao;
cvar_t *r_aoType;

/*
================
R_InitPostProcess

Initialize post-processing framework
================
*/
qboolean R_InitPostProcess( void ) {
    Com_Memset( &postProcessState, 0, sizeof( postProcessState ) );
    
    // Register CVars
    r_postProcess = ri.Cvar_Get( "r_postProcess", "1", CVAR_ARCHIVE );
    r_postProcessDebug = ri.Cvar_Get( "r_postProcessDebug", "0", CVAR_CHEAT );
    r_dof = ri.Cvar_Get( "r_dof", "0", CVAR_ARCHIVE );
    r_motionBlur = ri.Cvar_Get( "r_motionBlur", "0", CVAR_ARCHIVE );
    r_chromaticAberration = ri.Cvar_Get( "r_chromaticAberration", "0", CVAR_ARCHIVE );
    r_vignette = ri.Cvar_Get( "r_vignette", "0", CVAR_ARCHIVE );
    r_filmGrain = ri.Cvar_Get( "r_filmGrain", "0", CVAR_ARCHIVE );
    r_ao = ri.Cvar_Get( "r_ao", "0", CVAR_ARCHIVE );
    r_aoType = ri.Cvar_Get( "r_aoType", "0", CVAR_ARCHIVE );
    
    // Initialize default configurations
    
    // Depth of Field
    postProcessState.dofConfig.enabled = r_dof->integer ? qtrue : qfalse;
    postProcessState.dofConfig.focusDistance = 100.0f;
    postProcessState.dofConfig.focusRange = 50.0f;
    postProcessState.dofConfig.nearBlur = 1.0f;
    postProcessState.dofConfig.farBlur = 1.0f;
    postProcessState.dofConfig.bokehSize = 5.0f;
    postProcessState.dofConfig.bokehSamples = 16;
    
    // Motion Blur
    postProcessState.motionBlurConfig.enabled = r_motionBlur->integer ? qtrue : qfalse;
    postProcessState.motionBlurConfig.velocityScale = 1.0f;
    postProcessState.motionBlurConfig.samples = 8;
    postProcessState.motionBlurConfig.maxBlur = 20.0f;
    
    // Ambient Occlusion
    postProcessState.aoConfig.enabled = r_ao->integer ? qtrue : qfalse;
    postProcessState.aoConfig.radius = 0.5f;
    postProcessState.aoConfig.intensity = 1.0f;
    postProcessState.aoConfig.bias = 0.01f;
    postProcessState.aoConfig.samples = 16;
    postProcessState.aoConfig.power = 2.0f;
    
    // Chromatic Aberration
    postProcessState.chromaticConfig.enabled = r_chromaticAberration->integer ? qtrue : qfalse;
    postProcessState.chromaticConfig.strength = 0.5f;
    VectorSet( postProcessState.chromaticConfig.shift, 1.0f, 0.0f, -1.0f );
    
    // Setup pass configurations
    postProcessState.chain.passes[POST_PASS_DEPTH_OF_FIELD].config = &postProcessState.dofConfig;
    postProcessState.chain.passes[POST_PASS_MOTION_BLUR].config = &postProcessState.motionBlurConfig;
    postProcessState.chain.passes[POST_PASS_AMBIENT_OCCLUSION].config = &postProcessState.aoConfig;
    postProcessState.chain.passes[POST_PASS_CHROMATIC_ABERRATION].config = &postProcessState.chromaticConfig;
    
    postProcessState.initialized = qtrue;
    postProcessState.enabled = r_postProcess->integer ? qtrue : qfalse;
    postProcessState.globalIntensity = 1.0f;
    
    // Build initial post-process chain
    R_BuildPostProcessChain();
    
    ri.Printf( PRINT_ALL, "Post-processing framework initialized\n" );
    return qtrue;
}

/*
================
R_ShutdownPostProcess

Shutdown post-processing framework
================
*/
void R_ShutdownPostProcess( void ) {
    if ( !postProcessState.initialized ) {
        return;
    }
    
    Com_Memset( &postProcessState, 0, sizeof( postProcessState ) );
}

/*
================
R_BuildPostProcessChain

Build the post-processing chain based on enabled passes
================
*/
void R_BuildPostProcessChain( void ) {
    postProcessChain_t *chain = &postProcessState.chain;
    uint32_t passIndex = 0;
    
    // Clear chain
    chain->numActivePasses = 0;
    
    // Add passes in optimal order
    
    // Pre-tone mapping passes
    if ( postProcessState.aoConfig.enabled ) {
        chain->passOrder[passIndex++] = POST_PASS_AMBIENT_OCCLUSION;
        chain->passes[POST_PASS_AMBIENT_OCCLUSION].enabled = qtrue;
    }
    
    if ( postProcessState.dofConfig.enabled ) {
        chain->passOrder[passIndex++] = POST_PASS_DEPTH_OF_FIELD;
        chain->passes[POST_PASS_DEPTH_OF_FIELD].enabled = qtrue;
    }
    
    if ( postProcessState.motionBlurConfig.enabled ) {
        chain->passOrder[passIndex++] = POST_PASS_MOTION_BLUR;
        chain->passes[POST_PASS_MOTION_BLUR].enabled = qtrue;
    }
    
    // Post-tone mapping passes
    if ( postProcessState.chromaticConfig.enabled ) {
        chain->passOrder[passIndex++] = POST_PASS_CHROMATIC_ABERRATION;
        chain->passes[POST_PASS_CHROMATIC_ABERRATION].enabled = qtrue;
    }
    
    if ( r_vignette->integer ) {
        chain->passOrder[passIndex++] = POST_PASS_VIGNETTE;
        chain->passes[POST_PASS_VIGNETTE].enabled = qtrue;
    }
    
    if ( r_filmGrain->integer ) {
        chain->passOrder[passIndex++] = POST_PASS_FILM_GRAIN;
        chain->passes[POST_PASS_FILM_GRAIN].enabled = qtrue;
    }
    
    chain->numActivePasses = passIndex;
}

/*
================
R_EnablePostPass

Enable or disable a post-processing pass
================
*/
void R_EnablePostPass( postPassType_t type, qboolean enable ) {
    if ( type < 0 || type >= POST_PASS_COUNT ) {
        return;
    }
    
    postProcessState.chain.passes[type].enabled = enable;
    
    // Rebuild chain
    R_BuildPostProcessChain();
}

/*
================
R_SetPostPassIntensity

Set post-processing pass intensity
================
*/
void R_SetPostPassIntensity( postPassType_t type, float intensity ) {
    if ( type < 0 || type >= POST_PASS_COUNT ) {
        return;
    }
    
    postProcessState.chain.passes[type].intensity = intensity;
}

/*
================
R_SetDOFConfig

Set depth of field configuration
================
*/
void R_SetDOFConfig( const dofConfig_t *config ) {
    postProcessState.dofConfig = *config;
    
    // Update pass state
    postProcessState.chain.passes[POST_PASS_DEPTH_OF_FIELD].enabled = config->enabled;
    
    // Rebuild chain if needed
    R_BuildPostProcessChain();
}

/*
================
R_SetMotionBlurConfig

Set motion blur configuration
================
*/
void R_SetMotionBlurConfig( const motionBlurConfig_t *config ) {
    postProcessState.motionBlurConfig = *config;
    
    // Update pass state
    postProcessState.chain.passes[POST_PASS_MOTION_BLUR].enabled = config->enabled;
    
    // Rebuild chain if needed
    R_BuildPostProcessChain();
}

/*
================
R_SetAOConfig

Set ambient occlusion configuration
================
*/
void R_SetAOConfig( const aoConfig_t *config ) {
    postProcessState.aoConfig = *config;
    
    // Update pass state
    postProcessState.chain.passes[POST_PASS_AMBIENT_OCCLUSION].enabled = config->enabled;
    
    // Rebuild chain if needed
    R_BuildPostProcessChain();
}

/*
================
R_AutoFocus

Automatic depth of field focus
================
*/
void R_AutoFocus( const vec3_t viewOrigin, const vec3_t viewDir ) {
    if ( !postProcessState.dofConfig.enabled ) {
        return;
    }
    
    // Trace ray to find focus point
    trace_t trace;
    vec3_t end;
    
    VectorMA( viewOrigin, 10000.0f, viewDir, end );
    
    // Would perform trace here
    // For now, use fixed distance
    postProcessState.dofConfig.focusDistance = 500.0f;
}

/*
================
R_DrawPostProcessDebug

Draw post-processing debug information
================
*/
void R_DrawPostProcessDebug( void ) {
    if ( !r_postProcessDebug->integer || !postProcessState.initialized ) {
        return;
    }
    
    ri.Printf( PRINT_ALL, "Post-Process Debug:\n" );
    ri.Printf( PRINT_ALL, "  Enabled: %s\n", postProcessState.enabled ? "Yes" : "No" );
    ri.Printf( PRINT_ALL, "  Active Passes: %d\n", postProcessState.chain.numActivePasses );
    ri.Printf( PRINT_ALL, "  Frame Time: %.2f ms\n", postProcessState.frameTime );
    
    // List active passes
    for ( uint32_t i = 0; i < postProcessState.chain.numActivePasses; i++ ) {
        postPassType_t type = postProcessState.chain.passOrder[i];
        const char *passNames[] = {
            "Depth of Field",
            "Motion Blur",
            "Chromatic Aberration",
            "Vignette",
            "Film Grain",
            "Lens Flare",
            "God Rays",
            "Ambient Occlusion",
            "Fog",
            "SMAA",
            "FXAA",
            "Sharpen"
        };
        
        ri.Printf( PRINT_ALL, "    %d: %s\n", i + 1, passNames[type] );
    }
}