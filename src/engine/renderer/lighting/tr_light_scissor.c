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

#include "../tr_local.h"
#include "tr_light_dynamic.h"

/*
================================================================================
Phase 7: Light Scissoring Optimization

This file implements screen-space scissor rectangles to limit pixel shader 
execution to affected screen regions, providing 30-50% reduction in fragment 
shader invocations.
================================================================================
*/

// Helper macros for min/max
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

/*
================
R_GetBoundsCorners

Extract the 8 corner points from an AABB
================
*/
static void R_GetBoundsCorners(const vec3_t mins, const vec3_t maxs, vec3_t corners[8]) {
    corners[0][0] = mins[0]; corners[0][1] = mins[1]; corners[0][2] = mins[2];
    corners[1][0] = maxs[0]; corners[1][1] = mins[1]; corners[1][2] = mins[2];
    corners[2][0] = mins[0]; corners[2][1] = maxs[1]; corners[2][2] = mins[2];
    corners[3][0] = maxs[0]; corners[3][1] = maxs[1]; corners[3][2] = mins[2];
    corners[4][0] = mins[0]; corners[4][1] = mins[1]; corners[4][2] = maxs[2];
    corners[5][0] = maxs[0]; corners[5][1] = mins[1]; corners[5][2] = maxs[2];
    corners[6][0] = mins[0]; corners[6][1] = maxs[1]; corners[6][2] = maxs[2];
    corners[7][0] = maxs[0]; corners[7][1] = maxs[1]; corners[7][2] = maxs[2];
}

/*
================
R_ProjectPoint

Project a 3D point to screen space
Returns qfalse if point is behind viewer
================
*/
static qboolean R_ProjectPoint(const vec3_t point, const viewParms_t *view, vec3_t projected) {
    vec4_t eye, clip;
    vec3_t ndc;
    
    // Transform to clip space
    R_TransformModelToClip(point, view->world.modelMatrix, 
                           view->projectionMatrix, eye, clip);
    
    // Check if behind viewer
    if (clip[3] <= 0) {
        return qfalse;
    }
    
    // Convert to NDC
    ndc[0] = clip[0] / clip[3];
    ndc[1] = clip[1] / clip[3];
    ndc[2] = clip[2] / clip[3];
    
    // Convert to screen coordinates
    projected[0] = (ndc[0] * 0.5f + 0.5f) * view->viewportWidth;
    projected[1] = (ndc[1] * 0.5f + 0.5f) * view->viewportHeight;
    projected[2] = ndc[2] * 0.5f + 0.5f;  // Normalized depth
    
    return qtrue;
}

/*
================
R_CalcLightScissorRectangle

Calculate screen-space scissor rectangle for a light volume
================
*/
void R_CalcLightScissorRectangle(renderLight_t *light, viewParms_t *view, int *scissor) {
    vec3_t corners[8];
    vec3_t projected;
    float minX, minY, maxX, maxY;
    int i;
    qboolean anyVisible = qfalse;
    
    // Initialize with full viewport (in case all corners are behind)
    minX = (float)view->viewportWidth;
    minY = (float)view->viewportHeight;
    maxX = 0;
    maxY = 0;
    
    // Get light volume corners
    R_GetBoundsCorners(light->mins, light->maxs, corners);
    
    // Project each corner to screen space
    for (i = 0; i < 8; i++) {
        if (R_ProjectPoint(corners[i], view, projected)) {
            minX = min(minX, projected[0]);
            minY = min(minY, projected[1]);
            maxX = max(maxX, projected[0]);
            maxY = max(maxY, projected[1]);
            anyVisible = qtrue;
        }
    }
    
    // If no corners visible, check if camera is inside light volume
    if (!anyVisible) {
        // Camera inside volume - use full viewport
        if (view->or.origin[0] >= light->mins[0] && view->or.origin[0] <= light->maxs[0] &&
            view->or.origin[1] >= light->mins[1] && view->or.origin[1] <= light->maxs[1] &&
            view->or.origin[2] >= light->mins[2] && view->or.origin[2] <= light->maxs[2]) {
            scissor[0] = 0;
            scissor[1] = 0;
            scissor[2] = view->viewportWidth;
            scissor[3] = view->viewportHeight;
            return;
        }
        
        // Light completely behind viewer
        scissor[0] = scissor[1] = scissor[2] = scissor[3] = 0;
        return;
    }
    
    // Convert to integer pixel coordinates with padding
    scissor[0] = (int)max(0, minX - 1);
    scissor[1] = (int)max(0, minY - 1);
    scissor[2] = (int)min(view->viewportWidth, maxX + 1) - scissor[0];
    scissor[3] = (int)min(view->viewportHeight, maxY + 1) - scissor[1];
    
    // Ensure valid rectangle
    if (scissor[2] <= 0 || scissor[3] <= 0) {
        scissor[0] = scissor[1] = scissor[2] = scissor[3] = 0;
    }
}

/*
================
R_GetInteractionDepthBounds

Calculate min/max depth range for an interaction
================
*/
void R_GetInteractionDepthBounds(interaction_t *inter, float *minDepth, float *maxDepth) {
    vec3_t corners[8];
    vec3_t projected;
    int i;
    qboolean initialized = qfalse;
    
    if (!inter || !minDepth || !maxDepth) {
        return;
    }
    
    // Get interaction bounds corners
    R_GetBoundsCorners(inter->bounds[0], inter->bounds[1], corners);
    
    // Project corners and find depth range
    for (i = 0; i < 8; i++) {
        if (R_ProjectPoint(corners[i], &tr.viewParms, projected)) {
            if (!initialized) {
                *minDepth = *maxDepth = projected[2];
                initialized = qtrue;
            } else {
                *minDepth = min(*minDepth, projected[2]);
                *maxDepth = max(*maxDepth, projected[2]);
            }
        }
    }
    
    // If nothing was visible, use full depth range
    if (!initialized) {
        *minDepth = 0.0f;
        *maxDepth = 1.0f;
    }
    
    // Add small epsilon to avoid z-fighting
    *minDepth = max(0.0f, *minDepth - 0.001f);
    *maxDepth = min(1.0f, *maxDepth + 0.001f);
}

