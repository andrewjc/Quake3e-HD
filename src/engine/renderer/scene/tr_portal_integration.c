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
// tr_portal_integration.c - Integration of portal visibility with existing renderer

#include "../core/tr_local.h"
#include "tr_portal.h"
#include "tr_scene_graph.h"

// External functions
void R_AddWorldSurfaces_Original(void);
void R_MarkLeaves_Original(void);

// CVars
static cvar_t *r_portalOnly;
static cvar_t *r_portalBias;

/*
================
R_AddWorldSurfacesPortal

Replacement for R_AddWorldSurfaces that uses portal visibility
================
*/
void R_AddWorldSurfacesPortal(void) {
    visibilityQuery_t *query;
    portalArea_t *area;
    int i, j;
    msurface_t *surf;
    
    if (!r_drawworld->integer) {
        return;
    }
    
    if (tr.refdef.rdflags & RDF_NOWORLDMODEL) {
        return;
    }
    
    tr.currentEntityNum = REFENTITYNUM_WORLD;
    tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;
    
    // Check if portal system is active
    if (!R_UsePortalCulling()) {
        // Fall back to original PVS-based visibility
        R_AddWorldSurfaces_Original();
        return;
    }
    
    // Setup visibility query
    query = &portalSystem.currentQuery;
    R_SetupVisibilityQuery(query, tr.viewParms.origin, tr.viewParms.axis, 
                          tr.viewParms.fovX, tr.viewParms.fovY);
    
    // Find visible areas through portals
    R_FindVisibleAreas(query);
    
    // Clear visible bounds
    ClearBounds(tr.viewParms.visBounds[0], tr.viewParms.visBounds[1]);
    
    // Add surfaces from visible areas
    for (i = 0; i < query->numVisibleAreas; i++) {
        area = query->visibleAreas[i];
        
        // Skip if no surfaces
        if (!area->surfaces || area->numSurfaces == 0) {
            continue;
        }
        
        // Process each surface in the area
        for (j = 0; j < area->numSurfaces; j++) {
            surf = area->surfaces[j];
            if (!surf) continue;
            
            // Skip if already processed this frame
            if (surf->viewCount == tr.viewCount) {
                continue;
            }
            surf->viewCount = tr.viewCount;
            
            // Frustum cull
            if (R_CullSurface(surf->data, surf->shader)) {
                continue;
            }
            
            // Add surface
            R_AddDrawSurf(surf->data, surf->shader, surf->fogIndex, surf->dlightBits);
            
            // Update visible bounds
            AddPointToBounds(surf->cullinfo.bounds[0], 
                           tr.viewParms.visBounds[0], tr.viewParms.visBounds[1]);
            AddPointToBounds(surf->cullinfo.bounds[1], 
                           tr.viewParms.visBounds[0], tr.viewParms.visBounds[1]);
        }
    }
    
    // Add surfaces from scene graph visible nodes
    if (r_useSceneGraph && r_useSceneGraph->integer) {
        sceneNode_t *visibleNodes[MAX_SCENE_NODES];
        int numVisible = 0;
        
        // Update dirty transforms
        R_UpdateDirtyNodes();
        
        // Collect visible nodes
        R_CollectVisibleNodes(sceneGraph.root, tr.viewParms.frustum, 
                            visibleNodes, &numVisible);
        
        // Add surfaces from visible nodes
        for (i = 0; i < numVisible; i++) {
            sceneNode_t *node = visibleNodes[i];
            
            if (node->type == NODE_TYPE_MODEL && node->surfaces) {
                for (j = 0; j < node->numSurfaces; j++) {
                    surf = node->surfaces[j];
                    if (!surf) continue;
                    
                    if (surf->viewCount == tr.viewCount) {
                        continue;
                    }
                    surf->viewCount = tr.viewCount;
                    
                    R_AddDrawSurf(surf->data, surf->shader, 
                                surf->fogIndex, surf->dlightBits);
                }
            }
        }
    }
}

/*
================
R_SetupPortalFrustum

Setup frustum planes for portal culling
================
*/
void R_SetupPortalFrustum(void) {
    int i;
    
    // Use existing frustum setup
    for (i = 0; i < 6; i++) {
        VectorCopy(tr.viewParms.frustum[i].normal, portalSystem.currentQuery.frustum[i].normal);
        portalSystem.currentQuery.frustum[i].dist = tr.viewParms.frustum[i].dist;
        portalSystem.currentQuery.frustum[i].type = tr.viewParms.frustum[i].type;
        SetPlaneSignbits(&portalSystem.currentQuery.frustum[i]);
    }
}

