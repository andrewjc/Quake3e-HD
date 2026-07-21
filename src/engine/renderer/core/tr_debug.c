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
// tr_debug.c - Debug rendering functions

#include "tr_local.h"
#include "profiling/tr_debug.h"

// Types are defined in tr_debug.h

#define MAX_DEBUG_LINES 4096
#define MAX_DEBUG_BOXES 1024
#define MAX_DEBUG_TEXTS 256

static debugLine_t debugLines[MAX_DEBUG_LINES];
static int numDebugLines;

static debugBox_t debugBoxes[MAX_DEBUG_BOXES];
static int numDebugBoxes;

static debugText_t debugTexts[MAX_DEBUG_TEXTS];
static int numDebugTexts;

/*
================
RB_AddDebugLine

Add a debug line to be rendered
================
*/
void RB_AddDebugLine( const vec3_t start, const vec3_t end, const vec4_t color ) {
    if ( numDebugLines >= MAX_DEBUG_LINES ) {
        return;
    }
    
    debugLine_t *line = &debugLines[numDebugLines++];
    VectorCopy( start, line->start );
    VectorCopy( end, line->end );
    if ( color ) {
        Vector4Copy( color, line->color );
    } else {
        line->color[0] = 1.0f;
        line->color[1] = 1.0f;
        line->color[2] = 1.0f;
        line->color[3] = 1.0f;
    }
}

/*
================
RB_AddDebugBox

Add a debug box to be rendered
================
*/
void RB_AddDebugBox( const vec3_t mins, const vec3_t maxs, const vec4_t color ) {
    if ( numDebugBoxes >= MAX_DEBUG_BOXES ) {
        return;
    }
    
    debugBox_t *box = &debugBoxes[numDebugBoxes++];
    VectorCopy( mins, box->mins );
    VectorCopy( maxs, box->maxs );
    if ( color ) {
        Vector4Copy( color, box->color );
    } else {
        box->color[0] = 1.0f;
        box->color[1] = 0.0f;
        box->color[2] = 0.0f;
        box->color[3] = 1.0f;
    }
}

/*
================
RB_AddDebugText

Add debug text to be rendered
================
*/
void RB_AddDebugText( const vec3_t origin, float scale, const vec4_t color, const char *text ) {
    if ( numDebugTexts >= MAX_DEBUG_TEXTS ) {
        return;
    }
    
    debugText_t *dt = &debugTexts[numDebugTexts++];
    VectorCopy( origin, dt->position );
    Q_strncpyz( dt->text, text, sizeof(dt->text) );
    dt->scale = scale > 0 ? scale : 1.0f;
    dt->screenSpace = qfalse;
    dt->lifeTime = 0;
    if ( color ) {
        Vector4Copy( color, dt->color );
    } else {
        dt->color[0] = 1.0f;
        dt->color[1] = 1.0f;
        dt->color[2] = 1.0f;
        dt->color[3] = 1.0f;
    }
}

/*
================
RB_ClearDebugPrimitives

Clear all debug primitives
================
*/
void RB_ClearDebugPrimitives( void ) {
    numDebugLines = 0;
    numDebugBoxes = 0;
    numDebugTexts = 0;
}

/*
================
RB_RenderDebugPrimitives

Render all debug primitives
================
*/
void RB_RenderDebugPrimitives( void ) {
    // This would be implemented with actual Vulkan rendering
    // For now, just clear the primitives after "rendering"
    RB_ClearDebugPrimitives();
}

/*
================
R_DrawString

Draw a string at screen coordinates
================
*/
void R_DrawString( int x, int y, const char *string, const vec4_t color ) {
    // Stub implementation - would use font rendering system
    (void)x;
    (void)y;
    (void)string;
    (void)color;
}

/*
================
R_DrawRect

Draw a filled rectangle
================
*/
void R_DrawRect( float x, float y, float width, float height, const vec4_t color ) {
    // Stub implementation - would use 2D rendering system
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
}

/*
================
R_DrawLine

Draw a 2D line
================
*/
void R_DrawLine( float x1, float y1, float x2, float y2, const vec4_t color ) {
    // Stub implementation - would use 2D rendering system
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)color;
}

/*
================
R_DrawFrameTimeGraph

Draw frame time graph overlay
================
*/
void R_DrawFrameTimeGraph( int x, int y, int width, int height ) {
    // Stub implementation
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

/*
================
R_DrawTimerBars

Draw profiling timer bars
================
*/
void R_DrawTimerBars( int x, int y ) {
    // Stub implementation
    (void)x;
    (void)y;
}

/*
================
R_DrawCounterValues

Draw performance counter values
================
*/
void R_DrawCounterValues( int x, int y ) {
    // Stub implementation
    (void)x;
    (void)y;
}