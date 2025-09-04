/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

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

#ifndef TR_LIGHT_SCISSOR_H
#define TR_LIGHT_SCISSOR_H

// Forward declarations
typedef struct renderLight_s renderLight_t;
typedef struct interaction_s interaction_t;
// viewParms_t is defined in tr_local.h
typedef struct scissorStats_s scissorStats_t;

/*
================================================================================
Phase 7: Light Scissoring Optimization

This system implements screen-space scissor rectangles and depth bounds testing
to minimize fragment shader invocations, providing 30-50% performance improvement
for multi-light scenarios.
================================================================================
*/

// Scissor rectangle structure
typedef struct scissorRect_s {
    int     x, y;           // Top-left corner
    int     width, height;  // Dimensions
    float   depthMin;       // Minimum depth (for depth bounds)
    float   depthMax;       // Maximum depth
} scissorRect_t;

// Scissor statistics
typedef struct scissorStats_s {
    int     totalLights;            // Total lights processed
    int     totalInteractions;      // Total interactions
    int     scissoredInteractions;  // Interactions with valid scissors
    int     culledByScissor;        // Interactions culled by scissor
    int     depthBoundsCulled;      // Culled by depth bounds
    int     totalPixels;            // Total viewport pixels
    int     scissoredPixels;        // Pixels within scissor rects
    float   efficiency;             // Culling efficiency percentage
    double  calcTime;               // Time spent calculating scissors
} scissorStats_t;

// Scissor optimization levels
typedef enum {
    SCISSOR_OFF,            // No scissoring
    SCISSOR_LIGHT,          // Per-light scissoring only
    SCISSOR_INTERACTION,    // Per-interaction scissoring
    SCISSOR_FULL            // Full optimization with depth bounds
} scissorLevel_t;

// Function declarations

// Core scissor calculation
void R_CalcLightScissorRectangle(renderLight_t *light, viewParms_t *view, int *scissor);
void R_CalcInteractionScissor(interaction_t *inter, viewParms_t *view, scissorRect_t *scissor);
void R_GetBoundsCorners(const vec3_t mins, const vec3_t maxs, vec3_t corners[8]);
qboolean R_ProjectPoint(const vec3_t point, const viewParms_t *view, vec3_t projected);

// Depth bounds testing
void R_GetInteractionDepthBounds(interaction_t *inter, float *minDepth, float *maxDepth);
void R_SetDepthBoundsTest(interaction_t *inter);
void R_EnableDepthBoundsTest(float minDepth, float maxDepth);
void R_DisableDepthBoundsTest(void);
qboolean R_IsDepthBoundsTestAvailable(void);

// Scissor optimization
void R_OptimizeLightScissors(void);
void R_OptimizeInteractionScissors(renderLight_t *light);
qboolean R_ScissorCullInteraction(interaction_t *inter, const scissorRect_t *scissor);

// Backend integration
void RB_SetScissor(const scissorRect_t *scissor);
void RB_SetLightScissor(renderLight_t *light);
void RB_SetInteractionScissor(interaction_t *inter);
void RB_ResetScissor(void);

// Vulkan-specific scissor
void VK_SetScissorRect(const scissorRect_t *scissor);
void VK_SetDepthBounds(float minDepth, float maxDepth);

// Statistics and debugging
void R_ScissorStatistics(void);
void R_PrintScissorStats(const scissorStats_t *stats);
void R_DrawScissorRects(void);
void R_DrawDepthBounds(void);
void R_ClearScissorStats(void);
scissorStats_t *R_GetScissorStats(void);

// Utility functions
void R_IntersectScissorRects(const scissorRect_t *a, const scissorRect_t *b, scissorRect_t *result);
qboolean R_IsScissorEmpty(const scissorRect_t *scissor);
void R_ExpandScissorRect(scissorRect_t *scissor, int pixels);
void R_ClampScissorToViewport(scissorRect_t *scissor, const viewParms_t *view);

// Configuration
void R_SetScissorLevel(scissorLevel_t level);
scissorLevel_t R_GetScissorLevel(void);
void R_SetScissorExpansion(int pixels);
int R_GetScissorExpansion(void);

// CVars
extern cvar_t *r_lightScissoring;
extern cvar_t *r_scissorLevel;
extern cvar_t *r_depthBoundsTest;
extern cvar_t *r_scissorDebug;
extern cvar_t *r_scissorExpand;
extern cvar_t *r_scissorStats;

#endif // TR_LIGHT_SCISSOR_H