/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

This file is part of Quake3e-HD.

Quake3e-HD is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
===========================================================================
*/
// tr_common_utils.h - Common utility macros and inline functions

#ifndef __TR_COMMON_UTILS_H
#define __TR_COMMON_UTILS_H

#include "tr_local.h"
#include <math.h>

// Math helper macros
#ifndef Min
#define Min(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifndef Max
#define Max(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(v,min,max) ((v) < (min) ? (min) : ((v) > (max) ? (max) : (v)))
#endif

// Vector macros for 2D vectors
#ifndef VectorSet2
#define VectorSet2(v,x,y) ((v)[0]=(x),(v)[1]=(y))
#endif

#ifndef VectorCopy2
#define VectorCopy2(a,b) ((b)[0]=(a)[0],(b)[1]=(a)[1])
#endif

#ifndef VectorClear2
#define VectorClear2(a) ((a)[0]=0,(a)[1]=0)
#endif

// Vector macros for 4D vectors
#ifndef VectorSet4
#define VectorSet4(v,x,y,z,w) ((v)[0]=(x),(v)[1]=(y),(v)[2]=(z),(v)[3]=(w))
#endif

#ifndef VectorCopy4
#define VectorCopy4(a,b) ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2],(b)[3]=(a)[3])
#endif

#ifndef Vector2Length
static ID_INLINE float Vector2Length( const vec_t *v ) {
    return sqrtf( v[0] * v[0] + v[1] * v[1] );
}
#endif

// Matrix operations
static ID_INLINE void MatrixIdentity( float *m ) {
    m[0] = 1;  m[4] = 0;  m[8] = 0;  m[12] = 0;
    m[1] = 0;  m[5] = 1;  m[9] = 0;  m[13] = 0;
    m[2] = 0;  m[6] = 0;  m[10] = 1; m[14] = 0;
    m[3] = 0;  m[7] = 0;  m[11] = 0; m[15] = 1;
}

static ID_INLINE void MatrixCopy( const float *in, float *out ) {
    int i;
    for ( i = 0; i < 16; i++ ) {
        out[i] = in[i];
    }
}

static ID_INLINE void MatrixMultiply4x4( const float *a, const float *b, float *out ) {
    int i, j, k;
    float temp[16];
    
    for ( i = 0; i < 4; i++ ) {
        for ( j = 0; j < 4; j++ ) {
            temp[i*4+j] = 0;
            for ( k = 0; k < 4; k++ ) {
                temp[i*4+j] += a[i*4+k] * b[k*4+j];
            }
        }
    }
    
    MatrixCopy( temp, out );
}

static ID_INLINE void MatrixScale( float *m, float scale ) {
    m[0] *= scale;
    m[5] *= scale;
    m[10] *= scale;
}

static ID_INLINE void MatrixSetTranslation( float *m, const vec3_t trans ) {
    m[12] = trans[0];
    m[13] = trans[1];
    m[14] = trans[2];
}

static ID_INLINE void MatrixTransformPoint( const float *m, const vec3_t in, vec3_t out ) {
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
}

static ID_INLINE void MatrixTransformNormal( const float *m, const vec3_t in, vec3_t out ) {
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2];
}

// Quaternion operations
static ID_INLINE void QuatIdentity( vec4_t q ) {
    q[0] = 0;
    q[1] = 0;
    q[2] = 0;
    q[3] = 1;
}

static ID_INLINE void QuatFromAngles( const vec3_t angles, vec4_t q ) {
    float pitch = angles[0] * 0.5f * M_PI / 180.0f;
    float yaw = angles[1] * 0.5f * M_PI / 180.0f;
    float roll = angles[2] * 0.5f * M_PI / 180.0f;
    
    float cy = cosf(yaw);
    float sy = sinf(yaw);
    float cp = cosf(pitch);
    float sp = sinf(pitch);
    float cr = cosf(roll);
    float sr = sinf(roll);
    
    q[0] = sr * cp * cy - cr * sp * sy;
    q[1] = cr * sp * cy + sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy;
    q[3] = cr * cp * cy + sr * sp * sy;
}

