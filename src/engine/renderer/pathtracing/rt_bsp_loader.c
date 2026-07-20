/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

BSP to RTX Integration
Loads world geometry into RTX acceleration structures
===========================================================================
*/

#include "../core/tr_local.h"
#include "../lighting/tr_light_dynamic.h"
#include "rt_rtx.h"
#include "rt_pathtracer.h"
#include "rt_debug_overlay.h"
#include "rt_volumefx.h"

extern int RTX_GetMaterialIndex(shader_t *shader);

// Maximum vertices/indices per BLAS batch
// Reduced for better stability and multiple BLAS creation
#define MAX_BATCH_VERTS 8192
#define MAX_BATCH_INDICES (MAX_BATCH_VERTS * 3)
#define MAX_BATCH_TRIANGLES (MAX_BATCH_INDICES / 3)

// Batch accumulator for building BLAS
typedef struct {
    vec3_t vertices[MAX_BATCH_VERTS];
    vec3_t normals[MAX_BATCH_VERTS];
    float texCoords[MAX_BATCH_VERTS][2];
    float colors[MAX_BATCH_VERTS][4];
    unsigned int indices[MAX_BATCH_INDICES];
    uint32_t triangleMaterials[MAX_BATCH_TRIANGLES];
    int numVerts;
    int numIndices;
    int numTriangles;
    int numSurfaces;
} rtxBatchBuilder_t;

static rtxBatchBuilder_t batchBuilder;

// Water volumes collected from surfaces with CONTENTS_WATER shaders; used
// to flag underwater triangles (caustic receivers) in the material atlas.
#define RTX_MAX_WATER_VOLUMES 128
typedef struct {
    vec3_t mins;
    vec3_t maxs;
} rtxWaterVolume_t;
static rtxWaterVolume_t rtxWaterVolumes[RTX_MAX_WATER_VOLUMES];
static int rtxNumWaterVolumes;

/*
================
RTX_SurfaceBounds

Axis-aligned bounds of a world surface's vertices. Returns qfalse for
surface types without accessible geometry.
================
*/
static qboolean RTX_SurfaceBounds(const msurface_t *surf, vec3_t mins, vec3_t maxs) {
    surfaceType_t *type;

    if (!surf || !surf->data) {
        return qfalse;
    }

    ClearBounds(mins, maxs);
    type = (surfaceType_t *)surf->data;

    switch (*type) {
    case SF_FACE: {
        const srfSurfaceFace_t *face = (const srfSurfaceFace_t *)surf->data;
        for (int i = 0; i < face->numPoints; i++) {
            AddPointToBounds(face->points[i], mins, maxs);
        }
        return face->numPoints > 0 ? qtrue : qfalse;
    }
    case SF_GRID: {
        const srfGridMesh_t *grid = (const srfGridMesh_t *)surf->data;
        int numVerts = grid->width * grid->height;
        for (int i = 0; i < numVerts; i++) {
            AddPointToBounds(grid->verts[i].xyz, mins, maxs);
        }
        return numVerts > 0 ? qtrue : qfalse;
    }
    case SF_TRIANGLES: {
        const srfTriangles_t *tri = (const srfTriangles_t *)surf->data;
        for (int i = 0; i < tri->numVerts; i++) {
            AddPointToBounds(tri->verts[i].xyz, mins, maxs);
        }
        return tri->numVerts > 0 ? qtrue : qfalse;
    }
    default:
        return qfalse;
    }
}

