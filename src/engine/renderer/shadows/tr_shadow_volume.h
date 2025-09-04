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

#ifndef TR_SHADOW_VOLUME_H
#define TR_SHADOW_VOLUME_H

#include "../core/tr_local.h"
#include "../lighting/tr_light_dynamic.h"

/*
================================================================================
Phase 8: Shadow Volume Data Structures

This file defines the structures and constants for CPU-side shadow volume
generation and silhouette detection.
================================================================================
*/

// Constants
#define MAX_SHADOW_VOLUMES      256
#define MAX_SHADOW_VERTICES     65536
#define MAX_SILHOUETTE_EDGES    1024
#define MAX_EDGES_PER_SURFACE   4096
#define MAX_EDGE_CACHES         64

// Shadow vertex structure
typedef struct shadowVertex_s {
    vec3_t          xyz;            // Vertex position
} shadowVertex_t;

// Silhouette edge structure
typedef struct silEdge_s {
    int             v1, v2;         // Vertex indices
    int             f1, f2;         // Adjacent face indices (-1 if none)
} silEdge_t;

// Edge connectivity cache
typedef struct edgeCache_s {
    srfTriangles_t  *surface;       // Surface this cache is for
    int             numEdges;       // Number of edges
    silEdge_t       edges[MAX_EDGES_PER_SURFACE];  // Edge list
} edgeCache_t;

// Shadow volume structure
typedef struct shadowVolume_s {
    renderLight_t   *light;         // Light casting shadow
    srfTriangles_t  *surface;       // Surface casting shadow
    
    // Volume geometry
    shadowVertex_t  *verts;         // Shadow volume vertices
    int             numVerts;       // Number of vertices
    shadowVertex_t  *capVerts;     // Cap vertices
    int             numCapVerts;   // Number of cap vertices
    
    // Optimization flags
    qboolean        isCached;       // Volume is cached
    qboolean        needsRebuild;   // Needs rebuilding
    
    // Statistics
    int             numSilhouetteEdges;  // Number of silhouette edges
    int             frameUsed;          // Last frame used
} shadowVolume_t;

// Function declarations

// System management
void R_InitShadowVolumes(void);
void R_ShutdownShadowVolumes(void);
void R_ClearShadowVolumes(void);

// Shadow volume creation
shadowVolume_t* R_AllocShadowVolume(void);
shadowVertex_t* R_AllocShadowVertices(int count);
void R_CreateShadowVolume(renderLight_t *light, srfTriangles_t *tri, 
                         shadowVolume_t **volumeOut);

// Edge processing
void R_BuildEdgeList(srfTriangles_t *tri);
void R_FindSilhouetteEdges(srfTriangles_t *tri, vec3_t lightPos,
                           silEdge_t *edges, int *numEdges);

// Shadow caps
void R_AddShadowVolumeCaps(shadowVolume_t *volume, renderLight_t *light);

// Optimization
void R_OptimizeShadowVolume(shadowVolume_t *volume);
void R_CacheShadowVolume(shadowVolume_t *volume);
shadowVolume_t* R_GetCachedShadowVolume(renderLight_t *light, srfTriangles_t *surface);

// Statistics
void R_ShadowVolumeStatistics(void);

#endif // TR_SHADOW_VOLUME_H