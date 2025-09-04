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
#include "../lighting/tr_light_dynamic.h"
#include "tr_shadow_volume.h"

/*
================================================================================
Phase 8: Shadow Volume Generation (CPU Frontend)

This file implements CPU-side shadow volume generation using silhouette 
detection for pixel-accurate hard shadows.
================================================================================
*/

// Shadow volume memory pool
static shadowVolume_t shadowVolumePool[MAX_SHADOW_VOLUMES];
static int numShadowVolumes;
static shadowVertex_t *shadowVertexBuffer;
static int shadowVertexBufferSize;
static int shadowVertexBufferUsed;

// Edge connectivity cache
static edgeCache_t edgeCache[MAX_EDGE_CACHES];
static int numEdgeCaches;

/*
================
R_InitShadowVolumes

Initialize shadow volume system
================
*/
void R_InitShadowVolumes(void) {
    // Allocate shadow vertex buffer
    shadowVertexBufferSize = MAX_SHADOW_VERTICES;
    shadowVertexBuffer = ri.Hunk_Alloc(sizeof(shadowVertex_t) * shadowVertexBufferSize, h_low);
    
    // Clear pools
    numShadowVolumes = 0;
    shadowVertexBufferUsed = 0;
    numEdgeCaches = 0;
    
    Com_Memset(shadowVolumePool, 0, sizeof(shadowVolumePool));
    Com_Memset(edgeCache, 0, sizeof(edgeCache));
}

/*
================
R_ShutdownShadowVolumes

Cleanup shadow volume system
================
*/
void R_ShutdownShadowVolumes(void) {
    // Clear all caches
    numShadowVolumes = 0;
    shadowVertexBufferUsed = 0;
    numEdgeCaches = 0;
}

/*
================
R_AllocShadowVolume

Allocate a shadow volume from the pool
================
*/
shadowVolume_t* R_AllocShadowVolume(void) {
    shadowVolume_t *volume;
    
    if (numShadowVolumes >= MAX_SHADOW_VOLUMES) {
        ri.Printf(PRINT_WARNING, "R_AllocShadowVolume: Out of shadow volumes\n");
        return NULL;
    }
    
    volume = &shadowVolumePool[numShadowVolumes++];
    Com_Memset(volume, 0, sizeof(shadowVolume_t));
    
    return volume;
}

/*
================
R_ClearShadowVolumes

Clear all shadow volumes for new frame
================
*/
void R_ClearShadowVolumes(void) {
    numShadowVolumes = 0;
    shadowVertexBufferUsed = 0;
}

/*
================
R_AllocShadowVertices

Allocate vertices from the shadow vertex buffer
================
*/
shadowVertex_t* R_AllocShadowVertices(int count) {
    shadowVertex_t *verts;
    
    if (shadowVertexBufferUsed + count > shadowVertexBufferSize) {
        ri.Printf(PRINT_WARNING, "R_AllocShadowVertices: Buffer overflow\n");
        return NULL;
    }
    
    verts = &shadowVertexBuffer[shadowVertexBufferUsed];
    shadowVertexBufferUsed += count;
    
    return verts;
}

