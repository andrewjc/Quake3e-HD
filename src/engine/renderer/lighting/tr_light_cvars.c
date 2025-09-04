/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

This file is part of Quake3e-HD.

Quake3e-HD is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
===========================================================================
*/
// tr_light_cvars.c - Lighting system console variables

#include "../core/tr_local.h"

// Dynamic lighting
cvar_t *r_dynamicLighting;

// Light culling
cvar_t *r_lightCullDistance;

// Light grid
cvar_t *r_lightGridSize;

// Debug
cvar_t *r_showInteractions;

/*
================
R_InitLightingCVars

Initialize lighting console variables
================
*/
void R_InitLightingCVars( void ) {
    r_dynamicLighting = ri.Cvar_Get( "r_dynamicLighting", "1", CVAR_ARCHIVE );
    r_lightCullDistance = ri.Cvar_Get( "r_lightCullDistance", "8192", CVAR_ARCHIVE );
    r_lightGridSize = ri.Cvar_Get( "r_lightGridSize", "64", CVAR_ARCHIVE );
    r_showInteractions = ri.Cvar_Get( "r_showInteractions", "0", CVAR_CHEAT );
}