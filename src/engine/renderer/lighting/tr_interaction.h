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

#ifndef TR_INTERACTION_H
#define TR_INTERACTION_H

#include "../../common/q_shared.h"  // For vec3_t, qboolean, etc.

// Forward declarations
struct renderLight_s;
struct drawSurf_s;

// Interaction between a light and a surface
typedef struct interaction_s {
    // Linked list pointers (intrusive lists)
    struct interaction_s *lightNext;       // Next surface for this light
    struct interaction_s *lightPrev;       // Previous surface for this light
    struct interaction_s *surfaceNext;     // Next light for this surface  
    struct interaction_s *surfacePrev;     // Previous light for this surface
    
    // References
    struct renderLight_s *light;           // The light
    struct drawSurf_s    *surface;         // The surface
    
    // Cached lighting data
    void                *lightTris;        // Triangles facing the light (srfTriangles_t*)
    int                 numLightTris;      // Number of lit triangles
    void                *shadowTris;       // Shadow volume triangles (srfTriangles_t*)
    int                 numShadowTris;     // Number of shadow triangles
    
    // Culling/visibility
    vec3_t              bounds[2];          // Interaction AABB
    qboolean            culled;             // Culled this frame
    int                 scissorRect[4];     // Screen-space scissor rectangle
    float               depthBounds[2];     // Min/max depth bounds for depth test
    
    // Lighting parameters
    vec3_t              lightVector;       // Surface to light
    float               attenuation;       // Distance attenuation
    vec4_t              diffuseColor;      // Computed diffuse
    vec4_t              specularColor;     // Computed specular
    
    // Optimization flags
    qboolean            isStatic;          // Can be cached
    qboolean            isEmpty;           // No triangles affected
    qboolean            castsShadow;       // Surface casts shadow from light
    qboolean            receivesLight;     // Surface receives light
    
    // Dynamic updates
    int                 lastUpdateFrame;   // Last frame this was updated
    int                 dynamicFrameCount; // Frames since last update
    
    // Memory management
    struct interaction_s *nextFree;        // For free list
    int                 index;             // Interaction pool index
    
    // Vulkan resources (if using Vulkan backend)
#ifdef USE_VULKAN
    VkBuffer            vertexBuffer;      // Interaction vertices
    VkBuffer            indexBuffer;       // Interaction indices
    VkDescriptorSet     descriptorSet;     // Per-interaction data
#endif
    
} interaction_t;

// Interaction management
typedef struct {
    interaction_t       *interactions;      // Pool of interactions
    int                 numInteractions;    // Current count
    int                 maxInteractions;    // Pool size
    interaction_t       *freeList;          // Free interaction list
    
    // Statistics
    int                 numStaticCached;    // Cached static interactions
    int                 numDynamicCreated;  // Dynamic interactions this frame
    int                 numCulled;          // Culled interactions
    int                 numProcessed;       // Processed this frame
    
} interactionManager_t;

// Interaction management functions
interaction_t* R_AllocInteraction(void);
void R_FreeInteraction(interaction_t *inter);
void R_InitInteractionFreeList(void);
void R_ClearAllInteractions(void);

// Interaction generation
void R_GenerateLightInteractions(struct renderLight_s *light, viewParms_t *view);
void R_ProcessInteraction(interaction_t *inter);
interaction_t* R_FindInteraction(struct renderLight_s *light, struct drawSurf_s *surf);

// Interaction culling
void R_CullInteractions(struct renderLight_s *light, viewParms_t *view);
void R_CalculateInteractionBounds(interaction_t *inter);
void R_CalculateInteractionScissor(interaction_t *inter, viewParms_t *view);

// Interaction rendering
void R_AddLightInteractionsToDrawList(viewParms_t *view);
void R_DrawInteraction(interaction_t *inter);

// Interaction optimization
void R_OptimizeStaticInteractions(void);
void R_CompressInteractionMemory(void);

// Debug
void R_PrintInteractionStats(void);

#endif // TR_INTERACTION_H