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
// tr_scene_graph.h - Hierarchical scene graph for spatial queries

#ifndef TR_SCENE_GRAPH_H
#define TR_SCENE_GRAPH_H

#include "../core/tr_local.h"

// Forward declarations
typedef struct portalArea_s portalArea_t;
typedef struct sceneNode_s sceneNode_t;
typedef struct octreeNode_s octreeNode_t;

#define MAX_SCENE_NODES        16384
#define MAX_NODE_CHILDREN      8
#define MAX_NODE_SURFACES      256
#define MAX_NODE_ENTITIES      64
#define MAX_NODE_LIGHTS        32
#define OCTREE_MAX_DEPTH       12
#define BVH_MAX_DEPTH          16
#define BVH_MIN_NODE_SIZE      32.0f
#define OCTREE_MIN_NODE_SIZE   64.0f

// Node types
typedef enum {
    NODE_TYPE_ROOT,
    NODE_TYPE_AREA,         // Portal area node
    NODE_TYPE_MODEL,        // Static model
    NODE_TYPE_ENTITY,       // Dynamic entity
    NODE_TYPE_LIGHT,        // Light source
    NODE_TYPE_GROUP,        // Group node
    NODE_TYPE_LOD,          // Level of detail node
    NODE_TYPE_SWITCH,       // Conditional node
    NODE_TYPE_TRANSFORM,    // Transform node
    NODE_TYPE_BILLBOARD     // Billboard sprite
} nodeType_t;

// Node flags
#define NODE_FLAG_VISIBLE       0x0001
#define NODE_FLAG_STATIC        0x0002
#define NODE_FLAG_DYNAMIC       0x0004
#define NODE_FLAG_DIRTY         0x0008  // Transform needs update
#define NODE_FLAG_CULLED        0x0010
#define NODE_FLAG_SHADOWS       0x0020
#define NODE_FLAG_TRANSPARENT   0x0040
#define NODE_FLAG_NO_CULL       0x0080
#define NODE_FLAG_ALWAYS_DRAW   0x0100

// Scene node structure
typedef struct sceneNode_s {
    // Identification
    int                 nodeId;
    nodeType_t          type;
    int                 flags;
    char                name[64];
    
    // Hierarchy
    struct sceneNode_s  *parent;
    struct sceneNode_s  *firstChild;
    struct sceneNode_s  *nextSibling;
    int                 numChildren;
    
    // Transform
    vec3_t              origin;
    vec3_t              angles;
    vec3_t              scale;
    quat_t              rotation;       // Quaternion rotation
    mat4_t              localMatrix;    // Local transform
    mat4_t              worldMatrix;    // World transform (cached)
    
    // Bounds
    vec3_t              localMins, localMaxs;   // Local space AABB
    vec3_t              worldMins, worldMaxs;   // World space AABB
    float               boundingRadius;         // World space radius
    
    // Contents
    union {
        model_t         *model;         // Static model
        refEntity_t     *entity;        // Dynamic entity
        dlight_t        *light;         // Dynamic light
        portalArea_t    *area;          // Portal area
        void            *data;          // Generic data
    };
    
    // Surface list (for static models)
    msurface_t          **surfaces;
    int                 numSurfaces;
    int                 firstSurface;
    
    // LOD information
    float               lodDistance[4];    // LOD switch distances
    struct sceneNode_s  *lodNodes[4];      // LOD child nodes
    int                 currentLOD;
    
    // Visibility
    int                 visFrame;          // Last visible frame
    int                 queryFrame;        // Last queried frame
    byte                *visClusters;      // PVS clusters (compatibility)
    portalArea_t        *portalArea;       // Current portal area
    
    // Culling cache
    int                 frustumCullBits;   // Frustum cull result
    float               viewDistance;      // Distance from camera
    
    // User data
    void                *userData;
    int                 userDataSize;
} sceneNode_t;

// Spatial tree node types
typedef enum {
    SPATIAL_OCTREE,
    SPATIAL_BVH,
    SPATIAL_KDTREE,
    SPATIAL_GRID
} spatialNodeType_t;