/*
================
RTX_CollectWaterVolumes

Gather AABB volumes from every water-shader surface in the world. The top
water plane defines the volume ceiling; volumes extend downward so pool
floors and walls test as underwater.
================
*/
static void RTX_CollectWaterVolumes(void) {
    rtxNumWaterVolumes = 0;

    if (!tr.world || !tr.world->surfaces) {
        return;
    }

    for (int i = 0; i < tr.world->numsurfaces && rtxNumWaterVolumes < RTX_MAX_WATER_VOLUMES; i++) {
        msurface_t *surf = &tr.world->surfaces[i];

        if (!surf->shader || !(surf->shader->contentFlags & CONTENTS_WATER)) {
            continue;
        }

        rtxWaterVolume_t *vol = &rtxWaterVolumes[rtxNumWaterVolumes];
        if (!RTX_SurfaceBounds(surf, vol->mins, vol->maxs)) {
            continue;
        }

        // A flat water top surface has no depth of its own — extend the
        // volume downward to cover the pool interior.
        if (vol->maxs[2] - vol->mins[2] < 64.0f) {
            vol->mins[2] = vol->maxs[2] - 1024.0f;
        }

        ri.Printf(PRINT_DEVELOPER, "RTX: water volume %d: (%.0f %.0f %.0f) - (%.0f %.0f %.0f)\n",
                  rtxNumWaterVolumes, vol->mins[0], vol->mins[1], vol->mins[2],
                  vol->maxs[0], vol->maxs[1], vol->maxs[2]);
        rtxNumWaterVolumes++;
    }

    if (rtxNumWaterVolumes > 0) {
        ri.Printf(PRINT_ALL, "RTX: Collected %d water volumes for caustics\n", rtxNumWaterVolumes);
    }
}

/*
================
RTX_PointUnderwater
================
*/
static qboolean RTX_PointUnderwater(const vec3_t p) {
    for (int i = 0; i < rtxNumWaterVolumes; i++) {
        const rtxWaterVolume_t *vol = &rtxWaterVolumes[i];
        if (p[0] >= vol->mins[0] - 1.0f && p[0] <= vol->maxs[0] + 1.0f &&
            p[1] >= vol->mins[1] - 1.0f && p[1] <= vol->maxs[1] + 1.0f &&
            p[2] >= vol->mins[2] - 64.0f && p[2] <= vol->maxs[2] - 4.0f) {
            return qtrue;
        }
    }
    return qfalse;
}
static int totalBLASCreated = 0;
static int totalSurfacesProcessed = 0;
static uint64_t loggedUnsupportedTypesMask = 0ULL;
static qboolean loggedUnsupportedOverflow = qfalse;

static qboolean RTX_ComputeBoundsFromVecArray(const vec3_t *points, int count, vec3_t origin, float *radius) {
    if (!points || count <= 0) {
        return qfalse;
    }

    vec3_t sum = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < count; i++) {
        VectorAdd(sum, points[i], sum);
    }
    VectorScale(sum, 1.0f / count, origin);

    float maxDist = 0.0f;
    for (int i = 0; i < count; i++) {
        vec3_t delta;
        VectorSubtract(points[i], origin, delta);
        float dist = VectorLength(delta);
        if (dist > maxDist) {
            maxDist = dist;
        }
    }

    if (radius) {
        *radius = maxDist;
    }
    return qtrue;
}

static qboolean RTX_ComputeBoundsFromGrid(const srfGridMesh_t *grid, vec3_t origin, float *radius) {
    if (!grid || grid->width <= 0 || grid->height <= 0) {
        return qfalse;
    }

    int count = grid->width * grid->height;
    if (count <= 0) {
        return qfalse;
    }

    vec3_t sum = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < count; i++) {
        VectorAdd(sum, grid->verts[i].xyz, sum);
    }
    VectorScale(sum, 1.0f / count, origin);

    float maxDist = 0.0f;
    for (int i = 0; i < count; i++) {
        vec3_t delta;
        VectorSubtract(grid->verts[i].xyz, origin, delta);
        float dist = VectorLength(delta);
        if (dist > maxDist) {
            maxDist = dist;
        }
    }

    if (radius) {
        *radius = maxDist;
    }
    return qtrue;
}

