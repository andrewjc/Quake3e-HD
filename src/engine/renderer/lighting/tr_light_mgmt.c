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

#include "../core/tr_local.h"
#include "tr_light_dynamic.h"

#ifdef USE_VULKAN
// Forward declarations for Vulkan shadow rendering
extern void VK_UpdateLightShadowMap(renderLight_t *light);
#endif

/*
================================================================================
Phase 5: Dynamic Light Management System

This file implements the core light management functions for the enhanced
dynamic lighting system.
================================================================================
*/

// Global light system
lightSystem_t tr_lightSystem;

// Performance cvars
cvar_t *r_dynamicLightLimit;
cvar_t *r_lightCullMethod;
cvar_t *r_lightInteractionCull;
cvar_t *r_shadowMapSize;
cvar_t *r_shadowMapLod;

// Helper function for matrix identity
static void MatrixIdentity(mat3_t m) {
    m[0] = 1; m[1] = 0; m[2] = 0;
    m[3] = 0; m[4] = 1; m[5] = 0;
    m[6] = 0; m[7] = 0; m[8] = 1;
}

/*
================
R_InitLightSystem

Initialize the dynamic light system
================
*/
void R_InitLightSystem(void) {
    int i;
    
    ri.Printf(PRINT_ALL, "Initializing dynamic light system...\n");
    
    // Clear the light system
    Com_Memset(&tr_lightSystem, 0, sizeof(tr_lightSystem));
    
    // Register cvars
    r_dynamicLightLimit = ri.Cvar_Get("r_dynamicLightLimit", "32", CVAR_ARCHIVE);
    r_lightCullMethod = ri.Cvar_Get("r_lightCullMethod", "2", CVAR_ARCHIVE);
    r_lightInteractionCull = ri.Cvar_Get("r_lightInteractionCull", "1", CVAR_ARCHIVE);
    r_shadowMapSize = ri.Cvar_Get("r_shadowMapSize", "1024", CVAR_ARCHIVE);
    r_shadowMapLod = ri.Cvar_Get("r_shadowMapLod", "2", CVAR_ARCHIVE);
    
    // Initialize light pool
    for (i = 0; i < MAX_RENDER_LIGHTS; i++) {
        tr_lightSystem.lights[i].index = i;
    }
    
    // Allocate interaction pool
    tr_lightSystem.interactionMgr.maxInteractions = MAX_INTERACTIONS;
    tr_lightSystem.interactionMgr.interactions = ri.Hunk_Alloc(
        sizeof(interaction_t) * MAX_INTERACTIONS, h_low);
    
    // Initialize free list
    tr_lightSystem.interactionMgr.freeList = tr_lightSystem.interactionMgr.interactions;
    for (i = 0; i < MAX_INTERACTIONS - 1; i++) {
        tr_lightSystem.interactionMgr.interactions[i].nextFree = 
            &tr_lightSystem.interactionMgr.interactions[i + 1];
        tr_lightSystem.interactionMgr.interactions[i].index = i;
    }
    tr_lightSystem.interactionMgr.interactions[MAX_INTERACTIONS - 1].nextFree = NULL;
    tr_lightSystem.interactionMgr.interactions[MAX_INTERACTIONS - 1].index = MAX_INTERACTIONS - 1;
    
    ri.Printf(PRINT_ALL, "Light system initialized: %d lights, %d interactions\n",
        MAX_RENDER_LIGHTS, MAX_INTERACTIONS);
}

/*
================
R_ShutdownLightSystem

Shutdown the light system
================
*/
void R_ShutdownLightSystem(void) {
    R_ClearLights();
    Com_Memset(&tr_lightSystem, 0, sizeof(tr_lightSystem));
}