/*
================
R_ProjectPointToScreen

Project a 3D point to screen coordinates
Used by portal scissoring
================
*/
qboolean R_ProjectPointToScreen(const vec3_t point, vec3_t screen) {
    vec4_t eye;
    vec4_t clip;
    vec4_t normalized;
    
    // Transform to eye space
    eye[0] = DotProduct(point, tr.viewParms.axis[0]) - 
             DotProduct(tr.viewParms.origin, tr.viewParms.axis[0]);
    eye[1] = DotProduct(point, tr.viewParms.axis[1]) - 
             DotProduct(tr.viewParms.origin, tr.viewParms.axis[1]);
    eye[2] = DotProduct(point, tr.viewParms.axis[2]) - 
             DotProduct(tr.viewParms.origin, tr.viewParms.axis[2]);
    eye[3] = 1.0f;
    
    // Check if behind viewer
    if (eye[2] >= 0) {
        return qfalse;
    }
    
    // Apply projection matrix
    clip[0] = eye[0] * tr.viewParms.projectionMatrix[0] + 
              eye[1] * tr.viewParms.projectionMatrix[4] + 
              eye[2] * tr.viewParms.projectionMatrix[8] + 
              eye[3] * tr.viewParms.projectionMatrix[12];
    
    clip[1] = eye[0] * tr.viewParms.projectionMatrix[1] + 
              eye[1] * tr.viewParms.projectionMatrix[5] + 
              eye[2] * tr.viewParms.projectionMatrix[9] + 
              eye[3] * tr.viewParms.projectionMatrix[13];
    
    clip[2] = eye[0] * tr.viewParms.projectionMatrix[2] + 
              eye[1] * tr.viewParms.projectionMatrix[6] + 
              eye[2] * tr.viewParms.projectionMatrix[10] + 
              eye[3] * tr.viewParms.projectionMatrix[14];
    
    clip[3] = eye[0] * tr.viewParms.projectionMatrix[3] + 
              eye[1] * tr.viewParms.projectionMatrix[7] + 
              eye[2] * tr.viewParms.projectionMatrix[11] + 
              eye[3] * tr.viewParms.projectionMatrix[15];
    
    // Perspective divide
    if (clip[3] == 0) {
        return qfalse;
    }
    
    normalized[0] = clip[0] / clip[3];
    normalized[1] = clip[1] / clip[3];
    
    // Convert to screen coordinates
    screen[0] = (normalized[0] * 0.5f + 0.5f) * glConfig.vidWidth;
    screen[1] = (1.0f - (normalized[1] * 0.5f + 0.5f)) * glConfig.vidHeight;
    screen[2] = normalized[2];
    
    return qtrue;
}

/*
================
R_AddDebugLine

Add a debug line to be rendered
================
*/
void RB_AddDebugLine(const vec3_t start, const vec3_t end, const vec4_t color) {
    // This would add lines to a debug line buffer
    // Implementation depends on the debug rendering system
}

/*
================
R_AddDebugBox

Add a debug box to be rendered
================
*/
void RB_AddDebugBox(const vec3_t mins, const vec3_t maxs, const vec4_t color) {
    vec3_t corners[8];
    int i;
    
    // Calculate corners
    for (i = 0; i < 8; i++) {
        corners[i][0] = (i & 1) ? maxs[0] : mins[0];
        corners[i][1] = (i & 2) ? maxs[1] : mins[1];
        corners[i][2] = (i & 4) ? maxs[2] : mins[2];
    }
    
    // Draw edges
    RB_AddDebugLine(corners[0], corners[1], color);
    RB_AddDebugLine(corners[2], corners[3], color);
    RB_AddDebugLine(corners[4], corners[5], color);
    RB_AddDebugLine(corners[6], corners[7], color);
    
    RB_AddDebugLine(corners[0], corners[2], color);
    RB_AddDebugLine(corners[1], corners[3], color);
    RB_AddDebugLine(corners[4], corners[6], color);
    RB_AddDebugLine(corners[5], corners[7], color);
    
    RB_AddDebugLine(corners[0], corners[4], color);
    RB_AddDebugLine(corners[1], corners[5], color);
    RB_AddDebugLine(corners[2], corners[6], color);
    RB_AddDebugLine(corners[3], corners[7], color);
}

/*
================
R_AddDebugText

Add debug text at a world position
================
*/
void RB_AddDebugText(const vec3_t origin, float scale, const vec4_t color, const char *text) {
    // This would add text to a debug text buffer
    // Implementation depends on the debug rendering system
}

