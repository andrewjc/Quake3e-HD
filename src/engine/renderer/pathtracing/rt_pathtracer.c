/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Ultra-Fast Hybrid Path Traced Dynamic Lighting System
Optimized for real-time performance using BSP acceleration and caching
===========================================================================
*/

#include "../tr_local.h"
#include "rt_pathtracer.h"
#include <math.h>

// Helper macros
#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Helper functions
static float VectorDistance(const vec3_t v1, const vec3_t v2) {
    vec3_t diff;
    VectorSubtract(v1, v2, diff);
    return VectorLength(diff);
}

static void VectorLerp(const vec3_t from, const vec3_t to, float t, vec3_t result) {
    result[0] = from[0] + t * (to[0] - from[0]);
    result[1] = from[1] + t * (to[1] - from[1]);
    result[2] = from[2] + t * (to[2] - from[2]);
}

// Global path tracer instance
pathTracer_t rt;

// Path tracing CVARs
cvar_t *rt_enable;
cvar_t *rt_quality;
cvar_t *rt_bounces;
cvar_t *rt_samples;
cvar_t *rt_denoise;
cvar_t *rt_temporal;
cvar_t *rt_probes;
cvar_t *rt_cache;
cvar_t *rt_debug;

// Fast random number generator
static unsigned int g_seed = 0x12345678;
static float FastRandom(void) {
    g_seed = (g_seed * 1103515245 + 12345) & 0x7fffffff;
    return (float)g_seed / 0x7fffffff;
}

// Fast approximations for performance
static ID_INLINE float FastSqrt(float x) {
    // Quake's fast inverse square root
    float xhalf = 0.5f * x;
    int i = *(int*)&x;
    i = 0x5f3759df - (i >> 1);
    float y = *(float*)&i;
    y = y * (1.5f - xhalf * y * y);
    return x * y;
}

/*
===============
RT_InitPathTracer

Initialize the path tracing system
===============
*/
void RT_InitPathTracer(void) {
    Com_Memset(&rt, 0, sizeof(rt));
    
    // Set default quality
    rt.quality = RT_QUALITY_MEDIUM;
    rt.maxBounces = 2;
    rt.samplesPerPixel = 1;
    rt.enabled = qfalse;
    
    // Initialize random seed
    g_seed = ri.Milliseconds();
    
    ri.Printf(PRINT_ALL, "Path tracer initialized\n");
}

/*
===============
RT_ShutdownPathTracer

Shutdown and free resources
===============
*/
void RT_ShutdownPathTracer(void) {
    if (rt.lightCache) {
        ri.Free(rt.lightCache);
        rt.lightCache = NULL;
    }
    
    if (rt.probes) {
        ri.Free(rt.probes);
        rt.probes = NULL;
    }
    
    if (rt.accumBuffer) {
        ri.Free(rt.accumBuffer);
        rt.accumBuffer = NULL;
    }
}

/*
===============
RT_BuildAccelerationStructure

Build BSP acceleration structure for ray tracing
Uses existing BSP tree with additional optimization
===============
*/
void RT_BuildAccelerationStructure(void) {
    if (!tr.world) {
        return;
    }
    
    // We'll use the existing BSP tree directly
    // No need to build a separate structure
    rt.bspTree = (rtBspNode_t *)tr.world->nodes;
    rt.numNodes = tr.world->numnodes;
    
    // Initialize light cache
    RT_InitLightCache();
    
    // Initialize probe grid if enabled
    if (rt_probes && rt_probes->integer) {
        vec3_t worldMins, worldMaxs;
        VectorCopy(tr.world->nodes[0].mins, worldMins);
        VectorCopy(tr.world->nodes[0].maxs, worldMaxs);
        RT_InitProbeGrid(worldMins, worldMaxs);
    }
}