/*
================
R_ClearLights

Clear all lights
================
*/
void R_ClearLights(void) {
    int i;
    
    // Free all interactions
    for (i = 0; i < tr_lightSystem.numLights; i++) {
        renderLight_t *light = &tr_lightSystem.lights[i];
        while (light->firstInteraction) {
            R_FreeInteraction(light->firstInteraction);
        }
    }
    
    // Reset counters
    tr_lightSystem.numLights = 0;
    tr_lightSystem.numActiveLights = 0;
    tr_lightSystem.numVisibleLights = 0;
    tr_lightSystem.numShadowMaps = 0;
    
    // Clear area lists
    Com_Memset(tr_lightSystem.areaLights, 0, sizeof(tr_lightSystem.areaLights));
}

/*
================
R_AllocRenderLight

Allocate a new render light
================
*/
renderLight_t* R_AllocRenderLight(void) {
    renderLight_t *light;
    
    if (tr_lightSystem.numLights >= MAX_RENDER_LIGHTS) {
        ri.Printf(PRINT_WARNING, "R_AllocRenderLight: MAX_RENDER_LIGHTS hit\n");
        return NULL;
    }
    
    light = &tr_lightSystem.lights[tr_lightSystem.numLights++];
    R_InitRenderLight(light);
    
    return light;
}

/*
================
R_FreeRenderLight

Free a render light
================
*/
void R_FreeRenderLight(renderLight_t *light) {
    if (!light) {
        return;
    }
    
    // Free all interactions
    while (light->firstInteraction) {
        R_FreeInteraction(light->firstInteraction);
    }
    
    // Remove from area
    R_RemoveLightFromArea(light);
    
    // Free shadow map
    if (light->shadowMap) {
        R_FreeShadowMap(light->shadowMap);
        light->shadowMap = NULL;
    }
    
    // Mark as inactive
    light->isStatic = qfalse;
    light->needsUpdate = qfalse;
}

/*
================
R_InitRenderLight

Initialize a render light with defaults
================
*/
void R_InitRenderLight(renderLight_t *light) {
    Com_Memset(light, 0, sizeof(renderLight_t));
    
    // Set defaults
    light->type = RL_OMNI;
    light->intensity = 1.0f;
    VectorSet(light->color, 1.0f, 1.0f, 1.0f);
    
    // Default attenuation (roughly inverse square)
    light->constant = 1.0f;
    light->linear = 0.0f;
    light->quadratic = 1.0f;
    light->cutoffDistance = 1000.0f;
    
    // Identity orientation
    MatrixIdentity(light->axis);
    
    // Spot light defaults
    light->fovX = 90.0f;
    light->fovY = 90.0f;
    light->nearClip = 1.0f;
    light->farClip = 1000.0f;
    
    // Shadow defaults
    light->shadowLod = 1;
    light->shadowBias = 0.005f;
    light->shadowSoftness = 1.0f;
    
    light->areaNum = -1;
}

