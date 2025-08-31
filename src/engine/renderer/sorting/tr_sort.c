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
#include "../tr_scene.h"

/*
================================================================================
Phase 2: Sorting and Batching System

This file implements the radix sort algorithm and surface batching optimizations
for the enhanced rendering pipeline.
================================================================================
*/

/*
================
R_GenerateSortKey

Generate a 64-bit sort key for a draw surface
================
*/
sortKey_t R_GenerateSortKey(const drawSurfEnhanced_t *surf) {
    sortKey_t key = 0;
    shader_t *shader = (shader_t*)surf->material;
    renderPass_t pass;
    int materialSort;
    uint16_t depthValue;
    
    // Determine render pass based on shader sort value
    if (shader->sort < SS_OPAQUE) {
        pass = RP_SHADOWMAP;
    } else if (shader->sort <= SS_OPAQUE) {
        pass = RP_OPAQUE;
    } else if (shader->sort < SS_BLEND0) {
        pass = RP_ALPHATEST;
    } else if (shader->sort <= SS_BLEND3) {
        pass = RP_TRANSPARENT;
    } else if (shader->sort < SS_POST_PROCESS) {
        pass = RP_UI;
    } else {
        pass = RP_POSTPROCESS;
    }
    
    // Encode render pass (4 bits)
    key |= ((sortKey_t)pass << SORT_SHIFT_RENDERPASS);
    
    // Material sort value (12 bits)
    materialSort = shader->sortedIndex;
    key |= (((sortKey_t)materialSort & 0xFFF) << SORT_SHIFT_MATERIAL);
    
    // Depth value (16 bits) - different sorting for opaque vs transparent
    if (pass == RP_OPAQUE || pass == RP_ALPHATEST) {
        // Front-to-back for opaque (smaller depth first)
        depthValue = (uint16_t)Q_ftol(surf->viewDepth * 100.0f);
    } else {
        // Back-to-front for transparent (larger depth first)
        depthValue = (uint16_t)Q_ftol((10000.0f - surf->viewDepth) * 100.0f);
    }
    key |= ((sortKey_t)depthValue << SORT_SHIFT_DEPTH);
    
    // Entity number for batching (16 bits)
    key |= (((sortKey_t)surf->entityNum & 0xFFFF) << SORT_SHIFT_ENTITY);
    
    // Surface index for stable sorting (16 bits)
    key |= ((sortKey_t)surf->surfaceNum & 0xFFFF);
    
    return key;
}

/*
================
R_GenerateAllSortKeys

Generate sort keys for all surfaces in the current frame
================
*/
void R_GenerateAllSortKeys(void) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    int i;
    
    for (i = 0; i < scene->numDrawSurfs; i++) {
        scene->drawSurfs[i].sort = R_GenerateSortKey(&scene->drawSurfs[i]);
    }
}

/*
================
R_RadixSort64

Optimized 64-bit radix sort implementation
Uses 4 passes of 16-bit radix sorting
================
*/
void R_RadixSort64(drawSurfEnhanced_t **surfs, int numSurfs) {
    drawSurfEnhanced_t **temp;
    int pass, i, bucket;
    int shift;
    static int counts[65536];
    static int offsets[65536];
    
    if (numSurfs <= 1) {
        return;
    }
    
    // Allocate temporary array for sorting
    temp = (drawSurfEnhanced_t**)R_FrameAlloc(sizeof(drawSurfEnhanced_t*) * numSurfs);
    
    // 4-pass radix sort (16 bits per pass)
    for (pass = 0; pass < 4; pass++) {
        shift = pass * 16;
        
        // Clear counts
        Com_Memset(counts, 0, sizeof(counts));
        
        // Count frequencies
        for (i = 0; i < numSurfs; i++) {
            bucket = (surfs[i]->sort >> shift) & 0xFFFF;
            counts[bucket]++;
        }
        
        // Compute offsets (prefix sum)
        offsets[0] = 0;
        for (i = 1; i < 65536; i++) {
            offsets[i] = offsets[i-1] + counts[i-1];
        }
        
        // Scatter to temp array
        for (i = 0; i < numSurfs; i++) {
            bucket = (surfs[i]->sort >> shift) & 0xFFFF;
            temp[offsets[bucket]++] = surfs[i];
        }
        
        // Copy back to original array
        Com_Memcpy(surfs, temp, sizeof(drawSurfEnhanced_t*) * numSurfs);
    }
}

