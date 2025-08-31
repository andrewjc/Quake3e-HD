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
Phase 5: Light-Surface Interaction System

This file implements the interaction system that manages relationships between
lights and surfaces for efficient rendering.
================================================================================
*/

// Forward declarations
void R_BuildLightInteractions(renderLight_t *light);

// Helper macros
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

/*
================
R_CreateInteraction

Create a new interaction between a light and surface
================
*/
interaction_t* R_CreateInteraction(renderLight_t *light, msurface_t *surf) {
    interaction_t *inter;
    interactionManager_t *mgr = &tr_lightSystem.interactionMgr;
    
    // Check if we already have an interaction
    for (inter = light->firstInteraction; inter; inter = inter->lightNext) {
        if (inter->surface == surf) {
            return inter; // Already exists
        }
    }
    
    // Allocate from free list
    if (!mgr->freeList) {
        ri.Printf(PRINT_WARNING, "R_CreateInteraction: Out of interactions\n");
        return NULL;
    }
    
    inter = mgr->freeList;
    mgr->freeList = inter->nextFree;
    mgr->numInteractions++;
    
    // Initialize
    Com_Memset(inter, 0, sizeof(interaction_t));
    inter->light = light;
    inter->surface = surf;
    
    // Link into light's chain
    inter->lightPrev = NULL;
    inter->lightNext = light->firstInteraction;
    if (light->firstInteraction) {
        light->firstInteraction->lightPrev = inter;
    } else {
        light->lastInteraction = inter;
    }
    light->firstInteraction = inter;
    light->numInteractions++;
    
    // Calculate initial properties
    R_UpdateInteraction(inter);
    
    return inter;
}

/*
================
R_FreeInteraction

Free an interaction
================
*/
void R_FreeInteraction(interaction_t *inter) {
    interactionManager_t *mgr;
    
    if (!inter) {
        return;
    }
    
    mgr = &tr_lightSystem.interactionMgr;
    
    // Unlink from chains
    R_UnlinkInteraction(inter);
    
    // Free triangle data
    if (inter->lightTris) {
        // TODO: Free triangle data
        inter->lightTris = NULL;
    }
    if (inter->shadowTris) {
        // TODO: Free shadow triangle data
        inter->shadowTris = NULL;
    }
    
    // Return to free list
    inter->nextFree = mgr->freeList;
    mgr->freeList = inter;
    mgr->numInteractions--;
}

/*
================
R_UpdateInteraction

Update interaction properties
================
*/
void R_UpdateInteraction(interaction_t *inter) {
    renderLight_t *light;
    msurface_t *surf;
    vec3_t lightDir;
    float distance;
    
    if (!inter) {
        return;
    }
    
    light = inter->light;
    surf = inter->surface;
    
    // For now, use light bounds as interaction bounds
    // TODO: Calculate actual surface bounds from surface data
    VectorCopy(light->mins, inter->bounds[0]);
    VectorCopy(light->maxs, inter->bounds[1]);
    
    // Check if intersection is empty
    if (inter->bounds[0][0] > inter->bounds[1][0] ||
        inter->bounds[0][1] > inter->bounds[1][1] ||
        inter->bounds[0][2] > inter->bounds[1][2]) {
        inter->isEmpty = qtrue;
        return;
    }
    
    inter->isEmpty = qfalse;
    
    // Determine if surface can cast shadows
    if (surf->shader && (surf->shader->surfaceFlags & SURF_NOSHADOWS)) {
        inter->castsShadow = qfalse;
    } else if (light->flags & LIGHTFLAG_NOSHADOWS) {
        inter->castsShadow = qfalse;
    } else {
        inter->castsShadow = qtrue;
    }
    
    // Determine if surface receives light
    if (surf->shader && (surf->shader->surfaceFlags & SURF_NOLIGHTMAP)) {
        inter->receivesLight = qfalse;
    } else {
        inter->receivesLight = qtrue;
    }
    
    // Check if static (for caching)
    // TODO: Determine if surface is dynamic based on surface data
    inter->isStatic = light->isStatic;
    
    // Update frame counter
    inter->lastUpdateFrame = tr_lightSystem.frameCount;
}

/*
================
R_LinkInteraction

Link interaction into surface chain
================
*/
void R_LinkInteraction(interaction_t *inter) {
    msurface_t *surf;
    
    if (!inter || !inter->surface) {
        return;
    }
    
    surf = inter->surface;
    
    // Link into surface's chain
    inter->surfacePrev = NULL;
    inter->surfaceNext = surf->firstInteraction;
    if (surf->firstInteraction) {
        surf->firstInteraction->surfacePrev = inter;
    }
    surf->firstInteraction = inter;
}