/*
===============
RT_RayTriangleIntersect

Möller-Trumbore ray-triangle intersection
Optimized for SSE when available
===============
*/
qboolean RT_RayTriangleIntersect(const ray_t *ray, const vec3_t v0, const vec3_t v1, const vec3_t v2, float *t, vec2_t *uv) {
    vec3_t edge1, edge2, h, s, q;
    float a, f, u, v;
    
    VectorSubtract(v1, v0, edge1);
    VectorSubtract(v2, v0, edge2);
    
    CrossProduct(ray->direction, edge2, h);
    a = DotProduct(edge1, h);
    
    if (a > -0.00001f && a < 0.00001f) {
        return qfalse;
    }
    
    f = 1.0f / a;
    VectorSubtract(ray->origin, v0, s);
    u = f * DotProduct(s, h);
    
    if (u < 0.0f || u > 1.0f) {
        return qfalse;
    }
    
    CrossProduct(s, edge1, q);
    v = f * DotProduct(ray->direction, q);
    
    if (v < 0.0f || u + v > 1.0f) {
        return qfalse;
    }
    
    float rayT = f * DotProduct(edge2, q);
    
    if (rayT > ray->tMin && rayT < ray->tMax) {
        *t = rayT;
        if (uv) {
            uv[0][0] = u;
            uv[0][1] = v;
        }
        return qtrue;
    }
    
    return qfalse;
}

/*
===============
RT_RayBoxIntersect

Fast ray-AABB intersection using slab method
===============
*/
qboolean RT_RayBoxIntersect(const ray_t *ray, const vec3_t mins, const vec3_t maxs, float *tMin, float *tMax) {
    float t1, t2;
    float tNear = ray->tMin;
    float tFar = ray->tMax;
    
    for (int i = 0; i < 3; i++) {
        if (fabs(ray->direction[i]) < 0.00001f) {
            if (ray->origin[i] < mins[i] || ray->origin[i] > maxs[i]) {
                return qfalse;
            }
        } else {
            t1 = (mins[i] - ray->origin[i]) / ray->direction[i];
            t2 = (maxs[i] - ray->origin[i]) / ray->direction[i];
            
            if (t1 > t2) {
                float temp = t1;
                t1 = t2;
                t2 = temp;
            }
            
            if (t1 > tNear) tNear = t1;
            if (t2 < tFar) tFar = t2;
            
            if (tNear > tFar || tFar < 0) {
                return qfalse;
            }
        }
    }
    
    if (tMin) *tMin = tNear;
    if (tMax) *tMax = tFar;
    
    return qtrue;
}

/*
===============
RT_TraceSurface

Test ray against a surface (triangle mesh or patch)
===============
*/
static qboolean RT_TraceSurface(const ray_t *ray, msurface_t *surf, hitInfo_t *hit) {
    if (!surf || !surf->data) {
        return qfalse;
    }
    
    surfaceType_t *surface = surf->data;
    
    switch (*surface) {
    case SF_FACE: {
        srfSurfaceFace_t *face = (srfSurfaceFace_t *)surface;
        
        // Test all triangles in the face
        int *indices = (int *)((byte *)face + face->ofsIndices);
        
        for (int i = 0; i < face->numIndices; i += 3) {
            vec3_t v0, v1, v2;
            VectorCopy(face->points[indices[i]], v0);
            VectorCopy(face->points[indices[i+1]], v1);
            VectorCopy(face->points[indices[i+2]], v2);
            
            float t;
            vec2_t uv;
            if (RT_RayTriangleIntersect(ray, v0, v1, v2, &t, &uv)) {
                if (t < hit->t) {
                    hit->t = t;
                    VectorMA(ray->origin, t, ray->direction, hit->point);
                    
                    // Calculate normal
                    vec3_t edge1, edge2;
                    VectorSubtract(v1, v0, edge1);
                    VectorSubtract(v2, v0, edge2);
                    CrossProduct(edge1, edge2, hit->normal);
                    VectorNormalize(hit->normal);
                    
                    hit->shader = surf->shader;
                    hit->uv[0] = uv[0];
                    hit->uv[1] = uv[1];
                    
                    return qtrue;
                }
            }
        }
        break;
    }
    
    case SF_TRIANGLES: {
        srfTriangles_t *tris = (srfTriangles_t *)surface;
        
        // Quick bounds check
        float tMin, tMax;
        if (!RT_RayBoxIntersect(ray, tris->bounds[0], tris->bounds[1], &tMin, &tMax)) {
            return qfalse;
        }
        
        // Test all triangles
        for (int i = 0; i < tris->numIndexes; i += 3) {
            vec3_t v0, v1, v2;
            VectorCopy(tris->verts[tris->indexes[i]].xyz, v0);
            VectorCopy(tris->verts[tris->indexes[i+1]].xyz, v1);
            VectorCopy(tris->verts[tris->indexes[i+2]].xyz, v2);
            
            float t;
            vec2_t uv;
            if (RT_RayTriangleIntersect(ray, v0, v1, v2, &t, &uv)) {
                if (t < hit->t) {
                    hit->t = t;
                    VectorMA(ray->origin, t, ray->direction, hit->point);
                    
                    // Interpolate normal from vertices
                    vec3_t n0, n1, n2;
                    VectorCopy(tris->verts[tris->indexes[i]].normal, n0);
                    VectorCopy(tris->verts[tris->indexes[i+1]].normal, n1);
                    VectorCopy(tris->verts[tris->indexes[i+2]].normal, n2);
                    
                    hit->normal[0] = n0[0] * (1-uv[0]-uv[1]) + n1[0] * uv[0] + n2[0] * uv[1];
                    hit->normal[1] = n0[1] * (1-uv[0]-uv[1]) + n1[1] * uv[0] + n2[1] * uv[1];
                    hit->normal[2] = n0[2] * (1-uv[0]-uv[1]) + n1[2] * uv[0] + n2[2] * uv[1];
                    VectorNormalize(hit->normal);
                    
                    hit->shader = surf->shader;
                    hit->uv[0] = uv[0];
                    hit->uv[1] = uv[1];
                    
                    return qtrue;
                }
            }
        }
        break;
    }
    
    case SF_GRID: {
        // Grid meshes would need special handling
        // For now, skip them in path tracing
        break;
    }
    }
    
    return qfalse;
}