static qboolean RTX_ComputeBoundsFromTriangles(const srfTriangles_t *tri, vec3_t origin, float *radius) {
    if (!tri || tri->numVerts <= 0 || !tri->verts) {
        return qfalse;
    }

    vec3_t sum = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < tri->numVerts; i++) {
        VectorAdd(sum, tri->verts[i].xyz, sum);
    }
    VectorScale(sum, 1.0f / tri->numVerts, origin);

    float maxDist = 0.0f;
    for (int i = 0; i < tri->numVerts; i++) {
        vec3_t delta;
        VectorSubtract(tri->verts[i].xyz, origin, delta);
        float dist = VectorLength(delta);
        if (dist > maxDist) {
            maxDist = dist;
        }
    }

    if (radius) {
        *radius = maxDist;
    }
    return qtrue;
}

static void RTX_CreateEmissiveRenderAndStaticLight(const vec3_t origin, float baseRadius, const vec3_t luminousColor) {
    float intensity = VectorLength(luminousColor);
    if (intensity <= 0.0001f) {
        return;
    }

    vec3_t colorNormalized;
    VectorCopy(luminousColor, colorNormalized);
    VectorScale(colorNormalized, 1.0f / intensity, colorNormalized);

    float lightRadius = MAX(baseRadius * 1.5f, 64.0f);
    lightRadius = Com_Clamp(16.0f, 131072.0f, lightRadius);
    float finalIntensity = intensity * MAX(1.0f, lightRadius / 128.0f);
    if (finalIntensity <= 0.0001f) {
        return;
    }

    renderLight_t *light = R_CreatePointLight(origin, lightRadius, colorNormalized);
    if (light) {
        light->intensity = finalIntensity;
        light->isStatic = qtrue;
        if (tr_lightSystem.numActiveLights < MAX_RENDER_LIGHTS) {
            tr_lightSystem.activeLights[tr_lightSystem.numActiveLights++] = light;
        }
    }

    RT_AddEmissiveStaticLight(origin, colorNormalized, finalIntensity, lightRadius);
}

static void RTX_SelectSkyLuminousColor(const shader_t *shader, vec3_t outColor) {
    vec3_t base = { 1.0f, 0.95f, 0.9f };

    if (shader) {
        vec3_t candidate;
        if (RTX_GetShaderBaseColor(shader, candidate)) {
            VectorCopy(candidate, base);
        }

        if (shader->fogParms.color[0] > 0.0001f ||
            shader->fogParms.color[1] > 0.0001f ||
            shader->fogParms.color[2] > 0.0001f) {
            base[0] = shader->fogParms.color[0];
            base[1] = shader->fogParms.color[1];
            base[2] = shader->fogParms.color[2];
        }
    }

    float magnitude = VectorLength(base);
    if (magnitude <= 0.0001f) {
        VectorSet(base, 1.0f, 0.95f, 0.9f);
        magnitude = VectorLength(base);
    }

    vec3_t normalized = { base[0], base[1], base[2] };
    if (VectorNormalize(normalized) <= 0.0f) {
        VectorSet(normalized, 1.0f, 0.95f, 0.9f);
        VectorNormalize(normalized);
        magnitude = 1.0f;
    }

    float brightness = MAX(magnitude, 0.5f);
    float intensity = 90.0f * brightness;

    VectorScale(normalized, intensity, outColor);
}

static void RTX_TrySpawnEmissiveLightForFace(const srfSurfaceFace_t *face, uint32_t materialIndex) {
    vec3_t luminous;
    if (!RTX_GetMaterialEmission(materialIndex & 0x7FFFFFFFu, luminous, NULL)) {
        return;
    }

    vec3_t origin;
    float radius;
    if (!RTX_ComputeBoundsFromVecArray(face->points, face->numPoints, origin, &radius)) {
        return;
    }

    RTX_CreateEmissiveRenderAndStaticLight(origin, radius, luminous);
}

static void RTX_TrySpawnEmissiveLightForGrid(const srfGridMesh_t *grid, uint32_t materialIndex) {
    vec3_t luminous;
    if (!RTX_GetMaterialEmission(materialIndex & 0x7FFFFFFFu, luminous, NULL)) {
        return;
    }

    vec3_t origin;
    float radius;
    if (!RTX_ComputeBoundsFromGrid(grid, origin, &radius)) {
        return;
    }

    RTX_CreateEmissiveRenderAndStaticLight(origin, radius, luminous);
}

