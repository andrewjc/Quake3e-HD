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

#include "../core/tr_local.h"
#include "tr_light_scissor.h"
#include "tr_light_dynamic.h"

/*
================================================================================
Phase 7: Enhanced Light Scissoring Implementation

This file provides the complete scissoring system with depth bounds testing,
debug visualization, and performance metrics.
================================================================================
*/

// Global scissor state
static struct {
    scissorLevel_t  level;
    scissorStats_t  stats;
    int             expansionPixels;
    qboolean        depthBoundsAvailable;
    qboolean        debugVisualization;
    double          frameTime;
} scissorState = {
    .level = SCISSOR_INTERACTION,
    .expansionPixels = 2,
    .depthBoundsAvailable = qfalse,
    .debugVisualization = qfalse
};

// CVars
cvar_t *r_lightScissoring;
cvar_t *r_scissorLevel;
cvar_t *r_depthBoundsTest;
cvar_t *r_scissorDebug;
cvar_t *r_scissorExpand;
cvar_t *r_scissorStats;

/*
===============
R_InitScissorSystem

Initialize the scissoring system
===============
*/
void R_InitScissorSystem(void) {
    // Register CVars
    r_lightScissoring = ri.Cvar_Get("r_lightScissoring", "1", CVAR_ARCHIVE);
    r_scissorLevel = ri.Cvar_Get("r_scissorLevel", "2", CVAR_ARCHIVE);
    r_depthBoundsTest = ri.Cvar_Get("r_depthBoundsTest", "1", CVAR_ARCHIVE);
    r_scissorDebug = ri.Cvar_Get("r_scissorDebug", "0", CVAR_CHEAT);
    r_scissorExpand = ri.Cvar_Get("r_scissorExpand", "2", CVAR_ARCHIVE);
    r_scissorStats = ri.Cvar_Get("r_scissorStats", "0", CVAR_CHEAT);
    
    // Check for depth bounds test extension
    scissorState.depthBoundsAvailable = R_IsDepthBoundsTestAvailable();
    
    // Set scissor level
    scissorState.level = (scissorLevel_t)Com_Clamp(SCISSOR_OFF, SCISSOR_FULL, 
                                                    r_scissorLevel->integer);
    
    ri.Printf(PRINT_ALL, "Scissor system initialized (level %d, depth bounds %s)\n",
              scissorState.level, 
              scissorState.depthBoundsAvailable ? "available" : "unavailable");
}

/*
===============
R_CalcInteractionScissor

Calculate scissor rectangle for a specific interaction
===============
*/
void R_CalcInteractionScissor(interaction_t *inter, viewParms_t *view, scissorRect_t *scissor) {
    vec3_t corners[8];
    vec3_t projected;
    float minX, minY, maxX, maxY;
    float minDepth, maxDepth;
    int i;
    qboolean anyVisible = qfalse;
    
    if (!inter || !scissor) {
        return;
    }
    
    // Initialize with invalid values
    minX = view->viewportWidth;
    minY = view->viewportHeight;
    maxX = 0;
    maxY = 0;
    minDepth = 1.0f;
    maxDepth = 0.0f;
    
    // Get interaction bounds corners
    R_GetBoundsCorners(inter->bounds[0], inter->bounds[1], corners);
    
    // Project each corner to screen space
    for (i = 0; i < 8; i++) {
        if (R_ProjectPoint(corners[i], view, projected)) {
            minX = min(minX, projected[0]);
            minY = min(minY, projected[1]);
            maxX = max(maxX, projected[0]);
            maxY = max(maxY, projected[1]);
            minDepth = min(minDepth, projected[2]);
            maxDepth = max(maxDepth, projected[2]);
            anyVisible = qtrue;
        }
    }
    
    if (!anyVisible) {
        // Check if camera is inside bounds
        if (view->or.origin[0] >= inter->bounds[0][0] && 
            view->or.origin[0] <= inter->bounds[1][0] &&
            view->or.origin[1] >= inter->bounds[0][1] && 
            view->or.origin[1] <= inter->bounds[1][1] &&
            view->or.origin[2] >= inter->bounds[0][2] && 
            view->or.origin[2] <= inter->bounds[1][2]) {
            // Use full viewport
            scissor->x = 0;
            scissor->y = 0;
            scissor->width = view->viewportWidth;
            scissor->height = view->viewportHeight;
            scissor->depthMin = 0.0f;
            scissor->depthMax = 1.0f;
            return;
        }
        
        // Nothing visible
        scissor->x = scissor->y = 0;
        scissor->width = scissor->height = 0;
        scissor->depthMin = scissor->depthMax = 0;
        return;
    }
    
    // Apply expansion for conservative rasterization
    int expand = scissorState.expansionPixels;
    
    // Convert to integer pixel coordinates with expansion
    scissor->x = (int)max(0, minX - expand);
    scissor->y = (int)max(0, minY - expand);
    scissor->width = (int)min(view->viewportWidth, maxX + expand) - scissor->x;
    scissor->height = (int)min(view->viewportHeight, maxY + expand) - scissor->y;
    
    // Store depth bounds with small epsilon
    scissor->depthMin = max(0.0f, minDepth - 0.001f);
    scissor->depthMax = min(1.0f, maxDepth + 0.001f);
    
    // Ensure valid rectangle
    if (scissor->width <= 0 || scissor->height <= 0) {
        scissor->x = scissor->y = 0;
        scissor->width = scissor->height = 0;
    }
}

