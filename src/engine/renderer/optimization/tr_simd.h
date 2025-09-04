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

#ifndef TR_SIMD_H
#define TR_SIMD_H

#include "../core/tr_local.h"

/*
================================================================================
Phase 10: SIMD Optimizations

SSE2/AVX optimized math and rendering functions.
================================================================================
*/

// CPU feature detection
typedef struct {
    qboolean    sse;
    qboolean    sse2;
    qboolean    sse3;
    qboolean    ssse3;
    qboolean    sse41;
    qboolean    sse42;
    qboolean    avx;
    qboolean    avx2;
    qboolean    fma3;
} cpuFeatures_t;

extern cpuFeatures_t cpu;

// Initialize SIMD system
void R_InitSIMD(void);

// Feature detection
void R_DetectCPUFeatures(void);

// SIMD math operations
void R_Vec3Add_SSE2(const vec3_t *a, const vec3_t *b, vec3_t *out, int count);
void R_Vec3Scale_SSE2(const vec3_t *in, float scale, vec3_t *out, int count);
void R_Vec3Dot_SSE2(const vec3_t *a, const vec3_t *b, float *out, int count);
void R_Vec3Cross_SSE2(const vec3_t *a, const vec3_t *b, vec3_t *out, int count);
void R_Vec3Normalize_SSE2(vec3_t *vecs, int count);

// Matrix operations
void R_MatrixMultiply_SSE2(const float *a, const float *b, float *out);
void R_TransformPoints_SSE2(const float *matrix, const vec3_t *in, vec3_t *out, int count);

// Triangle operations
void R_CalcTriangleFacing_SSE2(const vec3_t *verts, const int *indices,
                               const vec3_t lightPos, byte *facing, int numTris);
void R_CalcTriangleNormals_SSE2(const vec3_t *verts, const int *indices,
                                vec3_t *normals, int numTris);

// Lighting calculations
void R_CalcDiffuseLighting_SSE2(const vec3_t *positions, const vec3_t *normals,
                                const vec3_t lightPos, const vec3_t lightColor,
                                vec4_t *colors, int count);
void R_CalcSpecularLighting_SSE2(const vec3_t *positions, const vec3_t *normals,
                                 const vec3_t lightPos, const vec3_t viewPos,
                                 float shininess, vec4_t *colors, int count);

// Culling operations
void R_CullSpheres_SSE2(const vec4_t *spheres, const vec4_t *planes, 
                       byte *results, int numSpheres, int numPlanes);
void R_CullBoxes_SSE2(const vec3_t *mins, const vec3_t *maxs,
                     const vec4_t *planes, byte *results, int numBoxes);

// Memory operations
void R_CopyVerts_SSE2(const void *src, void *dst, size_t size);
void R_ClearBuffer_SSE2(void *buffer, int value, size_t size);

// Performance monitoring
void R_SIMDStatistics(void);

// Function pointer selection based on CPU features
extern void (*R_Vec3Add)(const vec3_t *a, const vec3_t *b, vec3_t *out, int count);
extern void (*R_Vec3Scale)(const vec3_t *in, float scale, vec3_t *out, int count);
extern void (*R_MatrixMultiply)(const float *a, const float *b, float *out);

#endif // TR_SIMD_H