/*
================
R_SurfacesAreBatchable

Check if two surfaces can be rendered in the same batch
================
*/
qboolean R_SurfacesAreBatchable(drawSurfEnhanced_t *a, drawSurfEnhanced_t *b) {
    shader_t *shaderA, *shaderB;
    
    // Check surface type
    if (a->surfaceTypeValue != b->surfaceTypeValue) {
        return qfalse;
    }
    
    // Both must be from same entity
    if (a->entityNum != b->entityNum) {
        return qfalse;
    }
    
    // Must have same material
    if (a->material != b->material) {
        return qfalse;
    }
    
    // Must have same fog
    if (a->fogNum != b->fogNum) {
        return qfalse;
    }
    
    // Check vertex attributes
    shaderA = (shader_t*)a->material;
    shaderB = (shader_t*)b->material;
    
    if (shaderA->vertexAttribs != shaderB->vertexAttribs) {
        return qfalse;
    }
    
    // Check if both are static world surfaces
    if (a->entityNum == ENTITYNUM_WORLD) {
        // World surfaces can be batched if they're close enough
        float dist = Distance(a->bounds[0], b->bounds[0]);
        if (dist > 1024.0f) {
            return qfalse;
        }
    }
    
    return qtrue;
}

/*
================
R_MergeBatchableSurfaces

Merge surfaces that can be rendered together to reduce draw calls
================
*/
void R_MergeBatchableSurfaces(void) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    int i, writeIndex = 0;
    drawSurfEnhanced_t *prevSurf;
    
    if (scene->numSortedSurfs <= 1) {
        return;
    }
    
    for (i = 0; i < scene->numSortedSurfs; i++) {
        drawSurfEnhanced_t *surf = scene->sortedSurfs[i];
        
        // Check if we can merge with previous surface
        if (writeIndex > 0) {
            prevSurf = scene->sortedSurfs[writeIndex - 1];
            
            if (R_SurfacesAreBatchable(surf, prevSurf)) {
                // Merge dlight masks
                prevSurf->dlightMask |= surf->dlightMask;
                
                // Keep the surface with better depth for sorting
                if (surf->viewDepth < prevSurf->viewDepth) {
                    prevSurf->viewDepth = surf->viewDepth;
                }
                
                // Extend bounds
                AddPointToBounds(surf->bounds[0], prevSurf->bounds[0], prevSurf->bounds[1]);
                AddPointToBounds(surf->bounds[1], prevSurf->bounds[0], prevSurf->bounds[1]);
                
                // Update radius
                if (surf->radius > prevSurf->radius) {
                    prevSurf->radius = surf->radius;
                }
                
                scene->numMerged++;
                
                // Skip this surface (merged with previous)
                continue;
            }
        }
        
        // Keep this surface
        scene->sortedSurfs[writeIndex++] = surf;
    }
    
    // Update count
    scene->numSortedSurfs = writeIndex;
}

/*
================
R_SortDrawSurfs

Main sorting function - sorts all surfaces in the current frame
================
*/
void R_SortDrawSurfs(void) {
    frameScene_t *scene = &frameScenes[currentFrameScene];
    int i;
    
    if (scene->numDrawSurfs == 0) {
        return;
    }
    
    // Allocate pointer array for sorting if needed
    if (!scene->sortedSurfs || scene->maxDrawSurfs > scene->numSortedSurfs) {
        scene->sortedSurfs = (drawSurfEnhanced_t**)R_FrameAlloc(sizeof(drawSurfEnhanced_t*) * scene->maxDrawSurfs);
    }
    
    // Initialize pointers to surfaces
    for (i = 0; i < scene->numDrawSurfs; i++) {
        scene->sortedSurfs[i] = &scene->drawSurfs[i];
    }
    scene->numSortedSurfs = scene->numDrawSurfs;
    
    // Generate sort keys for all surfaces
    R_GenerateAllSortKeys();
    
    // Perform radix sort
    R_RadixSort64(scene->sortedSurfs, scene->numSortedSurfs);
    
    // Optional: Merge consecutive surfaces with same material/entity
    if (r_showBatching && r_showBatching->integer) {
        R_MergeBatchableSurfaces();
    }
}

