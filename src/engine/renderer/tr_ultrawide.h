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

#ifndef TR_ULTRAWIDE_H
#define TR_ULTRAWIDE_H

#include "tr_local.h"

/*
================================================================================
Ultra-Widescreen Support with Perspective Correction

This system implements multi-region rendering to correct perspective distortion
on ultra-wide displays (21:9, 32:9, etc.) by rendering the scene in multiple
frustum segments and combining them.
================================================================================
*/

// Common ultra-wide aspect ratios
#define ASPECT_16_9     (16.0f / 9.0f)     // Standard widescreen
#define ASPECT_21_9     (21.0f / 9.0f)     // Ultra-wide
#define ASPECT_32_9     (32.0f / 9.0f)     // Super ultra-wide
#define ASPECT_32_10    (32.0f / 10.0f)    // Dual 16:10
#define ASPECT_48_9     (48.0f / 9.0f)     // Triple monitor

// Rendering modes for ultra-wide
typedef enum {
    UW_RENDER_SINGLE,           // Standard single frustum (causes distortion)
    UW_RENDER_TRIPLE,           // Three regions (left, center, right)
    UW_RENDER_QUINTUPLE,        // Five regions for extreme wide
    UW_RENDER_CYLINDRICAL,      // Cylindrical projection
    UW_RENDER_PANINI            // Panini projection
} ultrawideRenderMode_t;

// Region definition for multi-region rendering
typedef struct uwRegion_s {
    float   xStart;             // Normalized screen X start (0-1)
    float   xEnd;               // Normalized screen X end (0-1)
    float   yawOffset;          // Yaw rotation offset in degrees
    float   fovScale;           // FOV scaling factor
    vec4_t  viewport;           // Viewport (x, y, width, height)
    float   projMatrix[16];     // Projection matrix for this region
    float   viewMatrix[16];     // View matrix for this region
} uwRegion_t;

// Ultra-wide rendering state
typedef struct uwState_s {
    qboolean                enabled;
    ultrawideRenderMode_t   mode;
    float                   aspectRatio;
    float                   baseFOV;
    int                     numRegions;
    uwRegion_t              regions[5];
    
    // Panini projection parameters
    float                   paniniDistance;    // d parameter (0.5 - 1.5)
    float                   paniniCompress;    // Compression factor
    
    // Cylindrical projection parameters
    float                   cylRadius;         // Cylinder radius
    float                   cylHeight;         // Cylinder height
    
    // Render targets for multi-pass
    int                     regionTextures[5];
    int                     compositeTexture;
    
    // Performance metrics
    int                     regionsRendered;
    float                   renderTime;
} uwState_t;

// Global ultra-wide state
extern uwState_t uwState;

// Function declarations

// System initialization
void R_InitUltraWide(void);
void R_ShutdownUltraWide(void);

// Configuration
void R_ConfigureUltraWide(int width, int height, float fov);
void R_SetUltraWideMode(ultrawideRenderMode_t mode);
qboolean R_IsUltraWide(float aspectRatio);

// Multi-region setup
void R_SetupMultiRegions(void);
void R_CalculateRegionFrustum(uwRegion_t *region, int regionIndex, int totalRegions);
void R_ApplyRegionTransform(const uwRegion_t *region);

// Projection methods
void R_SetupCylindricalProjection(float fovY, float aspect);
void R_SetupPaniniProjection(float fovY, float aspect, float d);
void R_CalculatePaniniMatrix(float *matrix, float fovY, float aspect, float d);

// Rendering
void R_BeginUltraWideFrame(void);
void R_RenderUltraWideRegion(int region);
void R_EndUltraWideFrame(void);
void R_CompositeUltraWideRegions(void);

// Perspective correction
void R_CorrectPerspective(float *projMatrix, float xOffset, float aspectRatio);
float R_CalculateFOVCorrection(float baseFOV, float aspectRatio);
void R_AdjustFOVForAspect(float *fovX, float *fovY, float aspect);

// HUD/UI adjustments
void R_ScaleHUDForUltraWide(float *x, float *y, float *w, float *h);
void R_GetSafeZone(float aspect, float *left, float *right);
void R_ConstrainToSafeZone(float *x, float *width, float aspect);

// Debug visualization
void R_DrawUltraWideDebug(void);
void R_ShowRegionBoundaries(void);

// CVARs
extern cvar_t *r_ultraWide;
extern cvar_t *r_ultraWideMode;
extern cvar_t *r_ultraWideFOVScale;
extern cvar_t *r_ultraWideDebug;
extern cvar_t *r_paniniDistance;
extern cvar_t *r_hudSafeZone;

#endif // TR_ULTRAWIDE_H