// Octree node
typedef struct octreeNode_s {
    vec3_t              center;
    float               halfSize;
    vec3_t              mins, maxs;
    
    struct octreeNode_s *children[8];
    struct octreeNode_s *parent;
    
    // Contents
    sceneNode_t         **nodes;
    int                 numNodes;
    int                 maxNodes;
    
    int                 depth;
    qboolean            isLeaf;
} octreeNode_t;

// BVH (Bounding Volume Hierarchy) node
typedef struct bvhNode_s {
    vec3_t              mins, maxs;
    vec3_t              center;
    float               radius;
    
    struct bvhNode_s    *left;
    struct bvhNode_s    *right;
    struct bvhNode_s    *parent;
    
    // Split information
    int                 splitAxis;     // 0=X, 1=Y, 2=Z
    float               splitPos;
    
    // Contents (leaf nodes only)
    sceneNode_t         **nodes;
    int                 numNodes;
    int                 maxNodes;
    
    int                 depth;
    qboolean            isLeaf;
} bvhNode_t;

// Spatial acceleration structure
typedef struct spatialTree_s {
    spatialNodeType_t   type;
    vec3_t              worldMins, worldMaxs;
    
    union {
        octreeNode_t    *octreeRoot;
        bvhNode_t       *bvhRoot;
        void            *root;
    };
    
    // Statistics
    int                 totalNodes;
    int                 leafNodes;
    int                 maxDepth;
    int                 itemCount;
    
    // Node pools
    octreeNode_t        *octreePool;
    int                 octreePoolSize;
    int                 octreePoolUsed;
    
    bvhNode_t           *bvhPool;
    int                 bvhPoolSize;
    int                 bvhPoolUsed;
} spatialTree_t;

// Scene graph manager
typedef struct sceneGraph_s {
    // Root node
    sceneNode_t         *root;
    
    // Node pool
    sceneNode_t         nodes[MAX_SCENE_NODES];
    int                 numNodes;
    sceneNode_t         *freeNodes;    // Free list
    
    // Spatial acceleration
    spatialTree_t       spatialTree;
    
    // Node lists by type
    sceneNode_t         *modelNodes[MAX_MODELS];
    int                 numModelNodes;
    
    sceneNode_t         *entityNodes[MAX_REFENTITIES];
    int                 numEntityNodes;
    
    sceneNode_t         *lightNodes[MAX_DLIGHTS];
    int                 numLightNodes;
    
    // Transform cache
    qboolean            transformsDirty;
    int                 dirtyNodes[MAX_SCENE_NODES];
    int                 numDirtyNodes;
    
    // Statistics
    int                 nodesTraversed;
    int                 nodesCulled;
    int                 nodesDrawn;
} sceneGraph_t;

// Global scene graph
extern sceneGraph_t sceneGraph;

// Scene graph management
void R_InitSceneGraph(void);
void R_ShutdownSceneGraph(void);
void R_ClearSceneGraph(void);
void R_ResetSceneGraph(void);
sceneNode_t* R_AddEntityToSceneGraph(const trRefEntity_t *ent);
void R_UpdateSceneGraph(void);

// Node creation and destruction
sceneNode_t* R_CreateSceneNode(nodeType_t type, const char *name);
sceneNode_t* R_AllocSceneNode(void);
void R_FreeSceneNode(sceneNode_t *node);
void R_DestroySceneNode(sceneNode_t *node);
void R_DestroyNodeTree(sceneNode_t *node);

// Node hierarchy
void R_AddChildNode(sceneNode_t *parent, sceneNode_t *child);
void R_RemoveChildNode(sceneNode_t *parent, sceneNode_t *child);
void R_ReparentNode(sceneNode_t *node, sceneNode_t *newParent);
sceneNode_t* R_FindNode(const char *name);
sceneNode_t* R_FindNodeById(int nodeId);

// Node transforms
void R_SetNodeTransform(sceneNode_t *node, const vec3_t origin, const vec3_t angles, const vec3_t scale);
void R_SetNodeMatrix(sceneNode_t *node, const mat4_t matrix);
void R_UpdateNodeTransform(sceneNode_t *node);
void R_UpdateNodeWorldMatrix(sceneNode_t *node);
void R_UpdateNodeBounds(sceneNode_t *node);
void R_MarkNodeDirty(sceneNode_t *node);
void R_UpdateDirtyNodes(void);

