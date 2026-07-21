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
// tr_debug.h - Debug Visualization System

#ifndef __TR_DEBUG_H
#define __TR_DEBUG_H

#include "../tr_local.h"

#define DEBUG_MAX_LINES         4096
#define DEBUG_MAX_POINTS        2048
#define DEBUG_MAX_SPHERES       512
#define DEBUG_MAX_BOXES         512
#define DEBUG_MAX_TEXT          256

typedef enum {
    DEBUG_VIS_NONE = 0,
    DEBUG_VIS_WIREFRAME,
    DEBUG_VIS_NORMALS,
    DEBUG_VIS_TANGENTS,
    DEBUG_VIS_UV,
    DEBUG_VIS_LIGHTMAP,
    DEBUG_VIS_VERTEX_COLOR,
    DEBUG_VIS_DEPTH,
    DEBUG_VIS_STENCIL,
    DEBUG_VIS_OVERDRAW,
    DEBUG_VIS_CASCADES,
    DEBUG_VIS_CLUSTERS,
    DEBUG_VIS_CULLING,
    DEBUG_VIS_BOUNDING_BOXES,
    DEBUG_VIS_COLLISION,
    DEBUG_VIS_PHYSICS,
    DEBUG_VIS_PORTALS,
    DEBUG_VIS_OCCLUDERS,
    DEBUG_VIS_LIGHTS,
    DEBUG_VIS_SHADOWS,
    DEBUG_VIS_REFLECTIONS,
    DEBUG_VIS_GI_PROBES,
    DEBUG_VIS_PERFORMANCE
} debugVisualization_t;

typedef struct debugLine_s {
    vec3_t                 start;
    vec3_t                 end;
    vec4_t                 color;
    float                  width;
    uint32_t               lifeTime;
} debugLine_t;

typedef struct debugPoint_s {
    vec3_t                 position;
    vec4_t                 color;
    float                  size;
    uint32_t               lifeTime;
} debugPoint_t;

typedef struct debugSphere_s {
    vec3_t                 center;
    float                  radius;
    vec4_t                 color;
    qboolean               wireframe;
    uint32_t               lifeTime;
} debugSphere_t;

typedef struct debugBox_s {
    vec3_t                 mins;
    vec3_t                 maxs;
    vec4_t                 color;
    qboolean               wireframe;
    uint32_t               lifeTime;
} debugBox_t;

typedef struct debugText_s {
    vec3_t                 position;
    char                   text[128];
    vec4_t                 color;
    float                  scale;
    qboolean               screenSpace;
    uint32_t               lifeTime;
} debugText_t;

typedef struct debugState_s {
    qboolean               enabled;
    debugVisualization_t   activeMode;
    
    // Debug primitives
    debugLine_t            lines[DEBUG_MAX_LINES];
    uint32_t               lineCount;
    debugPoint_t           points[DEBUG_MAX_POINTS];
    uint32_t               pointCount;
    debugSphere_t          spheres[DEBUG_MAX_SPHERES];
    uint32_t               sphereCount;
    debugBox_t             boxes[DEBUG_MAX_BOXES];
    uint32_t               boxCount;
    debugText_t            texts[DEBUG_MAX_TEXT];
    uint32_t               textCount;
    
    // Visualization options
    qboolean               showWireframe;
    qboolean               showNormals;
    qboolean               showBounds;
    qboolean               showCulling;
    qboolean               showOverdraw;
    qboolean               showCascades;
    qboolean               showPortals;
    qboolean               showLights;
    qboolean               showStats;
    
    // Overdraw visualization
    uint32_t               overdrawScale;
    vec4_t                 overdrawColors[8];
    
    // Performance visualization
    qboolean               showGPUTimers;
    qboolean               showDrawCalls;
    qboolean               showTriangleCount;
    qboolean               showTextureMemory;
    
    // Debug render targets
    qboolean               captureGBuffer;
    qboolean               captureShadowMaps;
    qboolean               captureLightmaps;
} debugState_t;

extern debugState_t debugRenderState;

// Initialization
qboolean R_InitDebugVisualization( void );
void R_ShutdownDebugVisualization( void );

// Mode control
void R_SetDebugVisualization( debugVisualization_t mode );
void R_ToggleDebugOption( const char *option );

// Debug primitives
void R_DebugLine( const vec3_t start, const vec3_t end, const vec4_t color, float width, uint32_t lifeTime );
void R_DebugPoint( const vec3_t position, const vec4_t color, float size, uint32_t lifeTime );
void R_DebugSphere( const vec3_t center, float radius, const vec4_t color, qboolean wireframe, uint32_t lifeTime );
void R_DebugBox( const vec3_t mins, const vec3_t maxs, const vec4_t color, qboolean wireframe, uint32_t lifeTime );
void R_DebugText( const vec3_t position, const char *text, const vec4_t color, float scale, qboolean screenSpace, uint32_t lifeTime );
void R_DebugAxis( const vec3_t origin, const vec3_t axis[3], float scale );

// Debug primitive functions
void RB_AddDebugLine( const vec3_t start, const vec3_t end, const vec4_t color );
void RB_AddDebugBox( const vec3_t mins, const vec3_t maxs, const vec4_t color );
void RB_AddDebugText( const vec3_t origin, float scale, const vec4_t color, const char *text );
void RB_ClearDebugPrimitives( void );
void RB_RenderDebugPrimitives( void );

// 2D Drawing functions
void R_DrawString( int x, int y, const char *string, const vec4_t color );
void R_DrawRect( float x, float y, float width, float height, const vec4_t color );
void R_DrawLine( float x1, float y1, float x2, float y2, const vec4_t color );
void R_DrawFrameTimeGraph( int x, int y, int width, int height );
void R_DrawTimerBars( int x, int y );
void R_DrawCounterValues( int x, int y );

// Visualization rendering
void R_RenderDebugPrimitives( void );
void R_RenderWireframeMode( void );
void R_RenderNormals( void );
void R_RenderOverdrawVisualization( void );
void R_RenderCascadeVisualization( void );
void R_RenderPerformanceOverlay( void );

// Buffer capture
void R_CaptureGBuffer( const char *prefix );
void R_CaptureShadowMaps( const char *prefix );
void R_CaptureRenderTarget( const char *name );

// Helpers
void R_ClearDebugPrimitives( void );
void R_UpdateDebugPrimitives( uint32_t deltaTime );
void R_GetDebugColor( uint32_t index, vec4_t color );
const char* R_GetDebugModeName( debugVisualization_t mode );

#endif // __TR_DEBUG_H