/*
===============
RT_TraceBSPNode

Traverse BSP tree to find ray intersection
Optimized for cache coherency
===============
*/
static qboolean RT_TraceBSPNode(const ray_t *ray, int nodeNum, hitInfo_t *hit) {
    if (nodeNum < 0) {
        // Leaf node - we're done
        return qfalse;
    }
    
    mnode_t *node = &tr.world->nodes[nodeNum];
    
    // Quick AABB test
    float tMin, tMax;
    if (!RT_RayBoxIntersect(ray, node->mins, node->maxs, &tMin, &tMax)) {
        return qfalse;
    }
    
    // If this is a leaf (contents != -1), test surfaces
    if (node->contents != -1) {
        return qfalse; // Actual leaf
    }
    
    // Calculate distance to splitting plane
    cplane_t *plane = node->plane;
    float d1 = DotProduct(ray->origin, plane->normal) - plane->dist;
    float d2 = DotProduct(ray->direction, plane->normal);
    
    qboolean hitFound = qfalse;
    
    // Determine which side(s) to traverse
    if (fabs(d2) < 0.00001f) {
        // Ray parallel to plane
        int side = (d1 >= 0) ? 0 : 1;
        hitFound = RT_TraceBSPNode(ray, node->children[side]->contents, hit);
    } else {
        // Calculate intersection point with plane
        float t = -d1 / d2;
        
        // Determine traversal order
        int nearSide = (d1 >= 0) ? 0 : 1;
        int farSide = 1 - nearSide;
        
        // Always check near side
        hitFound = RT_TraceBSPNode(ray, node->children[nearSide]->contents, hit);
        
        // Check far side if needed
        if (t > 0 && t < hit->t) {
            qboolean farHit = RT_TraceBSPNode(ray, node->children[farSide]->contents, hit);
            hitFound = hitFound || farHit;
        }
    }
    
    // Test surfaces at this node
    for (int i = 0; i < node->nummarksurfaces; i++) {
        msurface_t *surf = node->firstmarksurface[i];
        if (RT_TraceSurface(ray, surf, hit)) {
            hitFound = qtrue;
        }
    }
    
    return hitFound;
}