/*
===============
R_IntersectScissorRects

Calculate intersection of two scissor rectangles
===============
*/
void R_IntersectScissorRects(const scissorRect_t *a, const scissorRect_t *b, 
                             scissorRect_t *result) {
    int x1, y1, x2, y2;
    
    if (!a || !b || !result) {
        return;
    }
    
    // Calculate intersection
    x1 = max(a->x, b->x);
    y1 = max(a->y, b->y);
    x2 = min(a->x + a->width, b->x + b->width);
    y2 = min(a->y + a->height, b->y + b->height);
    
    if (x2 > x1 && y2 > y1) {
        result->x = x1;
        result->y = y1;
        result->width = x2 - x1;
        result->height = y2 - y1;
        result->depthMin = max(a->depthMin, b->depthMin);
        result->depthMax = min(a->depthMax, b->depthMax);
    } else {
        // No intersection
        result->x = result->y = 0;
        result->width = result->height = 0;
        result->depthMin = result->depthMax = 0;
    }
}

/*
===============
R_OptimizeInteractionScissors

Optimize scissor rectangles for all interactions of a light
===============
*/
void R_OptimizeInteractionScissors(renderLight_t *light) {
    interaction_t *inter;
    scissorRect_t lightScissor, interScissor, finalScissor;
    int numOptimized = 0;
    
    if (!light || scissorState.level < SCISSOR_INTERACTION) {
        return;
    }
    
    // Convert light scissor to our format
    lightScissor.x = light->scissorRect[0];
    lightScissor.y = light->scissorRect[1];
    lightScissor.width = light->scissorRect[2];
    lightScissor.height = light->scissorRect[3];
    lightScissor.depthMin = 0.0f;
    lightScissor.depthMax = 1.0f;
    
    // Process each interaction
    inter = light->firstInteraction;
    while (inter) {
        if (!inter->culled) {
            // Calculate interaction-specific scissor
            R_CalcInteractionScissor(inter, &tr.viewParms, &interScissor);
            
            // Intersect with light scissor
            R_IntersectScissorRects(&lightScissor, &interScissor, &finalScissor);
            
            // Store result
            inter->scissorRect[0] = finalScissor.x;
            inter->scissorRect[1] = finalScissor.y;
            inter->scissorRect[2] = finalScissor.width;
            inter->scissorRect[3] = finalScissor.height;
            inter->depthBounds[0] = finalScissor.depthMin;
            inter->depthBounds[1] = finalScissor.depthMax;
            
            // Check if scissor culled the interaction
            if (R_IsScissorEmpty(&finalScissor)) {
                inter->culled = qtrue;
                scissorState.stats.culledByScissor++;
            } else {
                numOptimized++;
            }
        }
        
        inter = inter->lightNext;
    }
    
    scissorState.stats.scissoredInteractions += numOptimized;
}

/*
===============
R_IsDepthBoundsTestAvailable

Check if hardware depth bounds test is available
===============
*/
qboolean R_IsDepthBoundsTestAvailable(void) {
#ifdef USE_OPENGL
    if (glConfig.driverType == GLDRV_OPENGL) {
        // Check for EXT_depth_bounds_test extension
        const char *extensions = (const char *)qglGetString(GL_EXTENSIONS);
        if (extensions && strstr(extensions, "GL_EXT_depth_bounds_test")) {
            return qtrue;
        }
    }
#endif
    
#ifdef USE_VULKAN
    // Vulkan depth bounds is a core feature if physical device supports it
    if (vk.active && vk.physicalDeviceFeatures.depthBounds) {
        return qtrue;
    }
#endif
    
    return qfalse;
}