/*
================
R_UpdateRenderLight

Update light matrices and bounds
================
*/
void R_UpdateRenderLight(renderLight_t *light) {
    vec3_t corners[8];
    int i;
    
    // Update bounding box based on light type
    switch (light->type) {
    case RL_OMNI:
        // Sphere bounds
        VectorSet(light->mins, 
            light->origin[0] - light->radius,
            light->origin[1] - light->radius,
            light->origin[2] - light->radius);
        VectorSet(light->maxs,
            light->origin[0] + light->radius,
            light->origin[1] + light->radius,
            light->origin[2] + light->radius);
        break;
        
    case RL_PROJ:
        // Calculate frustum corners and bounds for projected light
        {
            vec3_t forward, right, up;
            vec3_t corners[8];
            float nearDist = light->nearClip;
            float farDist = light->farClip;
            float tanFovX = tan(DEG2RAD(light->fovX * 0.5f));
            float tanFovY = tan(DEG2RAD(light->fovY * 0.5f));
            float nearX = nearDist * tanFovX;
            float nearY = nearDist * tanFovY;
            float farX = farDist * tanFovX;
            float farY = farDist * tanFovY;
            int i;
            
            // Calculate view vectors
            VectorSubtract(light->target, light->origin, forward);
            VectorNormalize(forward);
            
            // Calculate right and up vectors
            if (fabs(forward[2]) < 0.95f) {
                vec3_t worldUp = {0, 0, 1};
                CrossProduct(forward, worldUp, right);
            } else {
                vec3_t worldRight = {1, 0, 0};
                CrossProduct(forward, worldRight, right);
            }
            VectorNormalize(right);
            CrossProduct(right, forward, up);
            
            // Calculate 8 corners of the frustum
            // Near plane corners
            VectorMA(light->origin, nearDist, forward, corners[0]);
            VectorMA(corners[0], -nearX, right, corners[0]);
            VectorMA(corners[0], -nearY, up, corners[0]);
            
            VectorMA(light->origin, nearDist, forward, corners[1]);
            VectorMA(corners[1], nearX, right, corners[1]);
            VectorMA(corners[1], -nearY, up, corners[1]);
            
            VectorMA(light->origin, nearDist, forward, corners[2]);
            VectorMA(corners[2], nearX, right, corners[2]);
            VectorMA(corners[2], nearY, up, corners[2]);
            
            VectorMA(light->origin, nearDist, forward, corners[3]);
            VectorMA(corners[3], -nearX, right, corners[3]);
            VectorMA(corners[3], nearY, up, corners[3]);
            
            // Far plane corners
            VectorMA(light->origin, farDist, forward, corners[4]);
            VectorMA(corners[4], -farX, right, corners[4]);
            VectorMA(corners[4], -farY, up, corners[4]);
            
            VectorMA(light->origin, farDist, forward, corners[5]);
            VectorMA(corners[5], farX, right, corners[5]);
            VectorMA(corners[5], -farY, up, corners[5]);
            
            VectorMA(light->origin, farDist, forward, corners[6]);
            VectorMA(corners[6], farX, right, corners[6]);
            VectorMA(corners[6], farY, up, corners[6]);
            
            VectorMA(light->origin, farDist, forward, corners[7]);
            VectorMA(corners[7], -farX, right, corners[7]);
            VectorMA(corners[7], farY, up, corners[7]);
            
            // Calculate bounds from corners
            ClearBounds(light->mins, light->maxs);
            for (i = 0; i < 8; i++) {
                AddPointToBounds(corners[i], light->mins, light->maxs);
            }
        }
        break;
        
    case RL_DIRECTIONAL:
        // Infinite bounds for directional lights
        VectorSet(light->mins, -999999, -999999, -999999);
        VectorSet(light->maxs, 999999, 999999, 999999);
        break;
        
    default:
        break;
    }
    
    // Calculate view matrix for projected lights
    if (light->type == RL_PROJ) {
        vec3_t up = {0, 0, 1};
        vec3_t forward, right;
        
        VectorSubtract(light->target, light->origin, forward);
        VectorNormalize(forward);
        CrossProduct(forward, up, right);
        VectorNormalize(right);
        CrossProduct(right, forward, up);
        
        // Build view matrix
        light->viewMatrix[0] = right[0];
        light->viewMatrix[4] = right[1];
        light->viewMatrix[8] = right[2];
        light->viewMatrix[12] = -DotProduct(right, light->origin);
        
        light->viewMatrix[1] = up[0];
        light->viewMatrix[5] = up[1];
        light->viewMatrix[9] = up[2];
        light->viewMatrix[13] = -DotProduct(up, light->origin);
        
        light->viewMatrix[2] = -forward[0];
        light->viewMatrix[6] = -forward[1];
        light->viewMatrix[10] = -forward[2];
        light->viewMatrix[14] = DotProduct(forward, light->origin);
        
        light->viewMatrix[3] = 0;
        light->viewMatrix[7] = 0;
        light->viewMatrix[11] = 0;
        light->viewMatrix[15] = 1;
        
        // Build projection matrix
        float xScale = 1.0f / tan(DEG2RAD(light->fovX * 0.5f));
        float yScale = 1.0f / tan(DEG2RAD(light->fovY * 0.5f));
        float zScale = light->farClip / (light->farClip - light->nearClip);
        
        Com_Memset(light->projectionMatrix, 0, sizeof(light->projectionMatrix));
        light->projectionMatrix[0] = xScale;
        light->projectionMatrix[5] = yScale;
        light->projectionMatrix[10] = zScale;
        light->projectionMatrix[11] = 1.0f;
        light->projectionMatrix[14] = -light->nearClip * zScale;
    }
    
    light->needsUpdate = qfalse;
    light->lastUpdateFrame = tr_lightSystem.frameCount;
}