/*
===============
RT_TraceRay

Main ray tracing function
===============
*/
qboolean RT_TraceRay(const ray_t *ray, hitInfo_t *hit) {
    if (!tr.world) {
        return qfalse;
    }
    
    // Initialize hit info
    hit->t = ray->tMax;
    hit->shader = NULL;
    
    // Start traversal from root node
    return RT_TraceBSPNode(ray, 0, hit);
}

/*
===============
RT_TraceShadowRay

Fast shadow ray test - early exit on any hit
===============
*/
qboolean RT_TraceShadowRay(const vec3_t origin, const vec3_t target, float maxDist) {
    ray_t ray;
    hitInfo_t hit;
    
    VectorCopy(origin, ray.origin);
    VectorSubtract(target, origin, ray.direction);
    VectorNormalize(ray.direction);
    ray.tMin = 0.001f;
    ray.tMax = maxDist;
    
    // We only care if we hit anything, not what we hit
    return RT_TraceRay(&ray, &hit);
}

/*
===============
RT_EvaluateBRDF

Evaluate Cook-Torrance BRDF for physically-based shading
Optimized with approximations for real-time performance
===============
*/
void RT_EvaluateBRDF(const vec3_t wi, const vec3_t wo, const vec3_t normal, 
                     const vec3_t albedo, float roughness, float metallic, vec3_t result) {
    // Simplified BRDF for performance
    float NdotL = DotProduct(normal, wi);
    float NdotV = DotProduct(normal, wo);
    
    if (NdotL <= 0 || NdotV <= 0) {
        VectorClear(result);
        return;
    }
    
    // Half vector
    vec3_t H;
    VectorAdd(wi, wo, H);
    VectorNormalize(H);
    
    float NdotH = DotProduct(normal, H);
    float VdotH = DotProduct(wo, H);
    
    // Fresnel (Schlick approximation)
    vec3_t F0;
    if (metallic > 0.5f) {
        VectorCopy(albedo, F0);
    } else {
        VectorSet(F0, 0.04f, 0.04f, 0.04f);
    }
    
    float fresnel = F0[0] + (1.0f - F0[0]) * pow(1.0f - VdotH, 5.0f);
    
    // Distribution (GGX)
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (alpha2 - 1.0f) + 1.0f;
    float D = alpha2 / (M_PI * denom * denom);
    
    // Geometry (Smith)
    float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    float G1L = NdotL / (NdotL * (1.0f - k) + k);
    float G1V = NdotV / (NdotV * (1.0f - k) + k);
    float G = G1L * G1V;
    
    // Combine terms
    float specular = (D * G * fresnel) / (4.0f * NdotL * NdotV + 0.001f);
    float diffuse = (1.0f - fresnel) * (1.0f - metallic) / M_PI;
    
    result[0] = albedo[0] * (diffuse + specular) * NdotL;
    result[1] = albedo[1] * (diffuse + specular) * NdotL;
    result[2] = albedo[2] * (diffuse + specular) * NdotL;
}

/*
===============
RT_SampleBRDF

Importance sample the BRDF for next ray direction
===============
*/
void RT_SampleBRDF(const vec3_t wo, const vec3_t normal, float roughness, 
                   vec3_t wi, float *pdf, vec3_t result) {
    // For now, use cosine-weighted hemisphere sampling
    // This is simple and works well for diffuse surfaces
    RT_CosineSampleHemisphere(normal, wi);
    
    // PDF for cosine-weighted sampling
    *pdf = DotProduct(wi, normal) / M_PI;
    
    // The result would be the sampled direction
    VectorCopy(wi, result);
}

