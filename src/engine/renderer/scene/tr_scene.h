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

#ifndef TR_SCENE_H
#define TR_SCENE_H

/*
================================================================================
Phase 2: Structured Scene Representation with drawSurf_t

This file defines the enhanced draw surface structure and scene management
system for efficient sorting, batching, and rendering of surfaces.
================================================================================
*/

// Extended 64-bit sort key for better sorting granularity
typedef uint64_t sortKey_t;

// Render pass types for sorting
typedef enum {
    RP_SHADOWMAP = 0,      // Shadow map generation
    RP_OPAQUE = 1,         // Opaque geometry
    RP_ALPHATEST = 2,      // Alpha tested surfaces
    RP_TRANSPARENT = 3,    // Transparent surfaces (back-to-front)
    RP_POSTPROCESS = 4,    // Post-process effects
    RP_UI = 5              // UI/HUD elements
} renderPass_t;

// Sort key encoding (64-bit)
#define SORT_SHIFT_RENDERPASS   60  // 4 bits: render pass
#define SORT_SHIFT_MATERIAL     48  // 12 bits: material sort value
#define SORT_SHIFT_DEPTH        32  // 16 bits: depth value
#define SORT_SHIFT_ENTITY       16  // 16 bits: entity number
#define SORT_SHIFT_SURFACE      0   // 16 bits: surface index

#define SORT_MASK_RENDERPASS    0xF000000000000000ULL
#define SORT_MASK_MATERIAL      0x0FFF000000000000ULL
#define SORT_MASK_DEPTH         0x0000FFFF00000000ULL
#define SORT_MASK_ENTITY        0x00000000FFFF0000ULL
#define SORT_MASK_SURFACE       0x000000000000FFFFULL

// Enhanced draw surface structure
typedef struct drawSurfEnhanced_s {
    surfaceType_t       *surface;      // Pointer to any surface structure
    const void          *material;     // Initially shader_t*, later material_t*
    sortKey_t           sort;          // 64-bit sort key
    
    // Additional metadata for optimizations
    float               viewDepth;     // Distance from viewer
    int                 entityNum;     // Source entity
    int                 dlightMask;    // Affected dynamic lights (bitmask)
    int                 fogNum;        // Fog volume index
    
    // Culling data
    vec3_t              bounds[2];     // Surface AABB
    float               radius;        // Bounding sphere radius
    
    // For debugging/profiling
    int                 surfaceNum;    // Original surface index
    int                 surfaceTypeValue; // SF_GRID, SF_TRIANGLES, etc.
} drawSurfEnhanced_t;

// Per-frame scene data
typedef struct frameScene_s {
    // Surface lists
    drawSurfEnhanced_t  *drawSurfs;        // Array of enhanced surfaces
    int             numDrawSurfs;      // Current count
    int             maxDrawSurfs;      // Allocated size
    
    // Sorted surface pointers
    drawSurfEnhanced_t  **sortedSurfs;     // Pointers for sorting
    int             numSortedSurfs;    // May differ due to culling
    
    // Memory pools
    byte            *frameMemory;      // Frame temporary memory
    size_t          frameMemoryUsed;   // Current usage
    size_t          frameMemorySize;   // Total available
    
    // Statistics
    int             numWorldSurfs;     // World surface count
    int             numEntitySurfs;    // Entity surface count
    int             numCulledSurfs;    // Culled surface count
    int             numMerged;         // Merged surface count
} frameScene_t;

// Function declarations

// Initialization
void R_InitFrameScenes(void);
void R_ShutdownFrameScenes(void);

// Frame management
void R_ClearFrameScene(void);
void R_SwapFrameScenes(void);

// Allocation
drawSurfEnhanced_t* R_AllocDrawSurf(void);
void* R_FrameAlloc(size_t size);

// Surface addition
// TODO: These are placeholders - actual implementations need to be created
// void R_AddWorldSurface(msurface_t *surf, shader_t *shader, int dlightMask);
// void R_AddEntitySurfaces(trRefEntity_t *ent);
void R_AddDrawSurf(surfaceType_t *surface, shader_t *shader, int fogIndex, int dlightMap);

// Sorting
sortKey_t R_GenerateSortKey(const drawSurfEnhanced_t *surf);
void R_GenerateAllSortKeys(void);
void R_SortDrawSurfs(void);
void R_RadixSort64(drawSurfEnhanced_t **surfs, int numSurfs);

// Optimization
void R_MergeBatchableSurfaces(void);
qboolean R_SurfacesAreBatchable(drawSurfEnhanced_t *a, drawSurfEnhanced_t *b);

// Debug
void R_DebugDrawSortOrder(void);
void R_PrintSceneStats(void);

// Globals
extern frameScene_t frameScenes[2];
extern int currentFrameScene;

// CVars for debugging
extern cvar_t *r_showSortOrder;
extern cvar_t *r_showBatching;
extern cvar_t *r_showSurfaceStats;

#endif // TR_SCENE_H