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
// tr_mesh_shading.h - Mesh shading pipeline for modern GPUs

#ifndef __TR_MESH_SHADING_H
#define __TR_MESH_SHADING_H

#include "../core/tr_local.h"
#include "tr_gpu_driven.h"

// Mesh shader constants
#define MESHLET_MAX_VERTICES    64
#define MESHLET_MAX_PRIMITIVES  126
#define MESHLET_LOCAL_SIZE      32

// Meshlet vertex
typedef struct meshletVertex_s {
    vec3_t      position;
    vec3_t      normal;
    vec2_t      texCoord;
    vec4_t      tangent;
    color4ub_t  color;
} meshletVertex_t;

// Meshlet primitive (triangle)
typedef struct meshletPrimitive_s {
    uint8_t     indices[3];  // Local vertex indices within meshlet
} meshletPrimitive_t;

// Meshlet descriptor
typedef struct meshletDesc_s {
    uint32_t    vertexOffset;
    uint32_t    vertexCount;
    uint32_t    primitiveOffset;
    uint32_t    primitiveCount;
    
    // Culling data
    vec3_t      center;
    float       radius;
    vec3_t      coneAxis;
    float       coneCutoff;
    
    // Bounding box
    vec3_t      mins;
    vec3_t      maxs;
    
    // LOD information
    float       lodError;
    uint32_t    lodLevel;
} meshletDesc_t;

// Mesh shader push constants
typedef struct meshShaderPushConstants_s {
    mat4_t      mvpMatrix;
    mat4_t      modelMatrix;
    mat4_t      normalMatrix;
    vec4_t      viewOrigin;
    uint32_t    meshletOffset;
    uint32_t    meshletCount;
    uint32_t    materialID;
    uint32_t    flags;
} meshShaderPushConstants_t;

// Task shader payload
typedef struct taskShaderPayload_s {
    uint32_t    meshletIndices[32];
    uint32_t    meshletCount;
} taskShaderPayload_t;

// Meshlet building parameters
typedef struct meshletBuildParams_s {
    uint32_t    maxVertices;
    uint32_t    maxPrimitives;
    float       coneWeight;
    qboolean    optimizeVertexCache;
} meshletBuildParams_t;

// Mesh shading surface
typedef struct meshShadingSurface_s {
    meshletDesc_t      *meshlets;
    meshletVertex_t    *vertices;
    meshletPrimitive_t *primitives;
    
    uint32_t            numMeshlets;
    uint32_t            numVertices;
    uint32_t            numPrimitives;
    
    VkBuffer            meshletBuffer;
    VkBuffer            vertexBuffer;
    VkBuffer            primitiveBuffer;
    VkDeviceMemory      bufferMemory;
    
    uint32_t            lodCount;
    uint32_t           *lodMeshletOffsets;
    float              *lodDistances;
} meshShadingSurface_t;

// API functions
qboolean R_InitMeshShading( void );
void R_ShutdownMeshShading( void );

// Meshlet building
void R_BuildMeshlets( srfTriangles_t *surface );
void R_BuildMeshletsFromIndices( const void *vertices, uint32_t vertexCount, uint32_t vertexStride,
                                 const uint16_t *indices, uint32_t indexCount,
                                 meshletBuildParams_t *params,
                                 meshShadingSurface_t *output );

// Meshlet optimization
void R_OptimizeMeshlets( meshShadingSurface_t *surface );
void R_ComputeMeshletCullData( meshletDesc_t *meshlet, const meshletVertex_t *vertices );
void R_BuildMeshletLODs( meshShadingSurface_t *surface );

// Mesh shader rendering
void R_DrawMeshShading( VkCommandBuffer commandBuffer, const drawSurf_t *drawSurf );
void R_DrawMeshShadingSurface( VkCommandBuffer commandBuffer, meshShadingSurface_t *surface,
                               const mat4_t modelMatrix );

// Pipeline management
VkPipeline R_GetMeshShadingPipeline( shader_t *shader, uint32_t stateFlags );
void R_CreateMeshShadingPipelines( void );
void R_DestroyMeshShadingPipelines( void );

// Buffer management
void R_AllocateMeshShadingBuffers( meshShadingSurface_t *surface );
void R_FreeMeshShadingBuffers( meshShadingSurface_t *surface );
void R_UploadMeshShadingData( meshShadingSurface_t *surface );

// Debug visualization
void R_DrawMeshletBounds( meshShadingSurface_t *surface, const mat4_t modelMatrix );
void R_DrawMeshletCones( meshShadingSurface_t *surface, const mat4_t modelMatrix );

#endif // __TR_MESH_SHADING_H