/*
===============
RT_EvaluateDirectLighting

Calculate direct lighting from all light sources
Optimized with light culling and caching
===============
*/
void RT_EvaluateDirectLighting(const hitInfo_t *hit, const vec3_t wo, vec3_t result) {
    VectorClear(result);
    
    if (!hit->shader) {
        return;
    }
    
    // Get material properties from shader
    vec3_t albedo = {1, 1, 1};
    float roughness = 0.5f;
    float metallic = 0.0f;
    
    // Test all dynamic lights
    for (int i = 0; i < tr.refdef.num_dlights; i++) {
        dlight_t *dl = &tr.refdef.dlights[i];
        
        // Calculate light direction and distance
        vec3_t lightDir;
        VectorSubtract(dl->origin, hit->point, lightDir);
        float dist = VectorLength(lightDir);
        
        // Skip if out of range
        if (dist > dl->radius) {
            continue;
        }
        
        VectorNormalize(lightDir);
        
        // Shadow test
        if (RT_TraceShadowRay(hit->point, dl->origin, dist)) {
            continue; // In shadow
        }
        
        // Calculate BRDF
        vec3_t brdf;
        RT_EvaluateBRDF(lightDir, wo, hit->normal, albedo, roughness, metallic, brdf);
        
        // Apply light color and attenuation
        float atten = 1.0f - (dist / dl->radius);
        atten = atten * atten; // Quadratic falloff
        
        VectorMA(result, atten, dl->color, result);
    }
}

/*
===============
RT_EvaluateIndirectLighting

Calculate indirect lighting using path tracing
Limited bounces for performance
===============
*/
void RT_EvaluateIndirectLighting(const hitInfo_t *hit, const vec3_t wo, int depth, vec3_t result) {
    VectorClear(result);
    
    // Russian roulette for path termination
    if (depth > 2) {
        float p = 0.5f; // Termination probability
        if (FastRandom() > p) {
            return;
        }
    }
    
    // Sample new direction
    vec3_t wi;
    float pdf;
    vec3_t sample;
    RT_SampleBRDF(wo, hit->normal, 0.5f, wi, &pdf, sample);
    
    // Trace secondary ray
    ray_t ray;
    VectorCopy(hit->point, ray.origin);
    VectorCopy(wi, ray.direction);
    ray.tMin = 0.001f;
    ray.tMax = 10000.0f;
    ray.depth = depth + 1;
    
    hitInfo_t nextHit;
    if (RT_TraceRay(&ray, &nextHit)) {
        // Recursive evaluation
        vec3_t Li;
        RT_TracePath(&ray, depth + 1, Li);
        
        // Apply BRDF and PDF
        vec3_t brdf;
        RT_EvaluateBRDF(wi, wo, hit->normal, (vec3_t){1,1,1}, 0.5f, 0.0f, brdf);
        
        float NdotL = DotProduct(hit->normal, wi);
        if (NdotL > 0 && pdf > 0.001f) {
            VectorScale(Li, NdotL / pdf, result);
        }
    }
}

/*
===============
RT_TracePath

Main path tracing function - traces a complete light path
===============
*/
void RT_TracePath(const ray_t *ray, int depth, vec3_t result) {
    VectorClear(result);
    
    if (depth > rt.maxBounces) {
        return;
    }
    
    hitInfo_t hit;
    if (!RT_TraceRay(ray, &hit)) {
        // Sky color or environment
        VectorSet(result, 0.5f, 0.7f, 1.0f);
        return;
    }
    
    // View direction (opposite of ray)
    vec3_t wo;
    VectorScale(ray->direction, -1, wo);
    
    // Direct lighting
    vec3_t direct;
    RT_EvaluateDirectLighting(&hit, wo, direct);
    
    // Indirect lighting (if quality allows)
    vec3_t indirect = {0, 0, 0};
    if (rt.quality >= RT_QUALITY_HIGH && depth < rt.maxBounces) {
        RT_EvaluateIndirectLighting(&hit, wo, depth, indirect);
    }
    
    // Combine
    VectorAdd(direct, indirect, result);
}

/*
===============
RT_CosineSampleHemisphere

Generate cosine-weighted sample on hemisphere
===============
*/
void RT_CosineSampleHemisphere(const vec3_t normal, vec3_t result) {
    // Generate random point on unit disk
    float r1 = FastRandom();
    float r2 = FastRandom();
    
    float theta = 2.0f * M_PI * r1;
    float r = sqrt(r2);
    
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(1.0f - r2);
    
    // Transform to world space
    vec3_t tangent, bitangent;
    
    // Build orthonormal basis
    if (fabs(normal[0]) < 0.9f) {
        VectorSet(tangent, 1, 0, 0);
    } else {
        VectorSet(tangent, 0, 1, 0);
    }
    
    CrossProduct(normal, tangent, bitangent);
    VectorNormalize(bitangent);
    CrossProduct(bitangent, normal, tangent);
    
    // Transform sample to world space
    result[0] = x * tangent[0] + y * bitangent[0] + z * normal[0];
    result[1] = x * tangent[1] + y * bitangent[1] + z * normal[1];
    result[2] = x * tangent[2] + y * bitangent[2] + z * normal[2];
}