/*
================
R_DebugDrawSortOrder

Debug visualization of sort order
================
*/
void R_DebugDrawSortOrder(void) {
    frameScene_t *scene;
    int i, pass;
    vec4_t color;
    vec3_t center;
    char text[32];
    
    if (!r_showSortOrder || !r_showSortOrder->integer) {
        return;
    }
    
    scene = &frameScenes[currentFrameScene];
    
    for (i = 0; i < scene->numSortedSurfs; i++) {
        drawSurfEnhanced_t *surf = scene->sortedSurfs[i];
        
        // Color based on render pass
        pass = (surf->sort >> SORT_SHIFT_RENDERPASS) & 0xF;
        switch (pass) {
        case RP_OPAQUE:
            VectorSet4(color, 0, 1, 0, 0.5f);
            break;
        case RP_ALPHATEST:
            VectorSet4(color, 1, 1, 0, 0.5f);
            break;
        case RP_TRANSPARENT:
            VectorSet4(color, 0, 0, 1, 0.5f);
            break;
        default:
            VectorSet4(color, 1, 1, 1, 0.5f);
            break;
        }
        
        // Draw bounding box
        // GL_BindToTMU(tr.whiteImage, TB_COLORMAP);  // TODO: Implement for new system
        qglColor4fv(color);
        
        // Draw box edges
        // qglBegin(GL_LINES);  // TODO: Replace with modern rendering
        // Bottom face
        qglVertex3fv(surf->bounds[0]);
        qglVertex3f(surf->bounds[1][0], surf->bounds[0][1], surf->bounds[0][2]);
        
        qglVertex3f(surf->bounds[1][0], surf->bounds[0][1], surf->bounds[0][2]);
        qglVertex3f(surf->bounds[1][0], surf->bounds[1][1], surf->bounds[0][2]);
        
        qglVertex3f(surf->bounds[1][0], surf->bounds[1][1], surf->bounds[0][2]);
        qglVertex3f(surf->bounds[0][0], surf->bounds[1][1], surf->bounds[0][2]);
        
        qglVertex3f(surf->bounds[0][0], surf->bounds[1][1], surf->bounds[0][2]);
        qglVertex3fv(surf->bounds[0]);
        
        // Top face
        qglVertex3f(surf->bounds[0][0], surf->bounds[0][1], surf->bounds[1][2]);
        qglVertex3f(surf->bounds[1][0], surf->bounds[0][1], surf->bounds[1][2]);
        
        qglVertex3f(surf->bounds[1][0], surf->bounds[0][1], surf->bounds[1][2]);
        qglVertex3fv(surf->bounds[1]);
        
        qglVertex3fv(surf->bounds[1]);
        qglVertex3f(surf->bounds[0][0], surf->bounds[1][1], surf->bounds[1][2]);
        
        qglVertex3f(surf->bounds[0][0], surf->bounds[1][1], surf->bounds[1][2]);
        qglVertex3f(surf->bounds[0][0], surf->bounds[0][1], surf->bounds[1][2]);
        
        // Vertical edges
        qglVertex3fv(surf->bounds[0]);
        qglVertex3f(surf->bounds[0][0], surf->bounds[0][1], surf->bounds[1][2]);
        
        qglVertex3f(surf->bounds[1][0], surf->bounds[0][1], surf->bounds[0][2]);
        qglVertex3f(surf->bounds[1][0], surf->bounds[0][1], surf->bounds[1][2]);
        
        qglVertex3f(surf->bounds[1][0], surf->bounds[1][1], surf->bounds[0][2]);
        qglVertex3fv(surf->bounds[1]);
        
        qglVertex3f(surf->bounds[0][0], surf->bounds[1][1], surf->bounds[0][2]);
        qglVertex3f(surf->bounds[0][0], surf->bounds[1][1], surf->bounds[1][2]);
        qglEnd();
        
        // Draw sort order number at center
        VectorAdd(surf->bounds[0], surf->bounds[1], center);
        VectorScale(center, 0.5f, center);
        
        Com_sprintf(text, sizeof(text), "%d", i);
        // Note: Would need to implement 3D text rendering here
    }
}