/*
================
R_CullLights

Cull lights against view frustum and PVS
================
*/
void R_CullAllLights(void) {
    int i;
    renderLight_t *light;
    
    tr_lightSystem.numVisibleLights = 0;
    tr_lightSystem.totalLightTests = 0;
    tr_lightSystem.totalLightCulled = 0;
    
    for (i = 0; i < tr_lightSystem.numActiveLights; i++) {
        light = tr_lightSystem.activeLights[i];
        
        tr_lightSystem.totalLightTests++;
        
        // Skip if already culled this frame
        if (light->viewCount == tr_lightSystem.visCount) {
            continue;
        }
        
        // Frustum culling
        if (R_CullLightBounds(light)) {
            tr_lightSystem.totalLightCulled++;
            continue;
        }
        
        // PVS culling
        if (light->areaNum >= 0) {
            if (!R_inPVS(tr.viewParms.pvsOrigin, light->origin)) {
                tr_lightSystem.totalLightCulled++;
                continue;
            }
        }
        
        // Add to visible list
        light->viewCount = tr_lightSystem.visCount;
        tr_lightSystem.visibleLights[tr_lightSystem.numVisibleLights++] = light;
    }
}

/*
================
R_CullLightBounds

Check if light bounds are outside frustum
================
*/
qboolean R_CullLightBounds(renderLight_t *light) {
    int i;
    vec3_t bounds[2];
    
    if (light->type == RL_DIRECTIONAL) {
        return qfalse; // Never cull directional lights
    }
    
    VectorCopy(light->mins, bounds[0]);
    VectorCopy(light->maxs, bounds[1]);
    
    // Check against frustum planes
    for (i = 0; i < 4; i++) {
        if (BoxOnPlaneSide(bounds[0], bounds[1], 
            &tr.viewParms.frustum[i]) == 2) {
            return qtrue;
        }
    }
    
    return qfalse;
}

/*
================
R_AddLightToArea

Add light to BSP area list
================
*/
void R_AddLightToArea(renderLight_t *light, int areaNum) {
    if (areaNum < 0 || areaNum >= MAX_MAP_AREAS) {
        return;
    }
    
    // Remove from old area
    R_RemoveLightFromArea(light);
    
    // Add to new area
    light->areaNum = areaNum;
    light->areaNext = tr_lightSystem.areaLights[areaNum];
    tr_lightSystem.areaLights[areaNum] = light;
}

/*
================
R_RemoveLightFromArea

Remove light from its current area
================
*/
void R_RemoveLightFromArea(renderLight_t *light) {
    renderLight_t **prev;
    
    if (light->areaNum < 0) {
        return;
    }
    
    prev = &tr_lightSystem.areaLights[light->areaNum];
    while (*prev) {
        if (*prev == light) {
            *prev = light->areaNext;
            break;
        }
        prev = &(*prev)->areaNext;
    }
    
    light->areaNum = -1;
    light->areaNext = NULL;
}

/*
================
R_ConvertDlightToRenderLight

Convert legacy dlight to new render light
================
*/
void R_ConvertDlightToRenderLight(dlight_t *dl, renderLight_t *rl) {
    R_InitRenderLight(rl);
    
    rl->type = RL_OMNI;
    VectorCopy(dl->origin, rl->origin);
    VectorCopy(dl->color, rl->color);
    rl->radius = dl->radius;
    rl->intensity = 1.0f;
    
    // Set up attenuation to match old behavior
    rl->constant = 0.0f;
    rl->linear = 2.0f / dl->radius;
    rl->quadratic = 1.0f / (dl->radius * dl->radius);
    rl->cutoffDistance = dl->radius;
    
    if (dl->additive) {
        rl->flags |= LIGHTFLAG_NOSHADOWS;
    }
    
    R_UpdateRenderLight(rl);
}