// Node content
void R_SetNodeModel(sceneNode_t *node, model_t *model);
void R_SetNodeEntity(sceneNode_t *node, refEntity_t *entity);
void R_SetNodeLight(sceneNode_t *node, dlight_t *light);
void R_AddNodeSurface(sceneNode_t *node, msurface_t *surface);

// LOD management
void R_SetNodeLOD(sceneNode_t *node, int lodLevel, sceneNode_t *lodNode, float distance);
void R_UpdateNodeLOD(sceneNode_t *node, const vec3_t viewPos);
int R_SelectNodeLOD(sceneNode_t *node, float distance);

// Spatial acceleration structures
void R_BuildSpatialTree(spatialNodeType_t type);
void R_ClearSpatialTree(void);
void R_InsertNodeIntoSpatialTree(sceneNode_t *node);
void R_RemoveNodeFromSpatialTree(sceneNode_t *node);
void R_UpdateNodeInSpatialTree(sceneNode_t *node);

// Octree operations
octreeNode_t* R_CreateOctreeNode(const vec3_t center, float halfSize, int depth);
void R_InsertIntoOctree(octreeNode_t *octree, sceneNode_t *node);
void R_RemoveFromOctree(octreeNode_t *octree, sceneNode_t *node);
void R_QueryOctree(octreeNode_t *octree, const vec3_t mins, const vec3_t maxs, sceneNode_t **results, int *numResults);
void R_FrustumQueryOctree(octreeNode_t *octree, const cplane_t frustum[6], sceneNode_t **results, int *numResults);

// BVH operations
bvhNode_t* R_CreateBVHNode(void);
void R_BuildBVH(bvhNode_t *node, sceneNode_t **nodes, int numNodes);
void R_InsertIntoBVH(bvhNode_t *bvh, sceneNode_t *node);
void R_QueryBVH(bvhNode_t *bvh, const vec3_t mins, const vec3_t maxs, sceneNode_t **results, int *numResults);
void R_FrustumQueryBVH(bvhNode_t *bvh, const cplane_t frustum[6], sceneNode_t **results, int *numResults);
void R_RayQueryBVH(bvhNode_t *bvh, const vec3_t start, const vec3_t end, sceneNode_t **results, int *numResults);

// Scene traversal
void R_TraverseSceneGraph(sceneNode_t *node, void (*callback)(sceneNode_t *node, void *data), void *data);
void R_TraverseVisibleNodes(sceneNode_t *node, const cplane_t frustum[6], void (*callback)(sceneNode_t *node, void *data), void *data);
void R_CollectVisibleNodes(sceneNode_t *node, const cplane_t frustum[6], sceneNode_t **visibleNodes, int *numVisible);

// Culling
qboolean R_CullNode(const sceneNode_t *node, const cplane_t frustum[6]);
qboolean R_CullNodeBounds(const vec3_t mins, const vec3_t maxs, const cplane_t frustum[6]);
int R_NodeFrustumBits(const sceneNode_t *node, const cplane_t frustum[6]);
void R_UpdateNodeCulling(sceneNode_t *node, const cplane_t frustum[6]);

// Sorting
void R_SortNodesByDistance(sceneNode_t **nodes, int numNodes, const vec3_t viewPos);
void R_SortNodesByMaterial(sceneNode_t **nodes, int numNodes);
void R_SortTransparentNodes(sceneNode_t **nodes, int numNodes, const vec3_t viewPos);

// Statistics and debugging
void R_PrintSceneGraphStats(void);
void R_DrawSceneGraphBounds(void);
void R_DrawSpatialTree(void);
void R_ValidateSceneGraph(void);

// Integration with rendering
void R_SetupSceneNodes(void);
void R_AddSceneNodeSurfaces(sceneNode_t *node);
void R_RenderSceneNode(sceneNode_t *node);
void R_RenderVisibleNodes(void);

// Compatibility with existing systems
void R_LinkEntityToSceneGraph(refEntity_t *entity);
void R_LinkLightToSceneGraph(dlight_t *light);
void R_LinkModelToSceneGraph(model_t *model);

#endif // TR_SCENE_GRAPH_H