/*
===============
R_EnableDepthBoundsTest

Enable hardware depth bounds test
===============
*/
void R_EnableDepthBoundsTest(float minDepth, float maxDepth) {
    if (!scissorState.depthBoundsAvailable || !r_depthBoundsTest->integer) {
        return;
    }
    
#ifdef USE_OPENGL
    if (glConfig.driverType == GLDRV_OPENGL) {
        qglEnable(GL_DEPTH_BOUNDS_TEST_EXT);
        qglDepthBoundsEXT(minDepth, maxDepth);
    }
#endif
    
#ifdef USE_VULKAN
    if (vk.active) {
        VK_SetDepthBounds(minDepth, maxDepth);
    }
#endif
}

/*
===============
R_DisableDepthBoundsTest

Disable hardware depth bounds test
===============
*/
void R_DisableDepthBoundsTest(void) {
    if (!scissorState.depthBoundsAvailable) {
        return;
    }
    
#ifdef USE_OPENGL
    if (glConfig.driverType == GLDRV_OPENGL) {
        qglDisable(GL_DEPTH_BOUNDS_TEST_EXT);
    }
#endif
    
#ifdef USE_VULKAN
    if (vk.active) {
        VK_SetDepthBounds(0.0f, 1.0f);
    }
#endif
}

/*
===============
RB_SetScissor

Backend function to set scissor rectangle
===============
*/
void RB_SetScissor(const scissorRect_t *scissor) {
    if (!scissor || !r_lightScissoring->integer) {
        return;
    }
    
    // Validate scissor
    if (scissor->width <= 0 || scissor->height <= 0) {
        return;
    }
    
#ifdef USE_OPENGL
    if (glConfig.driverType == GLDRV_OPENGL) {
        GL_SetScissorRect(scissor);
    }
#endif
    
#ifdef USE_VULKAN
    if (vk.active) {
        VK_SetScissorRect(scissor);
    }
#endif
    
    // Set depth bounds if available
    if (scissorState.level >= SCISSOR_FULL && scissorState.depthBoundsAvailable) {
        R_EnableDepthBoundsTest(scissor->depthMin, scissor->depthMax);
    }
}

/*
===============
RB_SetLightScissor

Set scissor for a light
===============
*/
void RB_SetLightScissor(renderLight_t *light) {
    scissorRect_t scissor;
    
    if (!light || scissorState.level < SCISSOR_LIGHT) {
        return;
    }
    
    scissor.x = light->scissorRect[0];
    scissor.y = light->scissorRect[1];
    scissor.width = light->scissorRect[2];
    scissor.height = light->scissorRect[3];
    scissor.depthMin = 0.0f;
    scissor.depthMax = 1.0f;
    
    RB_SetScissor(&scissor);
}

/*
===============
RB_SetInteractionScissor

Set scissor for an interaction
===============
*/
void RB_SetInteractionScissor(interaction_t *inter) {
    scissorRect_t scissor;
    
    if (!inter || scissorState.level < SCISSOR_INTERACTION) {
        return;
    }
    
    scissor.x = inter->scissorRect[0];
    scissor.y = inter->scissorRect[1];
    scissor.width = inter->scissorRect[2];
    scissor.height = inter->scissorRect[3];
    scissor.depthMin = inter->depthBounds[0];
    scissor.depthMax = inter->depthBounds[1];
    
    RB_SetScissor(&scissor);
}

/*
===============
RB_ResetScissor

Reset scissor to full viewport
===============
*/
void RB_ResetScissor(void) {
#ifdef USE_OPENGL
    if (glConfig.driverType == GLDRV_OPENGL) {
        qglDisable(GL_SCISSOR_TEST);
    }
#endif
    
#ifdef USE_VULKAN
    if (vk.active) {
        // Reset to full viewport scissor
        scissorRect_t fullScissor = {
            .x = 0,
            .y = 0,
            .width = tr.viewParms.viewportWidth,
            .height = tr.viewParms.viewportHeight,
            .depthMin = 0.0f,
            .depthMax = 1.0f
        };
        VK_SetScissorRect(&fullScissor);
    }
#endif
    
    R_DisableDepthBoundsTest();
}