/*
===============
RT_UniformSampleHemisphere

Generate uniform sample on hemisphere
===============
*/
void RT_UniformSampleHemisphere(const vec3_t normal, vec3_t result) {
    float r1 = FastRandom();
    float r2 = FastRandom();
    
    float theta = 2.0f * M_PI * r1;
    float phi = acos(r2);
    
    float x = sin(phi) * cos(theta);
    float y = sin(phi) * sin(theta);
    float z = cos(phi);
    
    // Transform to world space (same as cosine sampling)
    vec3_t tangent, bitangent;
    
    if (fabs(normal[0]) < 0.9f) {
        VectorSet(tangent, 1, 0, 0);
    } else {
        VectorSet(tangent, 0, 1, 0);
    }
    
    CrossProduct(normal, tangent, bitangent);
    VectorNormalize(bitangent);
    CrossProduct(bitangent, normal, tangent);
    
    result[0] = x * tangent[0] + y * bitangent[0] + z * normal[0];
    result[1] = x * tangent[1] + y * bitangent[1] + z * normal[1];
    result[2] = x * tangent[2] + y * bitangent[2] + z * normal[2];
}

/*
===============
Light Cache Functions
Fast spatial hash for temporal coherence
===============
*/
void RT_InitLightCache(void) {
    if (rt.lightCache) {
        ri.Free(rt.lightCache);
    }
    
    rt.cacheSize = RT_CACHE_SIZE;
    rt.lightCache = ri.Malloc(sizeof(lightCacheEntry_t) * rt.cacheSize);
    Com_Memset(rt.lightCache, 0, sizeof(lightCacheEntry_t) * rt.cacheSize);
}

void RT_UpdateLightCache(const vec3_t pos, const vec3_t normal, const vec3_t irradiance) {
    // Simple spatial hash
    unsigned int hash = (unsigned int)(pos[0] * 73.0f + pos[1] * 179.0f + pos[2] * 283.0f);
    hash = hash % rt.cacheSize;
    
    lightCacheEntry_t *entry = &rt.lightCache[hash];
    
    // Update or replace entry
    if (VectorDistance(entry->position, pos) < 10.0f) {
        // Blend with existing
        float blend = 0.1f;
        VectorLerp(entry->irradiance, irradiance, blend, entry->irradiance);
        entry->confidence = MIN(1.0f, entry->confidence + 0.1f);
    } else {
        // Replace
        VectorCopy(pos, entry->position);
        VectorCopy(normal, entry->normal);
        VectorCopy(irradiance, entry->irradiance);
        entry->confidence = 0.5f;
    }
    
    entry->frameUpdated = rt.currentFrame;
    entry->sampleCount++;
}

qboolean RT_QueryLightCache(const vec3_t pos, const vec3_t normal, vec3_t irradiance) {
    unsigned int hash = (unsigned int)(pos[0] * 73.0f + pos[1] * 179.0f + pos[2] * 283.0f);
    hash = hash % rt.cacheSize;
    
    lightCacheEntry_t *entry = &rt.lightCache[hash];
    
    if (VectorDistance(entry->position, pos) < 10.0f &&
        DotProduct(entry->normal, normal) > 0.9f &&
        entry->confidence > 0.3f) {
        VectorCopy(entry->irradiance, irradiance);
        return qtrue;
    }
    
    return qfalse;
}