/*
================
R_ProcessDynamicLights

Process legacy dynamic lights
================
*/
void R_ProcessDynamicLights(void) {
    int i;
    dlight_t *dl;
    renderLight_t *rl;
    
    // Clear previous frame's lights
    tr_lightSystem.numActiveLights = 0;
    
    // Convert dlights to render lights
    for (i = 0; i < tr.refdef.num_dlights && i < r_dynamicLightLimit->integer; i++) {
        dl = &tr.refdef.dlights[i];
        
        rl = R_AllocRenderLight();
        if (!rl) {
            break;
        }
        
        R_ConvertDlightToRenderLight(dl, rl);
        tr_lightSystem.activeLights[tr_lightSystem.numActiveLights++] = rl;
    }
}

/*
================
R_UpdateLightSystem

Update the light system for current frame
================
*/
void R_UpdateLightSystem(void) {
    int i;
    
    tr_lightSystem.frameCount++;
    tr_lightSystem.visCount++;
    
    // Process legacy lights
    R_ProcessDynamicLights();
    
    // Update active lights
    for (i = 0; i < tr_lightSystem.numActiveLights; i++) {
        renderLight_t *light = tr_lightSystem.activeLights[i];
        if (light->needsUpdate) {
            R_UpdateRenderLight(light);
        }
    }
    
    // Cull lights
    R_CullLights(&tr.viewParms);
}

/*
================
Light property setters
================
*/
void R_SetLightColor(renderLight_t *light, float r, float g, float b) {
    VectorSet(light->color, r, g, b);
    light->needsUpdate = qtrue;
}

void R_SetLightIntensity(renderLight_t *light, float intensity) {
    light->intensity = intensity;
    light->needsUpdate = qtrue;
}

void R_SetLightAttenuation(renderLight_t *light, float constant, float linear, float quadratic) {
    light->constant = constant;
    light->linear = linear;
    light->quadratic = quadratic;
    light->needsUpdate = qtrue;
}

void R_SetSpotLightAngles(renderLight_t *light, float fovX, float fovY) {
    light->fovX = fovX;
    light->fovY = fovY;
    light->needsUpdate = qtrue;
}

/*
================
Performance monitoring
================
*/
int R_GetActiveLightCount(void) {
    return tr_lightSystem.numActiveLights;
}

int R_GetVisibleLightCount(void) {
    return tr_lightSystem.numVisibleLights;
}

int R_GetInteractionCount(void) {
    return tr_lightSystem.interactionMgr.numInteractions;
}

int R_GetShadowMapCount(void) {
    return tr_lightSystem.numShadowMaps;
}

/*
================
Shadow Map Management
================
*/
shadowMapInfo_t* R_AllocShadowMap(renderLight_t *light) {
    shadowMapInfo_t *shadow;
    
    if (tr_lightSystem.numShadowMaps >= MAX_SHADOW_MAPS) {
        ri.Printf(PRINT_WARNING, "R_AllocShadowMap: MAX_SHADOW_MAPS hit\n");
        return NULL;
    }
    
    shadow = &tr_lightSystem.shadowMaps[tr_lightSystem.numShadowMaps++];
    Com_Memset(shadow, 0, sizeof(shadowMapInfo_t));
    
    shadow->width = r_shadowMapSize->integer;
    shadow->height = r_shadowMapSize->integer;
    shadow->lod = light->shadowLod;
    shadow->needsUpdate = qtrue;
    
    return shadow;
}

void R_FreeShadowMap(shadowMapInfo_t *shadow) {
    if (!shadow) {
        return;
    }
    
    // Free depth texture
    if (shadow->depthImage) {
        // Vulkan image cleanup handled elsewhere
        
        ri.Free(shadow->depthImage);
        shadow->depthImage = NULL;
    }
    
    // Free framebuffer if exists (OpenGL only)
    // Vulkan handles this differently
    
    Com_Memset(shadow, 0, sizeof(shadowMapInfo_t));
}