/*
===============
R_ClearScissorStats

Clear scissor statistics
===============
*/
void R_ClearScissorStats(void) {
    Com_Memset(&scissorState.stats, 0, sizeof(scissorState.stats));
    scissorState.frameTime = ri.Milliseconds();
}

/*
===============
R_GetScissorStats

Get current scissor statistics
===============
*/
scissorStats_t *R_GetScissorStats(void) {
    // Calculate efficiency
    if (scissorState.stats.totalPixels > 0) {
        scissorState.stats.efficiency = 100.0f * (1.0f - 
            (float)scissorState.stats.scissoredPixels / 
            (float)scissorState.stats.totalPixels);
    }
    
    scissorState.stats.calcTime = ri.Milliseconds() - scissorState.frameTime;
    
    return &scissorState.stats;
}

/*
===============
R_PrintScissorStats

Print scissor statistics
===============
*/
void R_PrintScissorStats(const scissorStats_t *stats) {
    if (!stats || !r_scissorStats->integer) {
        return;
    }
    
    ri.Printf(PRINT_ALL, "=== Scissor Statistics ===\n");
    ri.Printf(PRINT_ALL, "Lights: %d, Interactions: %d (scissored: %d)\n",
              stats->totalLights, stats->totalInteractions, 
              stats->scissoredInteractions);
    ri.Printf(PRINT_ALL, "Culled: %d by scissor, %d by depth bounds\n",
              stats->culledByScissor, stats->depthBoundsCulled);
    ri.Printf(PRINT_ALL, "Pixels: %d total, %d scissored (%.1f%% saved)\n",
              stats->totalPixels, stats->scissoredPixels, stats->efficiency);
    ri.Printf(PRINT_ALL, "Time: %.2fms\n", stats->calcTime);
}

/*
===============
R_DrawScissorRects

Debug visualization of scissor rectangles
===============
*/
void R_DrawScissorRects(void) {
    extern lightSystem_t tr_lightSystem;
    int i;
    vec4_t color;
    
    if (!r_scissorDebug->integer) {
        return;
    }
    
    // Draw light scissors in yellow
    VectorSet4(color, 1.0f, 1.0f, 0.0f, 0.3f);
    
    for (i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        renderLight_t *light = tr_lightSystem.visibleLights[i];
        if (light && light->scissorRect[2] > 0 && light->scissorRect[3] > 0) {
            RE_SetColor(color);
            RE_StretchPic(light->scissorRect[0], light->scissorRect[1],
                         light->scissorRect[2], light->scissorRect[3],
                         0, 0, 1, 1, tr.whiteImage);
        }
    }
    
    // Draw interaction scissors in green
    if (scissorState.level >= SCISSOR_INTERACTION) {
        VectorSet4(color, 0.0f, 1.0f, 0.0f, 0.2f);
        
        for (i = 0; i < tr_lightSystem.numVisibleLights; i++) {
            renderLight_t *light = tr_lightSystem.visibleLights[i];
            if (!light) continue;
            
            interaction_t *inter = light->firstInteraction;
            while (inter) {
                if (!inter->culled && inter->scissorRect[2] > 0 && 
                    inter->scissorRect[3] > 0) {
                    RE_SetColor(color);
                    RE_StretchPic(inter->scissorRect[0], inter->scissorRect[1],
                                 inter->scissorRect[2], inter->scissorRect[3],
                                 0, 0, 1, 1, tr.whiteImage);
                }
                inter = inter->lightNext;
            }
        }
    }
    
    RE_SetColor(NULL);
}

/*
===============
R_SetScissorLevel

Set the scissor optimization level
===============
*/
void R_SetScissorLevel(scissorLevel_t level) {
    scissorState.level = Com_Clamp(SCISSOR_OFF, SCISSOR_FULL, level);
    r_scissorLevel->integer = scissorState.level;
}

/*
===============
R_GetScissorLevel

Get current scissor optimization level
===============
*/
scissorLevel_t R_GetScissorLevel(void) {
    return scissorState.level;
}

/*
===============
R_SetScissorExpansion

Set scissor expansion in pixels
===============
*/
void R_SetScissorExpansion(int pixels) {
    scissorState.expansionPixels = Com_Clamp(0, 16, pixels);
    r_scissorExpand->integer = scissorState.expansionPixels;
}

/*
===============
R_GetScissorExpansion

Get current scissor expansion
===============
*/
int R_GetScissorExpansion(void) {
    return scissorState.expansionPixels;
}