static void RTX_TrySpawnEmissiveLightForTriangles(const srfTriangles_t *tri, uint32_t materialIndex) {
    vec3_t luminous;
    if (!RTX_GetMaterialEmission(materialIndex & 0x7FFFFFFFu, luminous, NULL)) {
        return;
    }

    vec3_t origin;
    float radius;
    if (!RTX_ComputeBoundsFromTriangles(tri, origin, &radius)) {
        return;
    }

    RTX_CreateEmissiveRenderAndStaticLight(origin, radius, luminous);
}

/*
================
RTX_FlushBatch

Create BLAS from accumulated batch
================
*/
static void RTX_FlushBatch(void) {
    if (batchBuilder.numVerts > 0 && batchBuilder.numIndices > 0) {
        ri.Printf(PRINT_ALL, "RTX: Flushing batch with %d verts, %d indices, %d surfaces\n",
            batchBuilder.numVerts, batchBuilder.numIndices, batchBuilder.numSurfaces);

        rtxBLAS_t *blas = RTX_CreateBLAS(
            batchBuilder.vertices,
            batchBuilder.numVerts,
            batchBuilder.indices,
            batchBuilder.numIndices,
            batchBuilder.triangleMaterials,
            batchBuilder.normals,
            (const float (*)[2])batchBuilder.texCoords,
            (const float (*)[4])batchBuilder.colors,
            qfalse  // static geometry
        );

        if (blas) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX_BSP: batch upload attempt -> verts=%d indices=%d tris=%d surfaces=%d dynamic=%d\n",
                      batchBuilder.numVerts,
                      batchBuilder.numIndices,
                      batchBuilder.numIndices / 3,
                      batchBuilder.numSurfaces,
                      blas->isDynamic ? 1 : 0);

            ri.Printf(PRINT_WARNING,
                      "RTX_BSP: calling RTX_BuildBLASGPU (verts=%d tris=%d surfaces=%d)\n",
                      batchBuilder.numVerts,
                      batchBuilder.numIndices / 3,
                      batchBuilder.numSurfaces);

            if (RTX_BuildBLASGPU(blas)) {
                static const float identity[12] = {
                    1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f
                };
                RTX_AddInstance(&rtx.tlas, blas, identity, NULL);

                totalBLASCreated++;
                totalSurfacesProcessed += batchBuilder.numSurfaces;
                ri.Printf(PRINT_ALL, "RTX: Created BLAS %d with %d verts, %d tris, %d surfaces (total surfaces: %d)\n",
                    totalBLASCreated, batchBuilder.numVerts, batchBuilder.numIndices / 3, batchBuilder.numSurfaces, totalSurfacesProcessed);
            } else {
                ri.Printf(PRINT_WARNING, "RTX: Failed to upload BLAS to GPU\n");
                ri.Printf(PRINT_WARNING, "RTX: BLAS build failure stats -> verts=%d indices=%d surfaces=%d\n",
                    batchBuilder.numVerts, batchBuilder.numIndices, batchBuilder.numSurfaces);
                ri.Printf(PRINT_WARNING, "RTX_BSP: RTX_BuildBLASGPU returned false\n");
                RTX_DestroyBLAS(blas);
            }
        } else {
            ri.Printf(PRINT_WARNING, "RTX: Failed to create BLAS from batch\n");
        }

        // Reset batch
        batchBuilder.numVerts = 0;
        batchBuilder.numIndices = 0;
        batchBuilder.numTriangles = 0;
        batchBuilder.numSurfaces = 0;
    }
}