/*
================
R_BuildEdgeList

Build edge connectivity information for a surface
================
*/
void R_BuildEdgeList(srfTriangles_t *tri) {
    int i, j, k;
    int numEdges;
    silEdge_t *edges;
    int v1, v2;
    
    if (!tri || tri->numVerts == 0 || tri->numIndexes == 0) {
        return;
    }
    
    // Check if we already have edge data cached
    for (i = 0; i < numEdgeCaches; i++) {
        if (edgeCache[i].surface == tri) {
            return;  // Already cached
        }
    }
    
    // Allocate new cache entry
    if (numEdgeCaches >= MAX_EDGE_CACHES) {
        // Evict oldest entry by shifting array
        int j;
        for (j = 0; j < MAX_EDGE_CACHES - 1; j++) {
            edgeCache[j] = edgeCache[j + 1];
        }
        numEdgeCaches = MAX_EDGE_CACHES - 1;
    }
    
    edgeCache_t *cache = &edgeCache[numEdgeCaches++];
    cache->surface = tri;
    
    // Build edge list
    numEdges = 0;
    edges = cache->edges;
    
    // Process each triangle
    for (i = 0; i < tri->numIndexes; i += 3) {
        // Three edges per triangle
        for (j = 0; j < 3; j++) {
            v1 = tri->indexes[i + j];
            v2 = tri->indexes[i + ((j + 1) % 3)];
            
            // Ensure v1 < v2 for consistent edge direction
            if (v1 > v2) {
                int temp = v1;
                v1 = v2;
                v2 = temp;
            }
            
            // Check if edge already exists
            qboolean found = qfalse;
            for (k = 0; k < numEdges; k++) {
                if (edges[k].v1 == v1 && edges[k].v2 == v2) {
                    // Edge already exists, set second face
                    edges[k].f2 = i / 3;
                    found = qtrue;
                    break;
                }
            }
            
            // Add new edge
            if (!found && numEdges < MAX_EDGES_PER_SURFACE) {
                edges[numEdges].v1 = v1;
                edges[numEdges].v2 = v2;
                edges[numEdges].f1 = i / 3;
                edges[numEdges].f2 = -1;  // No second face yet
                numEdges++;
            }
        }
    }
    
    cache->numEdges = numEdges;
}

/*
================
R_GetTriangleNormal

Calculate normal for a triangle
================
*/
static void R_GetTriangleNormal(srfTriangles_t *tri, int triIndex, vec3_t normal) {
    vec3_t v1, v2;
    int i = triIndex * 3;
    
    VectorSubtract(tri->verts[tri->indexes[i+1]].xyz, 
                   tri->verts[tri->indexes[i]].xyz, v1);
    VectorSubtract(tri->verts[tri->indexes[i+2]].xyz, 
                   tri->verts[tri->indexes[i]].xyz, v2);
    
    CrossProduct(v1, v2, normal);
    VectorNormalize(normal);
}

/*
================
R_FindSilhouetteEdges

Find silhouette edges for shadow volume generation
================
*/
void R_FindSilhouetteEdges(srfTriangles_t *tri, vec3_t lightPos, 
                           silEdge_t *edges, int *numEdges) {
    int i;
    byte *facing;
    edgeCache_t *cache = NULL;
    
    if (!tri || !edges || !numEdges) {
        return;
    }
    
    *numEdges = 0;
    
    // Find edge cache for this surface
    for (i = 0; i < numEdgeCaches; i++) {
        if (edgeCache[i].surface == tri) {
            cache = &edgeCache[i];
            break;
        }
    }
    
    if (!cache) {
        // Build edge list if not cached
        R_BuildEdgeList(tri);
        cache = &edgeCache[numEdgeCaches - 1];
    }
    
    // Allocate facing array (static buffer for simplicity)
    static byte facingBuffer[4096];
    if (tri->numIndexes / 3 > 4096) {
        ri.Printf(PRINT_WARNING, "R_FindSilhouetteEdges: Too many triangles\n");
        return;
    }
    facing = facingBuffer;
    
    // Determine which triangles face the light
    for (i = 0; i < tri->numIndexes / 3; i++) {
        vec3_t normal, toLight;
        
        R_GetTriangleNormal(tri, i, normal);
        VectorSubtract(lightPos, tri->verts[tri->indexes[i*3]].xyz, toLight);
        
        facing[i] = (DotProduct(normal, toLight) > 0) ? 1 : 0;
    }
    
    // Find silhouette edges (edges between facing and non-facing triangles)
    for (i = 0; i < cache->numEdges; i++) {
        silEdge_t *edge = &cache->edges[i];
        
        // Check if this is a silhouette edge
        qboolean f1 = (edge->f1 >= 0) ? facing[edge->f1] : 0;
        qboolean f2 = (edge->f2 >= 0) ? facing[edge->f2] : 0;
        
        if (f1 != f2) {
            // This is a silhouette edge
            edges[*numEdges] = *edge;
            (*numEdges)++;
            
            if (*numEdges >= MAX_SILHOUETTE_EDGES) {
                break;
            }
        }
    }
}