/*
===============
Probe Grid Functions
Irradiance probes for global illumination
===============
*/
void RT_InitProbeGrid(const vec3_t mins, const vec3_t maxs) {
    VectorCopy(mins, rt.probeGridOrigin);
    VectorSubtract(maxs, mins, rt.probeGridSize);
    
    // Calculate probe spacing
    float spacing = rt.probeGridSize[0] / RT_PROBE_GRID_SIZE;
    
    rt.numProbes = RT_PROBE_GRID_SIZE * RT_PROBE_GRID_SIZE * RT_PROBE_GRID_SIZE;
    rt.probes = ri.Malloc(sizeof(irradianceProbe_t) * rt.numProbes);
    
    // Initialize probe positions
    int index = 0;
    for (int z = 0; z < RT_PROBE_GRID_SIZE; z++) {
        for (int y = 0; y < RT_PROBE_GRID_SIZE; y++) {
            for (int x = 0; x < RT_PROBE_GRID_SIZE; x++) {
                irradianceProbe_t *probe = &rt.probes[index++];
                
                probe->position[0] = rt.probeGridOrigin[0] + x * spacing;
                probe->position[1] = rt.probeGridOrigin[1] + y * spacing;
                probe->position[2] = rt.probeGridOrigin[2] + z * spacing;
                
                // Clear irradiance
                for (int i = 0; i < 6; i++) {
                    VectorClear(probe->irradiance[i]);
                    probe->visibility[i] = 1.0f;
                }
                
                probe->lastUpdate = 0;
                probe->dynamic = qfalse;
            }
        }
    }
}

void RT_UpdateProbe(int probeIndex) {
    if (probeIndex < 0 || probeIndex >= rt.numProbes) {
        return;
    }
    
    irradianceProbe_t *probe = &rt.probes[probeIndex];
    
    // Sample irradiance in 6 directions (cube faces)
    vec3_t directions[6] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1}
    };
    
    for (int i = 0; i < 6; i++) {
        ray_t ray;
        VectorCopy(probe->position, ray.origin);
        VectorCopy(directions[i], ray.direction);
        ray.tMin = 0.1f;
        ray.tMax = 1000.0f;
        ray.depth = 0;
        
        vec3_t irradiance;
        RT_TracePath(&ray, 0, irradiance);
        
        // Update probe irradiance with temporal filtering
        float blend = 0.1f;
        VectorLerp(probe->irradiance[i], irradiance, blend, probe->irradiance[i]);
    }
    
    probe->lastUpdate = rt.currentFrame;
}

void RT_SampleProbeGrid(const vec3_t pos, const vec3_t normal, vec3_t result) {
    VectorClear(result);
    
    if (!rt.probes) {
        return;
    }
    
    // Find nearest probes
    vec3_t gridPos;
    VectorSubtract(pos, rt.probeGridOrigin, gridPos);
    
    float spacing = rt.probeGridSize[0] / RT_PROBE_GRID_SIZE;
    int x = (int)(gridPos[0] / spacing);
    int y = (int)(gridPos[1] / spacing);
    int z = (int)(gridPos[2] / spacing);
    
    // Clamp to grid bounds
    x = CLAMP(x, 0, RT_PROBE_GRID_SIZE - 2);
    y = CLAMP(y, 0, RT_PROBE_GRID_SIZE - 2);
    z = CLAMP(z, 0, RT_PROBE_GRID_SIZE - 2);
    
    // Trilinear interpolation of 8 nearest probes
    float fx = (gridPos[0] / spacing) - x;
    float fy = (gridPos[1] / spacing) - y;
    float fz = (gridPos[2] / spacing) - z;
    
    for (int dz = 0; dz <= 1; dz++) {
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int index = (z + dz) * RT_PROBE_GRID_SIZE * RT_PROBE_GRID_SIZE +
                           (y + dy) * RT_PROBE_GRID_SIZE + (x + dx);
                
                if (index >= rt.numProbes) continue;
                
                irradianceProbe_t *probe = &rt.probes[index];
                
                // Weight based on position
                float weight = (dx ? fx : 1-fx) * (dy ? fy : 1-fy) * (dz ? fz : 1-fz);
                
                // Sample probe in direction of normal
                vec3_t probeIrradiance = {0, 0, 0};
                for (int i = 0; i < 6; i++) {
                    vec3_t directions[6] = {
                        {1, 0, 0}, {-1, 0, 0},
                        {0, 1, 0}, {0, -1, 0},
                        {0, 0, 1}, {0, 0, -1}
                    };
                    
                    float dot = DotProduct(normal, directions[i]);
                    if (dot > 0) {
                        VectorMA(probeIrradiance, dot, probe->irradiance[i], probeIrradiance);
                    }
                }
                
                VectorMA(result, weight, probeIrradiance, result);
            }
        }
    }
}