/*
================
RTX_AddSurfaceFace

Add a face surface to the current batch
================
*/
static void RTX_AddSurfaceFace(srfSurfaceFace_t *face, uint32_t materialIndex) {
    if (!face || face->numPoints < 3) {
        return;
    }

    int additionalVerts = face->numPoints;
    int additionalIndices = face->numIndices;
    int additionalTriangles = additionalIndices / 3;

    if (additionalTriangles <= 0) {
        return;
    }
    
    // Check if we need to flush
    if (batchBuilder.numVerts + additionalVerts > MAX_BATCH_VERTS ||
        batchBuilder.numIndices + additionalIndices > MAX_BATCH_INDICES ||
        batchBuilder.numTriangles + additionalTriangles > MAX_BATCH_TRIANGLES) {
        RTX_FlushBatch();
    }
    
    // Add vertices with full attributes
    // Face points are stored as float[VERTEXSIZE] where VERTEXSIZE=8:
    //   [0..2]=xyz, [3..4]=st, [5..6]=lightmap_st, [7]=packed_color
    int baseVertex = batchBuilder.numVerts;
    for (int i = 0; i < face->numPoints; i++) {
        int idx = batchBuilder.numVerts;
        float *v = face->points[i];
        VectorCopy(v, batchBuilder.vertices[idx]);
        batchBuilder.texCoords[idx][0] = v[3];
        batchBuilder.texCoords[idx][1] = v[4];

        // Normals: use per-vertex normals if available, otherwise face plane normal
        if (face->normals) {
            float *n = (float *)face->normals + i * 4; // normals stored as vec4_t
            VectorCopy(n, batchBuilder.normals[idx]);
        } else {
            VectorCopy(face->plane.normal, batchBuilder.normals[idx]);
        }

        // Color: unpack byte RGBA to float
        unsigned char *rgba = (unsigned char *)&v[7];
        batchBuilder.colors[idx][0] = rgba[0] / 255.0f;
        batchBuilder.colors[idx][1] = rgba[1] / 255.0f;
        batchBuilder.colors[idx][2] = rgba[2] / 255.0f;
        batchBuilder.colors[idx][3] = rgba[3] / 255.0f;

        batchBuilder.numVerts++;
    }
    
    // Add indices - they are stored after the points array
    int *indices = (int *)((byte *)face + face->ofsIndices);
    for (int i = 0; i < face->numIndices; i++) {
        batchBuilder.indices[batchBuilder.numIndices++] = baseVertex + indices[i];
    }

    for (int tri = 0; tri < additionalTriangles; tri++) {
        batchBuilder.triangleMaterials[batchBuilder.numTriangles++] = materialIndex;
    }
    
    batchBuilder.numSurfaces++;

    RTX_TrySpawnEmissiveLightForFace(face, materialIndex);
}

