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
// tr_portal.c - Portal-based visibility system implementation

#include "../core/tr_local.h"
#include "../core/tr_common_utils.h"
#include "../core/profiling/tr_debug.h"
#include "tr_portal.h"

// Global portal system
portalSystem_t portalSystem;

// CVars for portal system control
static cvar_t *r_usePortals;
static cvar_t *r_showPortals;
static cvar_t *r_showAreas;
static cvar_t *r_lockPVS;
static cvar_t *r_maxPortalDepth;
static cvar_t *r_portalDebug;
static cvar_t *r_areaDebug;

/*
================
R_InitPortalSystem

Initialize the portal visibility system
================
*/
void R_InitPortalSystem(void) {
    Com_Memset(&portalSystem, 0, sizeof(portalSystem));
    
    // Register CVars
    r_usePortals = ri.Cvar_Get("r_usePortals", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(r_usePortals, "Use portal-based visibility culling");
    
    r_showPortals = ri.Cvar_Get("r_showPortals", "0", CVAR_CHEAT);
    ri.Cvar_SetDescription(r_showPortals, "Draw portal boundaries");
    
    r_showAreas = ri.Cvar_Get("r_showAreas", "0", CVAR_CHEAT);
    ri.Cvar_SetDescription(r_showAreas, "Draw area boundaries");
    
    r_lockPVS = ri.Cvar_Get("r_lockPVS", "0", CVAR_CHEAT);
    ri.Cvar_SetDescription(r_lockPVS, "Lock visibility to current area");
    
    r_maxPortalDepth = ri.Cvar_Get("r_maxPortalDepth", "8", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(r_maxPortalDepth, "Maximum portal recursion depth");
    
    r_portalDebug = ri.Cvar_Get("r_portalDebug", "0", CVAR_CHEAT);
    r_areaDebug = ri.Cvar_Get("r_areaDebug", "-1", CVAR_CHEAT);
    
    // Initialize grid
    VectorSet(portalSystem.gridMins, -65536, -65536, -65536);
    VectorSet(portalSystem.gridMaxs, 65536, 65536, 65536);
    VectorSubtract(portalSystem.gridMaxs, portalSystem.gridMins, portalSystem.gridSize);
    portalSystem.gridCellSize[0] = portalSystem.gridSize[0] / 64.0f;
    portalSystem.gridCellSize[1] = portalSystem.gridSize[1] / 64.0f;
    portalSystem.gridCellSize[2] = portalSystem.gridSize[2] / 64.0f;
    
    ri.Printf(PRINT_ALL, "Portal system initialized\n");
}

/*
================
R_ShutdownPortalSystem

Cleanup portal system
================
*/
void R_ShutdownPortalSystem(void) {
    R_ClearPortals();
    Com_Memset(&portalSystem, 0, sizeof(portalSystem));
}

/*
================
R_ClearPortals

Clear all portal data
================
*/
void R_ClearPortals(void) {
    int i;
    
    // Free allocated memory for areas
    for (i = 0; i < portalSystem.numAreas; i++) {
        portalArea_t *area = &portalSystem.areas[i];
        if (area->surfaces) {
            Z_Free(area->surfaces);
            area->surfaces = NULL;
        }
        if (area->lights) {
            Z_Free(area->lights);
            area->lights = NULL;
        }
        if (area->areaPVS) {
            Z_Free(area->areaPVS);
            area->areaPVS = NULL;
        }
    }
    
    // Clear portal system
    Com_Memset(&portalSystem.areas, 0, sizeof(portalSystem.areas));
    Com_Memset(&portalSystem.portals, 0, sizeof(portalSystem.portals));
    Com_Memset(&portalSystem.areaGrid, 0, sizeof(portalSystem.areaGrid));
    
    portalSystem.numAreas = 0;
    portalSystem.numPortals = 0;
}

/*
================
R_PointInArea

Find which area contains a point
================
*/
portalArea_t* R_PointInArea(const vec3_t point) {
    int i;
    int gridX, gridY, gridZ;
    portalArea_t *area;
    
    // Try grid acceleration first
    gridX = (int)((point[0] - portalSystem.gridMins[0]) / portalSystem.gridCellSize[0]);
    gridY = (int)((point[1] - portalSystem.gridMins[1]) / portalSystem.gridCellSize[1]);
    gridZ = (int)((point[2] - portalSystem.gridMins[2]) / portalSystem.gridCellSize[2]);
    
    if (gridX >= 0 && gridX < 64 && gridY >= 0 && gridY < 64 && gridZ >= 0 && gridZ < 64) {
        area = portalSystem.areaGrid[gridX][gridY][gridZ];
        if (area) {
            // Verify point is actually in this area
            if (point[0] >= area->bounds[0][0] && point[0] <= area->bounds[1][0] &&
                point[1] >= area->bounds[0][1] && point[1] <= area->bounds[1][1] &&
                point[2] >= area->bounds[0][2] && point[2] <= area->bounds[1][2]) {
                return area;
            }
        }
    }
    
    // Fallback to linear search
    for (i = 0; i < portalSystem.numAreas; i++) {
        area = &portalSystem.areas[i];
        
        if (point[0] >= area->bounds[0][0] && point[0] <= area->bounds[1][0] &&
            point[1] >= area->bounds[0][1] && point[1] <= area->bounds[1][1] &&
            point[2] >= area->bounds[0][2] && point[2] <= area->bounds[1][2]) {
            return area;
        }
    }
    
    return NULL;  // Point is outside all areas
}

/*
================
R_BoxInAreas

Find all areas that intersect a bounding box
================
*/
portalArea_t* R_BoxInAreas(const vec3_t mins, const vec3_t maxs, portalArea_t **areas, int maxAreas) {
    int i, count = 0;
    portalArea_t *area;
    
    for (i = 0; i < portalSystem.numAreas && count < maxAreas; i++) {
        area = &portalSystem.areas[i];
        
        // Check AABB intersection
        if (mins[0] <= area->bounds[1][0] && maxs[0] >= area->bounds[0][0] &&
            mins[1] <= area->bounds[1][1] && maxs[1] >= area->bounds[0][1] &&
            mins[2] <= area->bounds[1][2] && maxs[2] >= area->bounds[0][2]) {
            areas[count++] = area;
        }
    }
    
    return count > 0 ? areas[0] : NULL;
}

/*
================
R_CreatePortal

Create a portal between two areas
================
*/
portal_t* R_CreatePortal(const vec3_t *points, int numPoints, int area1, int area2) {
    portal_t *portal;
    portalArea_t *a1, *a2;
    int i;
    vec3_t normal;
    float dist;
    
    if (portalSystem.numPortals >= MAX_PORTALS) {
        ri.Printf(PRINT_WARNING, "R_CreatePortal: MAX_PORTALS reached\n");
        return NULL;
    }
    
    if (area1 < 0 || area1 >= portalSystem.numAreas ||
        area2 < 0 || area2 >= portalSystem.numAreas) {
        ri.Printf(PRINT_WARNING, "R_CreatePortal: invalid area numbers\n");
        return NULL;
    }
    
    portal = &portalSystem.portals[portalSystem.numPortals++];
    Com_Memset(portal, 0, sizeof(*portal));
    
    portal->portalNum = portalSystem.numPortals - 1;
    portal->numPoints = numPoints;
    
    // Copy points and calculate bounds
    ClearBounds(portal->bounds[0], portal->bounds[1]);
    VectorClear(portal->center);
    
    for (i = 0; i < numPoints && i < MAX_PORTAL_POINTS; i++) {
        VectorCopy(points[i], portal->points[i]);
        AddPointToBounds(points[i], portal->bounds[0], portal->bounds[1]);
        VectorAdd(portal->center, points[i], portal->center);
    }
    
    VectorScale(portal->center, 1.0f / numPoints, portal->center);
    portal->radius = 0;
    
    for (i = 0; i < numPoints; i++) {
        float d = Distance(portal->points[i], portal->center);
        if (d > portal->radius) {
            portal->radius = d;
        }
    }
    
    // Calculate portal plane
    if (numPoints >= 3) {
        vec3_t v1, v2;
        
        VectorSubtract(points[1], points[0], v1);
        VectorSubtract(points[2], points[0], v2);
        CrossProduct(v1, v2, normal);
        VectorNormalize(normal);
        
        dist = DotProduct(normal, points[0]);
        
        VectorCopy(normal, portal->plane.normal);
        portal->plane.dist = dist;
        portal->plane.type = PlaneTypeForNormal(normal);
        SetPlaneSignbits(&portal->plane);
    }
    
    // Link to areas
    a1 = &portalSystem.areas[area1];
    a2 = &portalSystem.areas[area2];
    
    portal->areas[0] = a1;
    portal->areas[1] = a2;
    portal->areaNum[0] = area1;
    portal->areaNum[1] = area2;
    
    // Add portal to areas
    if (a1->numPortals < MAX_AREA_PORTALS) {
        a1->portals[a1->numPortals++] = portal;
    }
    if (a2->numPortals < MAX_AREA_PORTALS) {
        a2->portals[a2->numPortals++] = portal;
    }
    
    // Default state
    portal->state = PORTAL_OPEN;
    portal->visibility = 1.0f;
    portal->twoSided = qtrue;
    
    return portal;
}

/*
================
R_SetupVisibilityQuery

Setup visibility query for current view
================
*/
void R_SetupVisibilityQuery(visibilityQuery_t *query, const vec3_t origin, const vec3_t axis[3], float fovX, float fovY) {
    int i;
    
    Com_Memset(query, 0, sizeof(*query));
    
    VectorCopy(origin, query->viewOrigin);
    VectorCopy(axis[0], query->viewAxis[0]);
    VectorCopy(axis[1], query->viewAxis[1]);
    VectorCopy(axis[2], query->viewAxis[2]);
    
    query->fovX = fovX;
    query->fovY = fovY;
    query->nearPlane = r_znear->value;
    query->farPlane = tr.viewParms.zFar;
    
    // Setup frustum planes
    for (i = 0; i < 6; i++) {
        VectorCopy(tr.viewParms.frustum[i].normal, query->frustum[i].normal);
        query->frustum[i].dist = tr.viewParms.frustum[i].dist;
        query->frustum[i].type = tr.viewParms.frustum[i].type;
        SetPlaneSignbits(&query->frustum[i]);
    }
    
    query->maxPortalDepth = r_maxPortalDepth->integer;
    query->frameNum = tr.frameCount;
    
    // Initialize scissor to full screen
    query->scissorStack[0][0] = 0;
    query->scissorStack[0][1] = 0;
    query->scissorStack[0][2] = glConfig.vidWidth;
    query->scissorStack[0][3] = glConfig.vidHeight;
}

/*
================
R_FindVisibleAreas

Main visibility determination function
================
*/
void R_FindVisibleAreas(visibilityQuery_t *query) {
    portalArea_t *viewArea;
    
    // Clear results
    query->numVisibleAreas = 0;
    query->numVisiblePortals = 0;
    query->areasChecked = 0;
    query->portalsChecked = 0;
    query->surfacesChecked = 0;
    
    // Handle locked PVS
    if (r_lockPVS->integer && portalSystem.lockedArea) {
        viewArea = portalSystem.lockedArea;
    } else {
        // Find view area
        viewArea = R_PointInArea(query->viewOrigin);
        if (!viewArea) {
            // Camera is outside all areas - use closest area
            float minDist = 999999;
            int i;
            
            for (i = 0; i < portalSystem.numAreas; i++) {
                float dist = R_AreaDistance(&portalSystem.areas[i], query->viewOrigin);
                if (dist < minDist) {
                    minDist = dist;
                    viewArea = &portalSystem.areas[i];
                }
            }
        }
        
        if (r_lockPVS->integer) {
            portalSystem.lockedArea = viewArea;
        }
    }
    
    if (!viewArea) {
        return;  // No areas to render
    }
    
    // Mark view area visible
    R_MarkAreaVisible(query, viewArea, NULL);
    
    // Recursively check portals
    if (r_usePortals->integer) {
        R_RecursePortals(query, viewArea, 0);
    } else {
        // Fallback: mark all areas visible (no portal culling)
        int i;
        for (i = 0; i < portalSystem.numAreas; i++) {
            R_MarkAreaVisible(query, &portalSystem.areas[i], NULL);
        }
    }
    
    // Update statistics
    portalSystem.totalAreasVisible = query->numVisibleAreas;
    portalSystem.totalPortalsTraversed = query->numVisiblePortals;
}

/*
================
R_RecursePortals

Recursively traverse portals to find visible areas
================
*/
void R_RecursePortals(visibilityQuery_t *query, portalArea_t *area, int depth) {
    int i;
    portal_t *portal;
    portalArea_t *nextArea;
    cplane_t newFrustum[6];
    cplane_t oldFrustum[6];
    int oldScissor[4];
    
    if (depth >= query->maxPortalDepth) {
        if (depth > portalSystem.maxDepthReached) {
            portalSystem.maxDepthReached = depth;
        }
        return;
    }
    
    query->areasChecked++;
    
    // Check each portal in this area
    for (i = 0; i < area->numPortals; i++) {
        portal = area->portals[i];
        
        if (!portal || portal->state == PORTAL_CLOSED) {
            continue;
        }
        
        query->portalsChecked++;
        
        // Check if portal is in frustum
        if (!R_PortalInFrustum(portal, query->frustum)) {
            continue;
        }
        
        // Get adjacent area
        nextArea = (portal->areas[0] == area) ? portal->areas[1] : portal->areas[0];
        
        if (!nextArea || nextArea->visFrame == query->frameNum) {
            continue;  // Already processed or invalid
        }
        
        // Save current scissor
        Com_Memcpy(oldScissor, query->scissorStack[depth], sizeof(oldScissor));
        
        // Calculate portal scissor rect
        R_PortalScissor(portal, query);
        
        // Check if scissor is valid
        if (query->scissorStack[depth + 1][2] <= 0 || 
            query->scissorStack[depth + 1][3] <= 0) {
            continue;  // Portal not visible on screen
        }
        
        // Tighten frustum through portal
        Com_Memcpy(oldFrustum, query->frustum, sizeof(cplane_t) * 6);
        R_PortalFrustum(portal, query->frustum, newFrustum);
        Com_Memcpy(query->frustum, newFrustum, sizeof(cplane_t) * 6);
        
        // Add portal to chain
        query->portalChain[depth] = portal;
        query->portalPlanes[depth] = portal->plane;
        
        // Mark area visible
        R_MarkAreaVisible(query, nextArea, portal);
        
        // Mark portal visible
        if (query->numVisiblePortals < MAX_PORTALS) {
            query->visiblePortals[query->numVisiblePortals++] = portal;
            portal->visFrame = query->frameNum;
        }
        
        // Recurse into next area
        R_RecursePortals(query, nextArea, depth + 1);
        
        // Restore frustum and scissor
        Com_Memcpy(query->frustum, oldFrustum, sizeof(cplane_t) * 6);
        Com_Memcpy(query->scissorStack[depth + 1], oldScissor, sizeof(oldScissor));
    }
}

/*
================
R_PortalInFrustum

Check if portal intersects view frustum
================
*/
qboolean R_PortalInFrustum(const portal_t *portal, const cplane_t frustum[6]) {
    int i, j;
    
    for (i = 0; i < 6; i++) {
        const cplane_t *plane = &frustum[i];
        int numBehind = 0;
        
        for (j = 0; j < portal->numPoints; j++) {
            float d = DotProduct(portal->points[j], plane->normal) - plane->dist;
            if (d < 0) {
                numBehind++;
            }
        }
        
        if (numBehind == portal->numPoints) {
            return qfalse;  // All points behind plane
        }
    }
    
    return qtrue;
}

/*
================
R_PortalFrustum

Create tightened frustum through portal
================
*/
void R_PortalFrustum(const portal_t *portal, const cplane_t inFrustum[6], cplane_t outFrustum[6]) {
    int i, j;
    vec3_t edges[MAX_PORTAL_POINTS][2];
    int numEdges;
    
    // Copy original frustum
    Com_Memcpy(outFrustum, inFrustum, sizeof(cplane_t) * 6);
    
    // Build edge list
    numEdges = 0;
    for (i = 0; i < portal->numPoints; i++) {
        j = (i + 1) % portal->numPoints;
        VectorCopy(portal->points[i], edges[numEdges][0]);
        VectorCopy(portal->points[j], edges[numEdges][1]);
        numEdges++;
    }
    
    // Create planes from view origin through each edge
    for (i = 0; i < numEdges && i < 4; i++) {  // Limit to 4 additional planes
        vec3_t v1, v2, normal;
        float dist;
        
        VectorSubtract(edges[i][0], tr.viewParms.or.origin, v1);
        VectorSubtract(edges[i][1], tr.viewParms.or.origin, v2);
        CrossProduct(v1, v2, normal);
        
        if (VectorNormalize(normal) < 0.001f) {
            continue;
        }
        
        dist = DotProduct(normal, tr.viewParms.or.origin);
        
        // Add as additional frustum plane
        VectorCopy(normal, outFrustum[6 + i].normal);
        outFrustum[6 + i].dist = dist;
        outFrustum[6 + i].type = PlaneTypeForNormal(normal);
        SetPlaneSignbits(&outFrustum[6 + i]);
    }
}

/*
================
R_PortalScissor

Calculate screen-space scissor rectangle for portal
================
*/
void R_PortalScissor(const portal_t *portal, visibilityQuery_t *query) {
    int i;
    vec3_t projected;
    float minX = 999999, minY = 999999;
    float maxX = -999999, maxY = -999999;
    int parentScissor[4];
    int depth = query->portalDepth;
    
    // Get parent scissor
    if (depth > 0) {
        Com_Memcpy(parentScissor, query->scissorStack[depth], sizeof(parentScissor));
    } else {
        parentScissor[0] = 0;
        parentScissor[1] = 0;
        parentScissor[2] = glConfig.vidWidth;
        parentScissor[3] = glConfig.vidHeight;
    }
    
    // Project portal points to screen
    for (i = 0; i < portal->numPoints; i++) {
        if (R_ProjectPointToScreen(portal->points[i], &projected[0], &projected[1])) {
            if (projected[0] < minX) minX = projected[0];
            if (projected[0] > maxX) maxX = projected[0];
            if (projected[1] < minY) minY = projected[1];
            if (projected[1] > maxY) maxY = projected[1];
        }
    }
    
    // Convert to integer scissor rect
    int scissor[4];
    scissor[0] = (int)minX;
    scissor[1] = (int)minY;
    scissor[2] = (int)(maxX - minX + 1);
    scissor[3] = (int)(maxY - minY + 1);
    
    // Clip against parent scissor
    int x1 = max(scissor[0], parentScissor[0]);
    int y1 = max(scissor[1], parentScissor[1]);
    int x2 = min(scissor[0] + scissor[2], parentScissor[0] + parentScissor[2]);
    int y2 = min(scissor[1] + scissor[3], parentScissor[1] + parentScissor[3]);
    
    // Store result
    if (depth + 1 < MAX_PORTAL_DEPTH) {
        query->scissorStack[depth + 1][0] = x1;
        query->scissorStack[depth + 1][1] = y1;
        query->scissorStack[depth + 1][2] = max(0, x2 - x1);
        query->scissorStack[depth + 1][3] = max(0, y2 - y1);
    }
}

/*
================
R_MarkAreaVisible

Mark an area as visible in the query
================
*/
void R_MarkAreaVisible(visibilityQuery_t *query, portalArea_t *area, portal_t *portal) {
    if (!area || area->visFrame == query->frameNum) {
        return;  // Already marked
    }
    
    area->visFrame = query->frameNum;
    area->queryFrame = query->frameNum;
    
    if (query->numVisibleAreas < MAX_VISIBLE_AREAS) {
        query->visibleAreas[query->numVisibleAreas++] = area;
    }
    
    // Count surfaces in this area
    query->surfacesChecked += area->numSurfaces;
}

/*
================
R_AreaDistance

Calculate distance from point to area
================
*/
float R_AreaDistance(const portalArea_t *area, const vec3_t point) {
    vec3_t closest;
    int i;
    
    // Find closest point on area bounds
    for (i = 0; i < 3; i++) {
        if (point[i] < area->bounds[0][i]) {
            closest[i] = area->bounds[0][i];
        } else if (point[i] > area->bounds[1][i]) {
            closest[i] = area->bounds[1][i];
        } else {
            closest[i] = point[i];
        }
    }
    
    return Distance(point, closest);
}

/*
================
R_UpdateAreaBounds

Recalculate area bounds from surfaces
================
*/
void R_UpdateAreaBounds(portalArea_t *area) {
    int i, j;
    msurface_t *surf;
    
    if (!area->surfaces || area->numSurfaces == 0) {
        return;
    }
    
    ClearBounds(area->bounds[0], area->bounds[1]);
    
    for (i = 0; i < area->numSurfaces; i++) {
        surf = area->surfaces[i];
        if (!surf) continue;
        
        // Add surface bounds
        for (j = 0; j < 3; j++) {
            if (surf->cullinfo.bounds[0][j] < area->bounds[0][j]) {
                area->bounds[0][j] = surf->cullinfo.bounds[0][j];
            }
            if (surf->cullinfo.bounds[1][j] > area->bounds[1][j]) {
                area->bounds[1][j] = surf->cullinfo.bounds[1][j];
            }
        }
    }
    
    // Calculate center and radius
    VectorAdd(area->bounds[0], area->bounds[1], area->center);
    VectorScale(area->center, 0.5f, area->center);
    
    vec3_t extent;
    VectorSubtract(area->bounds[1], area->bounds[0], extent);
    area->radius = VectorLength(extent) * 0.5f;
}

/*
================
R_LinkSurfaceToArea

Link a surface to an area
================
*/
void R_LinkSurfaceToArea(msurface_t *surface) {
    portalArea_t *area;
    vec3_t center;
    
    if (!surface) {
        return;
    }
    
    // Find surface center
    VectorAdd(surface->cullinfo.bounds[0], surface->cullinfo.bounds[1], center);
    VectorScale(center, 0.5f, center);
    
    // Find containing area
    area = R_PointInArea(center);
    if (!area) {
        return;
    }
    
    // Grow surface array if needed
    if (!area->surfaces) {
        area->surfaces = Z_Malloc(sizeof(msurface_t*) * 1024);
    }
    
    // Add surface to area
    if (area->numSurfaces < 1024) {
        area->surfaces[area->numSurfaces++] = surface;
    }
}

/*
================
R_UsePortalCulling

Check if portal culling should be used
================
*/
qboolean R_UsePortalCulling(void) {
    return r_usePortals->integer && portalSystem.numAreas > 0 && portalSystem.numPortals > 0;
}

/*
================
R_DrawPortals

Debug visualization of portals
================
*/
void R_DrawPortals(void) {
    int i, j;
    portal_t *portal;
    vec4_t color;
    
    if (!r_showPortals->integer) {
        return;
    }
    
    for (i = 0; i < portalSystem.numPortals; i++) {
        portal = &portalSystem.portals[i];
        
        if (portal->visFrame != tr.frameCount && r_showPortals->integer < 2) {
            continue;  // Only show visible portals unless forced
        }
        
        // Set color based on state
        switch (portal->state) {
        case PORTAL_OPEN:
            VectorSet(color, 0, 1, 0);  // Green
            break;
        case PORTAL_CLOSED:
            VectorSet(color, 1, 0, 0);  // Red
            break;
        case PORTAL_BLOCKED:
            VectorSet(color, 1, 1, 0);  // Yellow
            break;
        case PORTAL_MIRROR:
            VectorSet(color, 0, 1, 1);  // Cyan
            break;
        case PORTAL_REMOTE:
            VectorSet(color, 1, 0, 1);  // Magenta
            break;
        default:
            VectorSet(color, 1, 1, 1);  // White
            break;
        }
        
        color[3] = 0.3f + 0.7f * portal->visibility;
        
        // Draw portal polygon
        if (portal->numPoints >= 3) {
            for (j = 0; j < portal->numPoints; j++) {
                int k = (j + 1) % portal->numPoints;
                RB_AddDebugLine(portal->points[j], portal->points[k], color);
            }
        }
        
        // Draw portal normal
        if (r_portalDebug->integer) {
            vec3_t end;
            VectorMA(portal->center, 50, portal->plane.normal, end);
            color[3] = 1.0f;
            RB_AddDebugLine(portal->center, end, color);
        }
    }
}

/*
================
R_DrawAreaBounds

Debug visualization of area bounds
================
*/
void R_DrawAreaBounds(const portalArea_t *area) {
    vec4_t color;
    vec3_t corners[8];
    int i;
    
    if (!r_showAreas->integer || !area) {
        return;
    }
    
    // Set color based on visibility
    if (area->visFrame == tr.frameCount) {
        VectorSet(color, 0, 1, 0);  // Green for visible
    } else {
        VectorSet(color, 1, 0, 0);  // Red for culled
    }
    color[3] = 0.3f;
    
    // Calculate corners
    for (i = 0; i < 8; i++) {
        corners[i][0] = (i & 1) ? area->bounds[1][0] : area->bounds[0][0];
        corners[i][1] = (i & 2) ? area->bounds[1][1] : area->bounds[0][1];
        corners[i][2] = (i & 4) ? area->bounds[1][2] : area->bounds[0][2];
    }
    
    // Draw edges
    RB_AddDebugBox(area->bounds[0], area->bounds[1], color);
    
    // Draw area number at center
    if (r_areaDebug->integer >= 0) {
        char text[32];
        Com_sprintf(text, sizeof(text), "%d", area->areaNum);
        RB_AddDebugText(area->center, 1.0f, color, text);
    }
}

/*
================
R_PrintPortalStats

Print portal system statistics
================
*/
void R_PrintPortalStats(void) {
    ri.Printf(PRINT_ALL, "Portal System Statistics:\n");
    ri.Printf(PRINT_ALL, "  Areas: %d / %d\n", portalSystem.numAreas, MAX_PORTAL_AREAS);
    ri.Printf(PRINT_ALL, "  Portals: %d / %d\n", portalSystem.numPortals, MAX_PORTALS);
    ri.Printf(PRINT_ALL, "  Visible areas: %d\n", portalSystem.totalAreasVisible);
    ri.Printf(PRINT_ALL, "  Portals traversed: %d\n", portalSystem.totalPortalsTraversed);
    ri.Printf(PRINT_ALL, "  Max depth reached: %d\n", portalSystem.maxDepthReached);
}