/*
================
R_CreateShadowVolume

Create shadow volume from silhouette edges
================
*/
void R_CreateShadowVolume(renderLight_t *light, srfTriangles_t *tri,
                         shadowVolume_t **volumeOut) {
    silEdge_t edges[MAX_SILHOUETTE_EDGES];
    int numEdges;
    shadowVolume_t *volume;
    shadowVertex_t *verts;
    int i;
    float extrudeDistance;
    
    if (!light || !tri || !volumeOut) {
        return;
    }
    
    *volumeOut = NULL;
    
    // Find silhouette edges
    R_FindSilhouetteEdges(tri, light->origin, edges, &numEdges);
    
    if (numEdges == 0) {
        return;
    }
    
    // Allocate shadow volume
    volume = R_AllocShadowVolume();
    if (!volume) {
        return;
    }
    
    // Allocate vertices (4 per edge for quad)
    volume->numVerts = numEdges * 4;
    verts = R_AllocShadowVertices(volume->numVerts);
    if (!verts) {
        return;
    }
    
    volume->verts = verts;
    
    // Calculate extrusion distance based on light radius
    if (light->type == RL_OMNI) {
        extrudeDistance = light->radius * 2.0f;
    } else {
        extrudeDistance = 10000.0f;  // Large value for directional lights
    }
    
    // Build shadow volume quads from silhouette edges
    for (i = 0; i < numEdges; i++) {
        vec3_t v1, v2, v3, v4;
        vec3_t dir1, dir2;
        
        // Get edge vertices
        VectorCopy(tri->verts[edges[i].v1].xyz, v1);
        VectorCopy(tri->verts[edges[i].v2].xyz, v2);
        
        // Calculate extrusion direction
        VectorSubtract(v1, light->origin, dir1);
        VectorNormalize(dir1);
        VectorSubtract(v2, light->origin, dir2);
        VectorNormalize(dir2);
        
        // Extrude to create far vertices
        VectorMA(v1, extrudeDistance, dir1, v3);
        VectorMA(v2, extrudeDistance, dir2, v4);
        
        // Create quad (two triangles)
        // Near edge
        VectorCopy(v1, verts[i*4+0].xyz);
        VectorCopy(v2, verts[i*4+1].xyz);
        // Far edge
        VectorCopy(v4, verts[i*4+2].xyz);
        VectorCopy(v3, verts[i*4+3].xyz);
    }
    
    // Store light reference
    volume->light = light;
    volume->surface = tri;
    
    // Output the volume
    *volumeOut = volume;
}