/*
================
RTX_AddSurfaceGrid

Add a grid mesh surface to the current batch
================
*/
static void RTX_AddSurfaceGrid(srfGridMesh_t *grid, uint32_t materialIndex) {
    if (!grid || grid->width < 2 || grid->height < 2) {
        return;
    }
    
    // Calculate vertex and triangle count
    int numVerts = grid->width * grid->height;
    int numTris = (grid->width - 1) * (grid->height - 1) * 2;
    int numIndices = numTris * 3;
    
    // Check if we need to flush
    if (batchBuilder.numVerts + numVerts > MAX_BATCH_VERTS ||
        batchBuilder.numIndices + numIndices > MAX_BATCH_INDICES ||
        batchBuilder.numTriangles + numTris > MAX_BATCH_TRIANGLES) {
        RTX_FlushBatch();
    }
    
    // Add vertices with full attributes from drawVert_t
    int baseVertex = batchBuilder.numVerts;
    for (int i = 0; i < numVerts; i++) {
        int idx = batchBuilder.numVerts;
        VectorCopy(grid->verts[i].xyz, batchBuilder.vertices[idx]);
        VectorCopy(grid->verts[i].normal, batchBuilder.normals[idx]);
        batchBuilder.texCoords[idx][0] = grid->verts[i].st[0];
        batchBuilder.texCoords[idx][1] = grid->verts[i].st[1];
        batchBuilder.colors[idx][0] = grid->verts[i].color.rgba[0] / 255.0f;
        batchBuilder.colors[idx][1] = grid->verts[i].color.rgba[1] / 255.0f;
        batchBuilder.colors[idx][2] = grid->verts[i].color.rgba[2] / 255.0f;
        batchBuilder.colors[idx][3] = grid->verts[i].color.rgba[3] / 255.0f;
        batchBuilder.numVerts++;
    }
    
    // Generate indices for the grid
    for (int y = 0; y < grid->height - 1; y++) {
        for (int x = 0; x < grid->width - 1; x++) {
            int v0 = baseVertex + y * grid->width + x;
            int v1 = v0 + 1;
            int v2 = v0 + grid->width;
            int v3 = v2 + 1;
            
            // First triangle
            batchBuilder.indices[batchBuilder.numIndices++] = v0;
            batchBuilder.indices[batchBuilder.numIndices++] = v2;
            batchBuilder.indices[batchBuilder.numIndices++] = v1;
            batchBuilder.triangleMaterials[batchBuilder.numTriangles++] = materialIndex;
            
            // Second triangle
            batchBuilder.indices[batchBuilder.numIndices++] = v1;
            batchBuilder.indices[batchBuilder.numIndices++] = v2;
            batchBuilder.indices[batchBuilder.numIndices++] = v3;
            batchBuilder.triangleMaterials[batchBuilder.numTriangles++] = materialIndex;
        }
    }
    
    batchBuilder.numSurfaces++;

    RTX_TrySpawnEmissiveLightForGrid(grid, materialIndex);
}

/*
================
RTX_AddSurfaceTriangles

Add a triangle soup surface to the current batch
================
*/
static void RTX_AddSurfaceTriangles(srfTriangles_t *tri, uint32_t materialIndex) {
    if (!tri || tri->numVerts < 3 || tri->numIndexes < 3) {
        return;
    }
    
    // Check if we need to flush
    int additionalTriangles = tri->numIndexes / 3;
    
    if (additionalTriangles <= 0) {
        return;
    }
    
    if (batchBuilder.numVerts + tri->numVerts > MAX_BATCH_VERTS ||
        batchBuilder.numIndices + tri->numIndexes > MAX_BATCH_INDICES ||
        batchBuilder.numTriangles + additionalTriangles > MAX_BATCH_TRIANGLES) {
        RTX_FlushBatch();
    }
    
    // Add vertices with full attributes from drawVert_t
    int baseVertex = batchBuilder.numVerts;
    for (int i = 0; i < tri->numVerts; i++) {
        int idx = batchBuilder.numVerts;
        VectorCopy(tri->verts[i].xyz, batchBuilder.vertices[idx]);
        VectorCopy(tri->verts[i].normal, batchBuilder.normals[idx]);
        batchBuilder.texCoords[idx][0] = tri->verts[i].st[0];
        batchBuilder.texCoords[idx][1] = tri->verts[i].st[1];
        batchBuilder.colors[idx][0] = tri->verts[i].color.rgba[0] / 255.0f;
        batchBuilder.colors[idx][1] = tri->verts[i].color.rgba[1] / 255.0f;
        batchBuilder.colors[idx][2] = tri->verts[i].color.rgba[2] / 255.0f;
        batchBuilder.colors[idx][3] = tri->verts[i].color.rgba[3] / 255.0f;
        batchBuilder.numVerts++;
    }
    
    // Add indices
    for (int i = 0; i < tri->numIndexes; i++) {
        batchBuilder.indices[batchBuilder.numIndices++] = baseVertex + tri->indexes[i];
    }

    for (int triIdx = 0; triIdx < additionalTriangles; triIdx++) {
        batchBuilder.triangleMaterials[batchBuilder.numTriangles++] = materialIndex;
    }
    
    batchBuilder.numSurfaces++;

    RTX_TrySpawnEmissiveLightForTriangles(tri, materialIndex);
}

