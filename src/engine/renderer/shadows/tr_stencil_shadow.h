/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#ifndef TR_STENCIL_SHADOW_H
#define TR_STENCIL_SHADOW_H

#include "../core/tr_local.h"
#include "../lighting/tr_light_dynamic.h"
#include "tr_shadow_volume.h"

/*
================================================================================
Phase 9: Stencil Shadow Rendering

This file defines the interface for GPU-based stencil shadow rendering
using the Z-fail algorithm.
================================================================================
*/

// Cull face modes
enum {
    CULLFACE_NONE,
    CULLFACE_FRONT,
    CULLFACE_BACK
};

// Stencil operations
enum {
    STENCIL_KEEP,
    STENCIL_ZERO,
    STENCIL_REPLACE,
    STENCIL_INCR,
    STENCIL_INCR_WRAP,
    STENCIL_DECR,
    STENCIL_DECR_WRAP,
    STENCIL_INVERT
};

// Function declarations

// System management
void RB_InitStencilShadows(void);
void RB_ShutdownStencilShadows(void);

// Shadow pass control
void RB_ShadowBegin(void);
void RB_ShadowEnd(void);
// RB_ShadowFinish is already defined in tr_shadows.c

// Depth prepass
void RB_DepthPrepass(void);

// Stencil buffer operations
void RB_ClearStencilBuffer(void);
void RB_SetStencilOp(int fail, int zfail, int zpass);

// Face culling
void RB_SetCullFace(int cullFace);

// Shadow volume rendering
void RB_DrawShadowVolume(shadowVolume_t *volume);
void RB_StencilShadowPass(renderLight_t *light);

// Lit surface rendering
void RB_RenderLitSurfaces(renderLight_t *light);
void RB_RenderShadowedLight(renderLight_t *light);

// Testing and optimization
qboolean RB_TestStencilShadow(void);
void RB_OptimizeStencilShadows(void);

// Statistics
void RB_StencilShadowStatistics(void);

#endif // TR_STENCIL_SHADOW_H