/*
================
R_UnlinkInteraction

Unlink interaction from all chains
================
*/
void R_UnlinkInteraction(interaction_t *inter) {
    renderLight_t *light;
    msurface_t *surf;
    
    if (!inter) {
        return;
    }
    
    light = inter->light;
    surf = inter->surface;
    
    // Unlink from light chain
    if (light) {
        if (inter->lightPrev) {
            inter->lightPrev->lightNext = inter->lightNext;
        } else {
            light->firstInteraction = inter->lightNext;
        }
        
        if (inter->lightNext) {
            inter->lightNext->lightPrev = inter->lightPrev;
        } else {
            light->lastInteraction = inter->lightPrev;
        }
        
        light->numInteractions--;
    }
    
    // Unlink from surface chain
    if (surf) {
        if (inter->surfacePrev) {
            inter->surfacePrev->surfaceNext = inter->surfaceNext;
        } else {
            surf->firstInteraction = inter->surfaceNext;
        }
        
        if (inter->surfaceNext) {
            inter->surfaceNext->surfacePrev = inter->surfacePrev;
        }
    }
    
    inter->lightPrev = inter->lightNext = NULL;
    inter->surfacePrev = inter->surfaceNext = NULL;
}

/*
================
R_LightAffectsSurface

Check if a light affects a surface
================
*/
qboolean R_LightAffectsSurface(renderLight_t *light, msurface_t *surf) {
    // TODO: Implement proper bounds checking for surfaces
    // For now, do basic shader checks
    
    // Check surface properties
    if (surf->shader) {
        if (surf->shader->surfaceFlags & SURF_NOLIGHTMAP) {
            return qfalse;
        }
        if (surf->shader->surfaceFlags & SURF_SKY) {
            return qfalse;
        }
    }
    
    return qtrue;
}

/*
================
R_BuildLightInteractions

Build interactions for a light
================
*/
void R_BuildLightInteractions(renderLight_t *light) {
    int i;
    msurface_t *surf;
    interaction_t *inter;
    
    // Clear existing interactions if not static
    if (!light->isStatic) {
        while (light->firstInteraction) {
            R_FreeInteraction(light->firstInteraction);
        }
    }
    
    // Test against all world surfaces
    for (i = 0; i < tr.world->numsurfaces; i++) {
        surf = &tr.world->surfaces[i];
        
        if (!R_LightAffectsSurface(light, surf)) {
            continue;
        }
        
        inter = R_CreateInteraction(light, surf);
        if (inter) {
            R_LinkInteraction(inter);
        }
    }
    
    // Test against entity surfaces
    // TODO: Handle entity surfaces
}

/*
================
R_CullInteractions

Cull interactions for current view
================
*/
void R_CullInteractions(void) {
    int i;
    renderLight_t *light;
    interaction_t *inter;
    int numCulled = 0;
    
    for (i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        light = tr_lightSystem.visibleLights[i];
        
        for (inter = light->firstInteraction; inter; inter = inter->lightNext) {
            // Reset culled flag
            inter->culled = qfalse;
            
            // Skip empty interactions
            if (inter->isEmpty) {
                inter->culled = qtrue;
                numCulled++;
                continue;
            }
            
            // Frustum cull interaction bounds
            if (R_CullBox(inter->bounds)) {
                inter->culled = qtrue;
                numCulled++;
                continue;
            }
        }
    }
    
    tr_lightSystem.interactionMgr.numCulled = numCulled;
}

/*
================
R_GatherLights

Gather lights affecting a bounding box
================
*/
int R_GatherLights(const vec3_t mins, const vec3_t maxs, renderLight_t **list, int maxLights) {
    int i, count = 0;
    renderLight_t *light;
    
    for (i = 0; i < tr_lightSystem.numVisibleLights && count < maxLights; i++) {
        light = tr_lightSystem.visibleLights[i];
        
        // Check bounds overlap
        if (light->mins[0] > maxs[0] ||
            light->mins[1] > maxs[1] ||
            light->mins[2] > maxs[2] ||
            light->maxs[0] < mins[0] ||
            light->maxs[1] < mins[1] ||
            light->maxs[2] < mins[2]) {
            continue;
        }
        
        list[count++] = light;
    }
    
    return count;
}

/*
================
R_GetNearestLight

Find the nearest light to a point
================
*/
renderLight_t* R_GetNearestLight(const vec3_t point) {
    int i;
    renderLight_t *light, *nearest = NULL;
    float distance, minDistance = 999999.0f;
    vec3_t delta;
    
    for (i = 0; i < tr_lightSystem.numVisibleLights; i++) {
        light = tr_lightSystem.visibleLights[i];
        
        if (light->type == RL_DIRECTIONAL) {
            continue; // Skip directional lights
        }
        
        VectorSubtract(point, light->origin, delta);
        distance = VectorLength(delta);
        
        if (distance < minDistance && distance <= light->cutoffDistance) {
            minDistance = distance;
            nearest = light;
        }
    }
    
    return nearest;
}