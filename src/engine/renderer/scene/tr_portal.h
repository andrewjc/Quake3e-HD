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
// tr_portal.h - Portal-based visibility system

#ifndef TR_PORTAL_H
#define TR_PORTAL_H

#include "../core/tr_local.h"

#define MAX_PORTAL_AREAS        1024
#define MAX_PORTALS             2048
#define MAX_AREA_PORTALS        32
#define MAX_PORTAL_POINTS       32
#define MAX_PORTAL_DEPTH        16
#define MAX_VISIBLE_AREAS       256

// Forward declarations
typedef struct portalArea_s portalArea_t;
typedef struct portal_s portal_t;

// Portal states
typedef enum {
    PORTAL_OPEN = 0,
    PORTAL_CLOSED,
    PORTAL_BLOCKED,     // Temporarily blocked by door/entity
    PORTAL_MIRROR,      // Mirror portal
    PORTAL_REMOTE       // Teleporter/remote view
} portalState_t;

// Portal structure
typedef struct portal_s {
    int             portalNum;          // Portal identifier
    
    // Geometry
    vec3_t          points[MAX_PORTAL_POINTS]; // Portal polygon vertices
    int             numPoints;          // Vertex count
    cplane_t        plane;              // Portal plane
    vec3_t          bounds[2];          // AABB
    vec3_t          center;             // Center point
    float           radius;             // Bounding sphere radius
    
    // Connectivity
    portalArea_t    *areas[2];          // Connected areas (front/back)
    int             areaNum[2];         // Area indices
    
    // Visibility state
    portalState_t   state;              // Current state
    float           visibility;         // 0-1 transparency/openness
    qboolean        twoSided;           // Visible from both sides
    
    // Scissor rect (screen space)
    int             scissor[4];         // x, y, width, height
    float           minDepth, maxDepth; // Depth range through portal
    
    // For mirrors/remote portals
    mat4_t          transformMatrix;    // View transformation
    portalArea_t    *targetArea;        // Remote area (if not adjacent)
    int             targetAreaNum;
    
    // Rendering
    int             visFrame;           // Last frame this was visible
    int             queryFrame;         // Last frame this was queried
    
    // Linked list
    struct portal_s *next;
} portal_t;

// Portal area structure
typedef struct portalArea_s {
    int             areaNum;            // Area identifier
    vec3_t          bounds[2];          // Area AABB
    vec3_t          center;             // Area center
    float           radius;             // Bounding sphere
    
    // Connected portals
    portal_t        *portals[MAX_AREA_PORTALS];
    int             numPortals;
    
    // Area contents
    int             firstSurface;       // First surface index
    int             numSurfaces;        // Surface count
    msurface_t      **surfaces;         // Surface pointers
    
    // Entities in this area
    int             firstEntity;
    int             numEntities;
    
    // Lights in this area
    int             firstLight;
    int             numLights;
    dlight_t        **lights;           // Static and dynamic lights
    
    // Visibility
    byte            *areaPVS;           // Which areas are potentially visible
    int             visFrame;           // Last frame this was visible
    int             queryFrame;         // Last frame this was queried
    qboolean        skyArea;            // Contains sky surfaces
    qboolean        outsideArea;        // Exterior area
    
    // Fog volume
    int             fogNum;             // Fog index (-1 for none)
    
    // Sound properties
    float           soundAbsorption;    // 0-1 sound dampening
    vec3_t          reverbProperties;   // Reverb parameters
    
    // Statistics
    int             drawSurfCount;      // Surfaces drawn last frame
    int             lightCount;         // Active lights last frame
} portalArea_t;

// Visibility query structure
typedef struct visibilityQuery_s {
    // View parameters
    vec3_t          viewOrigin;
    vec3_t          viewAxis[3];
    float           fovX, fovY;
    float           nearPlane, farPlane;
    
    // Frustum planes
    cplane_t        frustum[6];
    
    // Portal traversal state
    portal_t        *portalChain[MAX_PORTAL_DEPTH];
    cplane_t        portalPlanes[MAX_PORTAL_DEPTH];
    int             scissorStack[MAX_PORTAL_DEPTH][4];
    int             portalDepth;
    int             maxPortalDepth;
    
    // Results
    portalArea_t    *visibleAreas[MAX_VISIBLE_AREAS];
    int             numVisibleAreas;
    portal_t        *visiblePortals[MAX_PORTALS];
    int             numVisiblePortals;
    
    // Statistics
    int             areasChecked;
    int             portalsChecked;
    int             surfacesChecked;
    
    // Current frame
    int             frameNum;
} visibilityQuery_t;