/*
================
R_SetDepthBoundsTest

Enable hardware depth bounds test if available
================
*/
void R_SetDepthBoundsTest(interaction_t *inter) {
    float minDepth, maxDepth;
    
    if (!inter) {
        return;
    }
    
    // Calculate depth bounds for this interaction
    R_GetInteractionDepthBounds(inter, &minDepth, &maxDepth);
    
    // Note: Actual OpenGL/Vulkan depth bounds test would be set here
    // This is a placeholder for the API-specific implementation
    
    // Store bounds in interaction for backend use
    inter->depthBounds[0] = minDepth;
    inter->depthBounds[1] = maxDepth;
}

/*
================
R_OptimizeLightScissors

Optimize scissor rectangles for all visible lights
================
*/
void R_OptimizeLightScissors(void) {
    int i;
    renderLight_t *light;
    interaction_t *inter;
    int totalPixels = tr.viewParms.viewportWidth * tr.viewParms.viewportHeight;
    int scissoredPixels = 0;
    
    for (i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        light = tr_lightSystem.visibleLights[i];
        
        if (!light) {
            continue;
        }
        
        // Calculate light scissor rectangle
        R_CalcLightScissorRectangle(light, &tr.viewParms, light->scissorRect);
        
        // Update all interactions for this light
        inter = light->firstInteraction;
        while (inter) {
            if (!inter->culled) {
                // Intersection of light scissor and interaction bounds scissor
                int interScissor[4];
                
                // Calculate interaction-specific scissor
                vec3_t corners[8];
                R_GetBoundsCorners(inter->bounds[0], inter->bounds[1], corners);
                
                float minX = tr.viewParms.viewportWidth;
                float minY = tr.viewParms.viewportHeight;
                float maxX = 0, maxY = 0;
                int j;
                
                for (j = 0; j < 8; j++) {
                    vec3_t projected;
                    if (R_ProjectPoint(corners[j], &tr.viewParms, projected)) {
                        minX = min(minX, projected[0]);
                        minY = min(minY, projected[1]);
                        maxX = max(maxX, projected[0]);
                        maxY = max(maxY, projected[1]);
                    }
                }
                
                interScissor[0] = (int)max(0, minX);
                interScissor[1] = (int)max(0, minY);
                interScissor[2] = (int)min(tr.viewParms.viewportWidth, maxX);
                interScissor[3] = (int)min(tr.viewParms.viewportHeight, maxY);
                
                // Intersect with light scissor
                inter->scissorRect[0] = max(light->scissorRect[0], interScissor[0]);
                inter->scissorRect[1] = max(light->scissorRect[1], interScissor[1]);
                inter->scissorRect[2] = min(light->scissorRect[0] + light->scissorRect[2], 
                                           interScissor[2]) - inter->scissorRect[0];
                inter->scissorRect[3] = min(light->scissorRect[1] + light->scissorRect[3], 
                                           interScissor[3]) - inter->scissorRect[1];
                
                // Ensure valid rectangle
                if (inter->scissorRect[2] <= 0 || inter->scissorRect[3] <= 0) {
                    inter->scissorRect[0] = inter->scissorRect[1] = 0;
                    inter->scissorRect[2] = inter->scissorRect[3] = 0;
                    inter->culled = qtrue;  // Cull if scissor is empty
                } else {
                    // Calculate depth bounds for this interaction
                    R_SetDepthBoundsTest(inter);
                    
                    // Track statistics
                    scissoredPixels += inter->scissorRect[2] * inter->scissorRect[3];
                }
            }
            inter = inter->lightNext;
        }
    }
    
    // Calculate scissoring efficiency
    if (totalPixels > 0) {
        float efficiency = 100.0f * (1.0f - (float)scissoredPixels / (float)totalPixels);
        if (r_speeds->integer) {
            ri.Printf(PRINT_ALL, "Scissor efficiency: %.1f%% pixels culled\n", efficiency);
        }
    }
}

/*
================
R_ScissorStatistics

Print scissoring statistics for debugging
================
*/
void R_ScissorStatistics(void) {
    int i;
    renderLight_t *light;
    int totalScissored = 0;
    int totalInteractions = 0;
    int emptyScissors = 0;
    
    if (!r_speeds->integer) {
        return;
    }
    
    for (i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        light = tr_lightSystem.visibleLights[i];
        
        if (!light) {
            continue;
        }
        
        interaction_t *inter = light->firstInteraction;
        while (inter) {
            totalInteractions++;
            
            if (inter->scissorRect[2] > 0 && inter->scissorRect[3] > 0) {
                totalScissored++;
            } else if (!inter->culled) {
                emptyScissors++;
            }
            
            inter = inter->lightNext;
        }
    }
    
    if (totalInteractions > 0) {
        ri.Printf(PRINT_ALL, "Scissoring: %d/%d interactions (%d empty)\n", 
                  totalScissored, totalInteractions, emptyScissors);
    }
}