void R_RenderShadowMap(renderLight_t *light) {
    shadowMapInfo_t *shadow;
    
    if (!light || !light->shadowMap) {
        return;
    }
    
    shadow = light->shadowMap;
    
    // Skip if doesn't need update
    if (!shadow->needsUpdate) {
        return;
    }
    
#ifdef USE_VULKAN
    // Use the Vulkan shadow rendering system
    VK_UpdateLightShadowMap(light);
    shadow->needsUpdate = qfalse;
    return;
#endif
    
    // Calculate and store the light matrices for later use
    if (light->type == RL_PROJ) {
        // Projected light matrices are already calculated
    } else if (light->type == RL_DIRECTIONAL) {
        // Calculate orthographic matrices for directional light
        vec3_t forward, right, up;
        vec3_t viewOrigin;
        float orthoSize = light->radius;
        
        VectorCopy(light->origin, viewOrigin);
        
        // Extract forward direction from axis matrix
        forward[0] = light->axis[0];
        forward[1] = light->axis[1];
        forward[2] = light->axis[2];
        VectorNormalize(forward);
        
        // Build orthonormal basis
        if (fabs(forward[2]) < 0.95f) {
            vec3_t worldUp = {0, 0, 1};
            CrossProduct(forward, worldUp, right);
        } else {
            vec3_t worldRight = {1, 0, 0};
            CrossProduct(forward, worldRight, right);
        }
        VectorNormalize(right);
        CrossProduct(right, forward, up);
        
        // Build and store view matrix
        light->viewMatrix[0] = right[0];
        light->viewMatrix[4] = right[1];
        light->viewMatrix[8] = right[2];
        light->viewMatrix[12] = -DotProduct(right, viewOrigin);
        
        light->viewMatrix[1] = up[0];
        light->viewMatrix[5] = up[1];
        light->viewMatrix[9] = up[2];
        light->viewMatrix[13] = -DotProduct(up, viewOrigin);
        
        light->viewMatrix[2] = -forward[0];
        light->viewMatrix[6] = -forward[1];
        light->viewMatrix[10] = -forward[2];
        light->viewMatrix[14] = DotProduct(forward, viewOrigin);
        
        light->viewMatrix[3] = 0;
        light->viewMatrix[7] = 0;
        light->viewMatrix[11] = 0;
        light->viewMatrix[15] = 1;
        
        // Build orthographic projection matrix
        Com_Memset(light->projectionMatrix, 0, sizeof(light->projectionMatrix));
        light->projectionMatrix[0] = 1.0f / orthoSize;
        light->projectionMatrix[5] = 1.0f / orthoSize;
        light->projectionMatrix[10] = 2.0f / (light->farClip - light->nearClip);
        light->projectionMatrix[14] = -(light->farClip + light->nearClip) / (light->farClip - light->nearClip);
        light->projectionMatrix[15] = 1.0f;
    } else {
        // Perspective projection for point lights
        float fov = 90.0f;
        float aspect = 1.0f;
        float znear = light->nearClip;
        float zfar = light->farClip;
        float yScale = 1.0f / tan(DEG2RAD(fov * 0.5f));
        float xScale = yScale / aspect;
        
        Com_Memset(light->projectionMatrix, 0, sizeof(light->projectionMatrix));
        light->projectionMatrix[0] = xScale;
        light->projectionMatrix[5] = yScale;
        light->projectionMatrix[10] = (zfar + znear) / (zfar - znear);
        light->projectionMatrix[11] = 1.0f;
        light->projectionMatrix[14] = -(2.0f * zfar * znear) / (zfar - znear);
    }
}

void R_BindShadowMap(shadowMapInfo_t *shadow) {
    if (!shadow || !shadow->depthImage) {
        return;
    }
    
    // In Vulkan, shadow map binding is done via descriptor sets
    // Not through direct texture binding like OpenGL
}

void R_SetLightProjectionTexture(renderLight_t *light, const char *name) {
    if (!light || !name) {
        return;
    }
    
    light->projectionImage = R_FindImageFile(name, IMGFLAG_CLAMPTOEDGE);
    light->needsUpdate = qtrue;
}