// Portal system state
typedef struct portalSystem_s {
    // Areas and portals
    portalArea_t    areas[MAX_PORTAL_AREAS];
    int             numAreas;
    portal_t        portals[MAX_PORTALS];
    int             numPortals;
    
    // Current visibility query
    visibilityQuery_t currentQuery;
    
    // Area lookup acceleration
    portalArea_t    *areaGrid[64][64][64];  // Spatial grid
    vec3_t          gridMins, gridMaxs;
    vec3_t          gridSize;
    vec3_t          gridCellSize;
    
    // Statistics
    int             totalAreasVisible;
    int             totalPortalsTraversed;
    int             maxDepthReached;
    
    // Debugging
    qboolean        showPortals;
    qboolean        showAreas;
    qboolean        lockPVS;
    portalArea_t    *lockedArea;
} portalSystem_t;

// Global portal system
extern portalSystem_t portalSystem;

// Portal system functions
void R_InitPortalSystem(void);
void R_ShutdownPortalSystem(void);
void R_LoadPortals(const char *name);
void R_ClearPortals(void);

// Area management
portalArea_t* R_PointInArea(const vec3_t point);
portalArea_t* R_BoxInAreas(const vec3_t mins, const vec3_t maxs, portalArea_t **areas, int maxAreas);
void R_LinkEntityToArea(refEntity_t *entity);
void R_LinkLightToArea(dlight_t *light);
void R_LinkSurfaceToArea(msurface_t *surface);

// Portal management
portal_t* R_CreatePortal(const vec3_t *points, int numPoints, int area1, int area2);
void R_OpenPortal(portal_t *portal);
void R_ClosePortal(portal_t *portal);
void R_SetPortalState(portal_t *portal, portalState_t state);

// Visibility determination
void R_SetupVisibilityQuery(visibilityQuery_t *query, const vec3_t origin, const vec3_t axis[3], float fovX, float fovY);
void R_FindVisibleAreas(visibilityQuery_t *query);
void R_RecursePortals(visibilityQuery_t *query, portalArea_t *area, int depth);
qboolean R_AreaVisible(const portalArea_t *area, const visibilityQuery_t *query);
qboolean R_PortalVisible(const portal_t *portal, const visibilityQuery_t *query);

// Frustum operations
qboolean R_PortalInFrustum(const portal_t *portal, const cplane_t frustum[6]);
void R_PortalFrustum(const portal_t *portal, const cplane_t inFrustum[6], cplane_t outFrustum[6]);
void R_PortalScissor(const portal_t *portal, visibilityQuery_t *query);
void R_ClipFrustumToPortal(const portal_t *portal, cplane_t frustum[6]);

// Utility functions
void R_MarkAreaVisible(visibilityQuery_t *query, portalArea_t *area, portal_t *portal);
void R_BuildAreaConnectivity(void);
void R_UpdateAreaBounds(portalArea_t *area);
float R_AreaDistance(const portalArea_t *area, const vec3_t point);

// Debug visualization
void R_DrawPortals(void);
void R_DrawPortalBounds(const portal_t *portal);
void R_DrawAreaBounds(const portalArea_t *area);
void R_DrawVisibilityPath(const visibilityQuery_t *query);

// Statistics
void R_PrintPortalStats(void);
int R_GetVisibleAreaCount(void);
int R_GetPortalTraversalCount(void);

// Integration with existing systems
qboolean R_UsePortalCulling(void);
void R_SetupPortalFrustum(void);
void R_ApplyPortalScissor(const portal_t *portal);

// Mirror/remote portal support
void R_SetupMirrorView(const portal_t *portal, viewParms_t *view);
void R_SetupRemoteView(const portal_t *portal, viewParms_t *view);
qboolean R_IsMirrorPortal(const portal_t *portal);

// BSP integration
void R_LoadAreaPortals(lump_t *l);
void R_LoadAreas(lump_t *l);
void R_GenerateAreaPortals(void);

// PVS compatibility layer
byte* R_GetAreaPVS(const portalArea_t *area);
void R_DecompressAreaVis(const byte *in, byte *out, const portalArea_t *area);

// Integration with world rendering
void R_AddWorldSurfacesPortal(void);

#endif // TR_PORTAL_H