/*
================
RTX_ProcessWorldSurface

Process a world surface and add it to RTX acceleration structures
================
*/
void RTX_ProcessWorldSurface(msurface_t *surf) {
    static int debugCount = 0;

    if (!surf || !surf->data) {
        return;
    }

    surfaceType_t *type = (surfaceType_t *)surf->data;

    // Skip surfaces that shouldn't be in RTX
    if (surf->shader) {
        if (surf->shader->surfaceFlags & SURF_SKY) {
            vec3_t luminous;
            RTX_SelectSkyLuminousColor(surf->shader, luminous);

            vec3_t direction = { 0.0f, -1.0f, 0.0f };
            float weight = 1.0f;

            if (type && *type == SF_FACE) {
                srfSurfaceFace_t *face = (srfSurfaceFace_t *)surf->data;
                vec3_t inward;
                VectorScale(face->plane.normal, -1.0f, inward);
                if (VectorNormalize(inward) > 0.0f && inward[2] < -0.1f) {
                    VectorCopy(inward, direction);
                    weight = MAX(1.0f, (float)(face->numIndices / 3));
                } else {
                    VectorSet(direction, 0.0f, -1.0f, 0.0f);
                    weight = 1.0f;
                }
            }

            RT_AddSkyLightingContribution(direction, luminous, weight);
            return;  // Skip sky surfaces
        }
        if (surf->shader->surfaceFlags & SURF_NODRAW) {
            return;  // Skip nodraw surfaces
        }
        if (surf->shader->contentFlags & CONTENTS_WATER) {
            // Water surfaces are translucent: the raster pass draws them in
            // the blend phase on top of the traced world. Their volumes were
            // collected for caustics before this sweep.
            return;
        }
    }

    // Debug: Log first few surface types
    if (debugCount < 10) {
        ri.Printf(PRINT_ALL, "RTX: Surface %d type: %d\n", debugCount, *type);
        debugCount++;
    }

    uint32_t materialIndex = 0;
    if (surf->shader) {
        materialIndex = (uint32_t)RTX_GetMaterialIndex(surf->shader);
    }

    // Atlas bit 31 marks caustic receivers (surfaces inside water volumes)
    if (rtxNumWaterVolumes > 0) {
        vec3_t mins, maxs, center;
        if (RTX_SurfaceBounds(surf, mins, maxs)) {
            VectorAdd(mins, maxs, center);
            VectorScale(center, 0.5f, center);
            if (RTX_PointUnderwater(center)) {
                materialIndex |= 0x80000000u;
            }
        }
    }

    switch (*type) {
        case SF_FACE:
            RTX_AddSurfaceFace((srfSurfaceFace_t *)surf->data, materialIndex);
            break;

        case SF_GRID:
            RTX_AddSurfaceGrid((srfGridMesh_t *)surf->data, materialIndex);
            break;

        case SF_TRIANGLES:
            RTX_AddSurfaceTriangles((srfTriangles_t *)surf->data, materialIndex);
            break;

        case SF_POLY:
            // Polys are usually dynamic, skip for now
            break;

        default: {
            // Unsupported surface type – log once per unique type to avoid spam
            int surfaceTypeValue = *type;
            if (surfaceTypeValue >= 0 && surfaceTypeValue < 64) {
                uint64_t bit = 1ULL << surfaceTypeValue;
                if ((loggedUnsupportedTypesMask & bit) == 0) {
                    loggedUnsupportedTypesMask |= bit;
                    ri.Printf(PRINT_DEVELOPER,
                        "RTX: Unsupported surface type %d (additional occurrences suppressed)\n",
                        surfaceTypeValue);
                }
            } else if (!loggedUnsupportedOverflow) {
                loggedUnsupportedOverflow = qtrue;
                ri.Printf(PRINT_DEVELOPER,
                    "RTX: Unsupported surface type %d (outside tracked range, suppressing repeats)\n",
                    surfaceTypeValue);
            }
            break;
        }
    }
}