/*
================
R_CullSurface

Check if a surface is culled by the view frustum
================
*/
qboolean R_CullSurface(surfaceType_t *surface, shader_t *shader) {
    vec3_t bounds[2];
    
    if (!surface) {
        return qtrue;
    }
    
    // Get surface bounds
    switch (*surface) {
    case SF_FACE:
    case SF_GRID:
        {
            srfBspSurface_t *bspSurf = (srfBspSurface_t *)surface;
            VectorCopy(bspSurf->cullinfo.bounds[0], bounds[0]);
            VectorCopy(bspSurf->cullinfo.bounds[1], bounds[1]);
        }
        break;
        
    case SF_TRIANGLES:
        {
            srfTriangles_t *tri = (srfTriangles_t *)surface;
            VectorCopy(tri->bounds[0], bounds[0]);
            VectorCopy(tri->bounds[1], bounds[1]);
        }
        break;
        
    case SF_POLY:
        {
            srfPoly_t *poly = (srfPoly_t *)surface;
            VectorCopy(poly->bounds[0], bounds[0]);
            VectorCopy(poly->bounds[1], bounds[1]);
        }
        break;
        
    default:
        return qfalse;  // Don't cull unknown surface types
    }
    
    // Check against frustum
    return R_CullNodeBounds(bounds[0], bounds[1], tr.viewParms.frustum);
}

/*
================
QuatFromAngles

Convert Euler angles to quaternion
================
*/
void QuatFromAngles(quat_t q, float pitch, float yaw, float roll) {
    float cp = cos(DEG2RAD(pitch * 0.5f));
    float sp = sin(DEG2RAD(pitch * 0.5f));
    float cy = cos(DEG2RAD(yaw * 0.5f));
    float sy = sin(DEG2RAD(yaw * 0.5f));
    float cr = cos(DEG2RAD(roll * 0.5f));
    float sr = sin(DEG2RAD(roll * 0.5f));
    
    q[0] = sr * cp * cy - cr * sp * sy;  // X
    q[1] = cr * sp * cy + sr * cp * sy;  // Y
    q[2] = cr * cp * sy - sr * sp * cy;  // Z
    q[3] = cr * cp * cy + sr * sp * sy;  // W
}

/*
================
QuatIdentity

Set quaternion to identity
================
*/
void QuatIdentity(quat_t q) {
    q[0] = 0;
    q[1] = 0;
    q[2] = 0;
    q[3] = 1;
}

/*
================
MatrixFromQuat

Convert quaternion to rotation matrix
================
*/
void MatrixFromQuat(mat4_t m, const quat_t q) {
    float xx = q[0] * q[0];
    float xy = q[0] * q[1];
    float xz = q[0] * q[2];
    float xw = q[0] * q[3];
    
    float yy = q[1] * q[1];
    float yz = q[1] * q[2];
    float yw = q[1] * q[3];
    
    float zz = q[2] * q[2];
    float zw = q[2] * q[3];
    
    m[0] = 1 - 2 * (yy + zz);
    m[1] = 2 * (xy - zw);
    m[2] = 2 * (xz + yw);
    m[3] = 0;
    
    m[4] = 2 * (xy + zw);
    m[5] = 1 - 2 * (xx + zz);
    m[6] = 2 * (yz - xw);
    m[7] = 0;
    
    m[8] = 2 * (xz - yw);
    m[9] = 2 * (yz + xw);
    m[10] = 1 - 2 * (xx + yy);
    m[11] = 0;
    
    m[12] = 0;
    m[13] = 0;
    m[14] = 0;
    m[15] = 1;
}

/*
================
MatrixScale

Apply scale to matrix
================
*/
void MatrixScale(mat4_t m, float x, float y, float z) {
    m[0] *= x;
    m[1] *= x;
    m[2] *= x;
    
    m[4] *= y;
    m[5] *= y;
    m[6] *= y;
    
    m[8] *= z;
    m[9] *= z;
    m[10] *= z;
}

/*
================
MatrixSetTranslation

Set translation in matrix
================
*/
void MatrixSetTranslation(mat4_t m, const vec3_t t) {
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
}

/*
================
MatrixTransformPoint

Transform a point by a matrix
================
*/
void MatrixTransformPoint(const mat4_t m, const vec3_t in, vec3_t out) {
    vec3_t temp;
    
    temp[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12];
    temp[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13];
    temp[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
    
    VectorCopy(temp, out);
}