static ID_INLINE void MatrixFromQuat( const vec4_t q, float *m ) {
    float x2 = q[0] + q[0];
    float y2 = q[1] + q[1];
    float z2 = q[2] + q[2];
    float xx = q[0] * x2;
    float xy = q[0] * y2;
    float xz = q[0] * z2;
    float yy = q[1] * y2;
    float yz = q[1] * z2;
    float zz = q[2] * z2;
    float wx = q[3] * x2;
    float wy = q[3] * y2;
    float wz = q[3] * z2;
    
    m[0] = 1.0f - (yy + zz);
    m[1] = xy + wz;
    m[2] = xz - wy;
    m[3] = 0.0f;
    
    m[4] = xy - wz;
    m[5] = 1.0f - (xx + zz);
    m[6] = yz + wx;
    m[7] = 0.0f;
    
    m[8] = xz + wy;
    m[9] = yz - wx;
    m[10] = 1.0f - (xx + yy);
    m[11] = 0.0f;
    
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
}

// Matrix inverse (simplified for 4x4 transformation matrices)
static ID_INLINE qboolean MatrixInverse( const float *m, float *out ) {
    float inv[16];
    float det;
    int i;
    
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + 
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - 
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + 
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - 
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    
    if ( det == 0 ) {
        return qfalse;
    }
    
    det = 1.0f / det;
    
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - 
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + 
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - 
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + 
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + 
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - 
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + 
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - 
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - 
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + 
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - 
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + 
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    
    for ( i = 0; i < 16; i++ ) {
        out[i] = inv[i] * det;
    }
    
    return qtrue;
}

// Projection helpers
static ID_INLINE qboolean R_ProjectPointToScreen( const vec3_t world, float *x, float *y ) {
    vec4_t eye;
    vec4_t clip;
    vec4_t normalized;
    
    // Transform by modelview matrix
    eye[0] = tr.or.modelMatrix[0] * world[0] + tr.or.modelMatrix[4] * world[1] + 
             tr.or.modelMatrix[8] * world[2] + tr.or.modelMatrix[12];
    eye[1] = tr.or.modelMatrix[1] * world[0] + tr.or.modelMatrix[5] * world[1] + 
             tr.or.modelMatrix[9] * world[2] + tr.or.modelMatrix[13];
    eye[2] = tr.or.modelMatrix[2] * world[0] + tr.or.modelMatrix[6] * world[1] + 
             tr.or.modelMatrix[10] * world[2] + tr.or.modelMatrix[14];
    eye[3] = tr.or.modelMatrix[3] * world[0] + tr.or.modelMatrix[7] * world[1] + 
             tr.or.modelMatrix[11] * world[2] + tr.or.modelMatrix[15];
    
    // Transform by projection matrix
    clip[0] = tr.viewParms.projectionMatrix[0] * eye[0] + tr.viewParms.projectionMatrix[4] * eye[1] + 
              tr.viewParms.projectionMatrix[8] * eye[2] + tr.viewParms.projectionMatrix[12] * eye[3];
    clip[1] = tr.viewParms.projectionMatrix[1] * eye[0] + tr.viewParms.projectionMatrix[5] * eye[1] + 
              tr.viewParms.projectionMatrix[9] * eye[2] + tr.viewParms.projectionMatrix[13] * eye[3];
    clip[2] = tr.viewParms.projectionMatrix[2] * eye[0] + tr.viewParms.projectionMatrix[6] * eye[1] + 
              tr.viewParms.projectionMatrix[10] * eye[2] + tr.viewParms.projectionMatrix[14] * eye[3];
    clip[3] = tr.viewParms.projectionMatrix[3] * eye[0] + tr.viewParms.projectionMatrix[7] * eye[1] + 
              tr.viewParms.projectionMatrix[11] * eye[2] + tr.viewParms.projectionMatrix[15] * eye[3];
    
    if ( clip[3] == 0.0f ) {
        return qfalse;
    }
    
    // Normalize
    normalized[0] = clip[0] / clip[3];
    normalized[1] = clip[1] / clip[3];
    
    // Convert to window coordinates
    *x = ( normalized[0] * 0.5f + 0.5f ) * glConfig.vidWidth;
    *y = ( normalized[1] * 0.5f + 0.5f ) * glConfig.vidHeight;
    
    return qtrue;
}

#endif // __TR_COMMON_UTILS_H