/*
================
RTX_BeginWorldLoad

Initialize RTX world loading
================
*/
void RTX_BeginWorldLoad(void) {
    // Reset batch builder
    Com_Memset(&batchBuilder, 0, sizeof(batchBuilder));
    totalBLASCreated = 0;
    totalSurfacesProcessed = 0;
    loggedUnsupportedTypesMask = 0ULL;
    loggedUnsupportedOverflow = qfalse;
    RT_ResetSkyLighting();
    
    ri.Printf(PRINT_ALL, "RTX: Beginning world geometry loading...\n");
}

/*
================
RTX_EndWorldLoad

Finalize RTX world loading
================
*/
void RTX_EndWorldLoad(void) {
    // Flush any remaining surfaces
    RTX_FlushBatch();
    
    // Build the TLAS
    if (totalBLASCreated > 0) {
        RTX_BuildTLAS(&rtx.tlas);

        // Update debug overlay stats
        ri.Printf(PRINT_ALL, "RTX: Calling RTX_UpdateDebugStats with surfaces=%d, BLAS=%d\n",
            totalSurfacesProcessed, totalBLASCreated);
        RTX_UpdateDebugStats(totalSurfacesProcessed, totalBLASCreated);

        ri.Printf(PRINT_ALL, "RTX: World loading complete - %d BLAS created from %d surfaces\n",
            totalBLASCreated, totalSurfacesProcessed);
        ri.Printf(PRINT_ALL, "RTX: TLAS state after world load - instances=%d, needsRebuild=%d\n",
            rtx.tlas.numInstances, rtx.tlas.needsRebuild);
    } else {
        ri.Printf(PRINT_WARNING, "RTX: No world geometry loaded!\n");
    }

    // Mark completion
    ri.Printf(PRINT_ALL, "RTX: World population complete\n");
}

/*
================
RTX_LoadWorldMap

Main entry point for loading world geometry into RTX
Called from R_LoadWorldMap after BSP is loaded
================
*/
void RTX_LoadWorldMap(void) {
    ri.Printf(PRINT_ALL, "RTX: LoadWorldMap called\n");

    if (!rtx_enable) {
        ri.Printf(PRINT_WARNING, "RTX: rtx_enable cvar is NULL\n");
        return;
    }

    if (!rtx_enable->integer) {
        ri.Printf(PRINT_WARNING, "RTX: Disabled by rtx_enable cvar (value=%d)\n", rtx_enable->integer);
        return;
    }

    if (!tr.world) {
        ri.Printf(PRINT_WARNING, "RTX: No world loaded\n");
        return;
    }

    if (!tr.world->surfaces) {
        ri.Printf(PRINT_WARNING, "RTX: World has no surfaces\n");
        return;
    }

    ri.Printf(PRINT_ALL, "RTX: Beginning world load process\n");
    RTX_BeginWorldLoad();

    // Volume effects and their shader-handle cache do not survive a level
    // transition
    RT_VolumeFX_Clear();

    // Water volumes must exist before surfaces are flagged as underwater
    RTX_CollectWaterVolumes();

    // Process all world surfaces
    int numSurfaces = tr.world->numsurfaces;
    msurface_t *surfaces = tr.world->surfaces;
    
    ri.Printf(PRINT_ALL, "RTX: Processing %d world surfaces...\n", numSurfaces);

    for (int i = 0; i < numSurfaces; i++) {
        RTX_ProcessWorldSurface(&surfaces[i]);

        // Update progress for loading screen
        int populationProgress = (numSurfaces > 0) ? (i * 100) / numSurfaces : 100;

        // Progress update every 1000 surfaces
        if ((i % 1000) == 0 && i > 0) {
            ri.Printf(PRINT_ALL, "RTX: Processed %d/%d surfaces (%d%%), %d surfaces added to batch\n",
                i, numSurfaces, populationProgress, totalSurfacesProcessed);
        }
    }

    ri.Printf(PRINT_ALL, "RTX: Finished processing all surfaces, total processed: %d\n", totalSurfacesProcessed);
    
    RTX_EndWorldLoad();
}