/*
================
R_AddShadowVolumeCaps

Add near and far caps to shadow volume
================
*/
void R_AddShadowVolumeCaps(shadowVolume_t *volume, renderLight_t *light) {
    srfTriangles_t *tri;
    int i, j;
    byte *facing;
    shadowVertex_t *capVerts;
    int numCapVerts;
    float extrudeDistance;
    
    if (!volume || !light || !volume->surface) {
        return;
    }
    
    tri = volume->surface;
    
    // Allocate facing array (static buffer for simplicity)
    static byte facingBuffer[4096];
    if (tri->numIndexes / 3 > 4096) {
        ri.Printf(PRINT_WARNING, "R_FindSilhouetteEdges: Too many triangles\n");
        return;
    }
    facing = facingBuffer;
    
    // Determine which triangles face the light
    for (i = 0; i < tri->numIndexes / 3; i++) {
        vec3_t normal, toLight;
        
        R_GetTriangleNormal(tri, i, normal);
        VectorSubtract(light->origin, tri->verts[tri->indexes[i*3]].xyz, toLight);
        
        facing[i] = (DotProduct(normal, toLight) > 0) ? 1 : 0;
    }
    
    // Count cap triangles
    int numNearCap = 0, numFarCap = 0;
    for (i = 0; i < tri->numIndexes / 3; i++) {
        if (facing[i]) numNearCap++;
        else numFarCap++;
    }
    
    // Allocate cap vertices
    numCapVerts = (numNearCap + numFarCap) * 3;
    capVerts = R_AllocShadowVertices(numCapVerts);
    if (!capVerts) {
        return;
    }
    
    // Calculate extrusion distance
    if (light->type == RL_OMNI) {
        extrudeDistance = light->radius * 2.0f;
    } else {
        extrudeDistance = 10000.0f;
    }
    
    int vertIndex = 0;
    
    // Add near cap (light-facing triangles)
    for (i = 0; i < tri->numIndexes / 3; i++) {
        if (facing[i]) {
            for (j = 0; j < 3; j++) {
                VectorCopy(tri->verts[tri->indexes[i*3+j]].xyz, 
                          capVerts[vertIndex++].xyz);
            }
        }
    }
    
    // Add far cap (extruded back-facing triangles)
    for (i = 0; i < tri->numIndexes / 3; i++) {
        if (!facing[i]) {
            for (j = 0; j < 3; j++) {
                vec3_t dir;
                VectorSubtract(tri->verts[tri->indexes[i*3+j]].xyz, 
                              light->origin, dir);
                VectorNormalize(dir);
                VectorMA(tri->verts[tri->indexes[i*3+j]].xyz, 
                        extrudeDistance, dir, capVerts[vertIndex++].xyz);
            }
        }
    }
    
    // Store cap information
    volume->numCapVerts = numCapVerts;
    volume->capVerts = capVerts;
}

/*
================
R_OptimizeShadowVolume

Optimize shadow volume for rendering
================
*/
void R_OptimizeShadowVolume(shadowVolume_t *volume) {
    if (!volume) {
        return;
    }
    
    // Remove degenerate quads
    // Merge duplicate vertices
    // Sort for cache efficiency
    
    // This is a placeholder for optimization routines
    // Actual implementation would include:
    // - Vertex deduplication
    // - Index buffer generation
    // - Cache optimization
}

/*
================
R_CacheShadowVolume

Cache static shadow volumes
================
*/
void R_CacheShadowVolume(shadowVolume_t *volume) {
    if (!volume || !volume->light || !volume->surface) {
        return;
    }
    
    // Only cache for static lights and surfaces
    if (!volume->light->isStatic) {
        return;
    }
    
    // Mark as cached
    volume->isCached = qtrue;
    
    // In a real implementation, this would:
    // - Store the volume in a persistent cache
    // - Create GPU buffers for the volume
    // - Index by light/surface pair for quick lookup
}

/*
================
R_GetCachedShadowVolume

Retrieve cached shadow volume if available
================
*/
shadowVolume_t* R_GetCachedShadowVolume(renderLight_t *light, srfTriangles_t *surface) {
    int i;
    
    // Search through shadow volumes for matching cached entry
    for (i = 0; i < numShadowVolumes; i++) {
        shadowVolume_t *volume = &shadowVolumePool[i];
        
        if (volume->isCached && 
            volume->light == light && 
            volume->surface == surface) {
            return volume;
        }
    }
    
    return NULL;
}

/*
================
R_ShadowVolumeStatistics

Print shadow volume statistics
================
*/
void R_ShadowVolumeStatistics(void) {
    int totalVerts = 0;
    int cachedVolumes = 0;
    int i;
    
    if (!r_speeds->integer) {
        return;
    }
    
    for (i = 0; i < numShadowVolumes; i++) {
        shadowVolume_t *volume = &shadowVolumePool[i];
        totalVerts += volume->numVerts + volume->numCapVerts;
        if (volume->isCached) {
            cachedVolumes++;
        }
    }
    
    ri.Printf(PRINT_ALL, "Shadow Volumes: %d (%d cached), %d verts\n",
              numShadowVolumes, cachedVolumes, totalVerts);
}