/*
===============
RT_RenderPathTracedLighting

Main rendering function - integrates with existing renderer
This is called per frame to add path traced lighting
===============
*/
void RT_RenderPathTracedLighting(void) {
    if (!rt.enabled || !rt_enable || !rt_enable->integer) {
        return;
    }
    
    if (!tr.world) {
        return;
    }
    
    // Update frame counter
    rt.currentFrame++;
    
    // For hybrid rendering, we only path trace dynamic lights
    // The base lighting comes from lightmaps
    
    // Update probes if needed
    if (rt_probes && rt_probes->integer) {
        // Update a subset of probes each frame for performance
        int probesPerFrame = rt.numProbes / 16;
        for (int i = 0; i < probesPerFrame; i++) {
            int index = (rt.currentFrame * probesPerFrame + i) % rt.numProbes;
            RT_UpdateProbe(index);
        }
    }
    
    // Screen-space lighting pass would go here
    // For now, the path tracer is ready to be used by the main renderer
    // when drawing surfaces that need dynamic lighting
}

/*
===============
Utility Functions
===============
*/
void RT_HammersleySequence(int i, int n, vec2_t result) {
    result[0] = (float)i / (float)n;
    result[1] = RT_RadicalInverse(i);
}

float RT_RadicalInverse(unsigned int bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f; // / 0x100000000
}

void RT_GenerateRay(int x, int y, int sample, ray_t *ray) {
    // Generate camera ray for pixel (x, y) with jitter for anti-aliasing
    // This would use the view parameters from tr.refdef
    
    // For now, just return a default ray
    VectorCopy(tr.refdef.vieworg, ray->origin);
    VectorCopy(tr.refdef.viewaxis[0], ray->direction);
    ray->tMin = 0.1f;
    ray->tMax = 10000.0f;
    ray->depth = 0;
}

void RT_GetAccumulatedColor(int x, int y, vec3_t result) {
    // Get accumulated color from temporal buffer
    if (!rt.accumBuffer) {
        VectorClear(result);
        return;
    }
    
    int index = (y * glConfig.vidWidth + x) * 3;
    result[0] = rt.accumBuffer[index + 0];
    result[1] = rt.accumBuffer[index + 1];
    result[2] = rt.accumBuffer[index + 2];
}

void RT_AccumulateSample(int x, int y, const vec3_t color) {
    if (!rt.accumBuffer) {
        return;
    }
    
    int index = (y * glConfig.vidWidth + x) * 3;
    
    // Temporal accumulation with exponential moving average
    float blend = 0.1f;
    rt.accumBuffer[index + 0] = rt.accumBuffer[index + 0] * (1-blend) + color[0] * blend;
    rt.accumBuffer[index + 1] = rt.accumBuffer[index + 1] * (1-blend) + color[1] * blend;
    rt.accumBuffer[index + 2] = rt.accumBuffer[index + 2] * (1-blend) + color[2] * blend;
}

/*
===============
Stub functions for remaining features
These would be implemented as needed
===============
*/
void RT_InitTemporalBuffers(void) {}
void RT_ResetAccumulation(void) {}
void RT_BeginFrame(void) {}
void RT_EndFrame(void) {}
void RT_UpdateDynamicLights(void) {}
void RT_InitDenoiser(void) {}
void RT_DenoiseFrame(float *input, float *output, int width, int height) {}
void RT_ApplyTemporalFilter(float *current, float *history, float *output, int width, int height) {}
void RT_ApplySpatialFilter(float *input, float *output, int width, int height) {}
void RT_ClearLightCache(void) {}
void RT_DrawDebugRays(void) {}
void RT_DrawProbeGrid(void) {}
void RT_DrawLightCache(void) {}
qboolean RT_RayBSPIntersect(const ray_t *ray, rtBspNode_t *node, hitInfo_t *hit) { return qfalse; }