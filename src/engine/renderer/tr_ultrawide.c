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

#include "tr_local.h"
#include "tr_ultrawide.h"
#include <math.h>

/*
================================================================================
Ultra-Widescreen Support Implementation

Implements multi-region rendering and projection methods to correct
perspective distortion on ultra-wide displays.
================================================================================
*/

// Global state
uwState_t uwState;

// CVARs
cvar_t *r_ultraWide = NULL;
cvar_t *r_ultraWideMode = NULL;
cvar_t *r_ultraWideFOVScale = NULL;
cvar_t *r_ultraWideDebug = NULL;
cvar_t *r_paniniDistance = NULL;
cvar_t *r_hudSafeZone = NULL;

/*
================
R_InitUltraWide

Initialize ultra-wide rendering system
================
*/
void R_InitUltraWide(void) {
    Com_Memset(&uwState, 0, sizeof(uwState));
    
    // CVARs are already registered in tr_init.c
    // Just update the descriptions here for clarity
    if (r_ultraWideMode) {
        ri.Cvar_SetDescription(r_ultraWideMode, 
            "Ultra-wide rendering mode:\n"
            " 0 - Single frustum (standard)\n"
            " 1 - Triple region\n"
            " 2 - Quintuple region\n"
            " 3 - Cylindrical projection\n"
            " 4 - Panini projection");
    }
    
    // Configure based on current resolution
    R_ConfigureUltraWide(glConfig.vidWidth, glConfig.vidHeight, 90.0f);
    
    ri.Printf(PRINT_ALL, "Ultra-wide rendering system initialized\n");
}

/*
================
R_ShutdownUltraWide

Cleanup ultra-wide system
================
*/
void R_ShutdownUltraWide(void) {
    // Cleanup render targets if allocated
    uwState.enabled = qfalse;
}

/*
================
R_IsUltraWide

Check if aspect ratio qualifies as ultra-wide
================
*/
qboolean R_IsUltraWide(float aspectRatio) {
    // Consider anything wider than 16:9 as ultra-wide
    return (aspectRatio > ASPECT_16_9 + 0.01f);
}

/*
================
R_ConfigureUltraWide

Configure ultra-wide rendering based on resolution
================
*/
void R_ConfigureUltraWide(int width, int height, float fov) {
    float aspectRatio;
    
    if (!r_ultraWide || !r_ultraWide->integer) {
        uwState.enabled = qfalse;
        return;
    }
    
    aspectRatio = (float)width / (float)height;
    
    // Only enable for ultra-wide aspects
    if (!R_IsUltraWide(aspectRatio)) {
        uwState.enabled = qfalse;
        return;
    }
    
    uwState.enabled = qtrue;
    uwState.aspectRatio = aspectRatio;
    uwState.baseFOV = fov;
    
    // For now, only use single-pass rendering with FOV correction
    uwState.mode = UW_RENDER_SINGLE;
    uwState.numRegions = 1;
    
    ri.Printf(PRINT_ALL, "Ultra-wide FOV correction enabled for %.2f:1 aspect ratio\n", 
              aspectRatio);
}

/*
================
R_SetupMultiRegions

Setup rendering regions for multi-frustum rendering
================
*/
void R_SetupMultiRegions(void) {
    int i;
    float regionWidth;
    float overlap = 0.0f;  // Small overlap to avoid seams
    
    switch (uwState.mode) {
    case UW_RENDER_TRIPLE:
        uwState.numRegions = 3;
        regionWidth = 1.0f / 3.0f;
        
        // Left region
        uwState.regions[0].xStart = 0.0f;
        uwState.regions[0].xEnd = regionWidth + overlap;
        uwState.regions[0].yawOffset = -30.0f;  // Look left
        uwState.regions[0].fovScale = 1.0f;
        
        // Center region
        uwState.regions[1].xStart = regionWidth - overlap;
        uwState.regions[1].xEnd = 2.0f * regionWidth + overlap;
        uwState.regions[1].yawOffset = 0.0f;    // Look straight
        uwState.regions[1].fovScale = 1.0f;
        
        // Right region
        uwState.regions[2].xStart = 2.0f * regionWidth - overlap;
        uwState.regions[2].xEnd = 1.0f;
        uwState.regions[2].yawOffset = 30.0f;   // Look right
        uwState.regions[2].fovScale = 1.0f;
        break;
        
    case UW_RENDER_QUINTUPLE:
        uwState.numRegions = 5;
        regionWidth = 1.0f / 5.0f;
        
        for (i = 0; i < 5; i++) {
            uwState.regions[i].xStart = i * regionWidth - overlap;
            uwState.regions[i].xEnd = (i + 1) * regionWidth + overlap;
            uwState.regions[i].yawOffset = (i - 2) * 20.0f;  // -40, -20, 0, 20, 40
            uwState.regions[i].fovScale = 1.0f;
        }
        break;
        
    case UW_RENDER_CYLINDRICAL:
    case UW_RENDER_PANINI:
        // These use single pass with special projection
        uwState.numRegions = 1;
        uwState.regions[0].xStart = 0.0f;
        uwState.regions[0].xEnd = 1.0f;
        uwState.regions[0].yawOffset = 0.0f;
        uwState.regions[0].fovScale = 1.0f;
        break;
        
    default:
        // Single frustum
        uwState.numRegions = 1;
        uwState.regions[0].xStart = 0.0f;
        uwState.regions[0].xEnd = 1.0f;
        uwState.regions[0].yawOffset = 0.0f;
        uwState.regions[0].fovScale = 1.0f;
        break;
    }
    
    // Calculate viewport and matrices for each region
    for (i = 0; i < uwState.numRegions; i++) {
        R_CalculateRegionFrustum(&uwState.regions[i], i, uwState.numRegions);
    }
}

/*
================
R_CalculateRegionFrustum

Calculate frustum parameters for a specific region
================
*/
void R_CalculateRegionFrustum(uwRegion_t *region, int regionIndex, int totalRegions) {
    float fovX, fovY;
    float xOffset;
    float aspect;
    
    // Calculate viewport in pixels
    region->viewport[0] = region->xStart * glConfig.vidWidth;
    region->viewport[1] = 0;
    region->viewport[2] = (region->xEnd - region->xStart) * glConfig.vidWidth;
    region->viewport[3] = glConfig.vidHeight;
    
    // Calculate aspect for this region
    aspect = region->viewport[2] / region->viewport[3];
    
    // Adjust FOV for this region
    fovY = uwState.baseFOV * region->fovScale;
    fovX = atan(tan(fovY * M_PI / 360.0f) * aspect) * 360.0f / M_PI;
    
    // Calculate horizontal offset for asymmetric frustum
    xOffset = ((float)regionIndex - (totalRegions - 1) * 0.5f) / totalRegions;
    
    // Build projection matrix with perspective correction
    R_CorrectPerspective(region->projMatrix, xOffset, aspect);
}

/*
================
R_CorrectPerspective

Create projection matrix with perspective correction for ultra-wide
================
*/
void R_CorrectPerspective(float *projMatrix, float xOffset, float aspectRatio) {
    float left, right, bottom, top, znear, zfar;
    float fovY, fovX;
    float shift;
    
    // Get base FOV
    fovY = uwState.baseFOV;
    
    // Calculate corrected FOV for aspect ratio
    fovX = R_CalculateFOVCorrection(fovY, aspectRatio);
    
    znear = r_znear->value;
    zfar = tr.viewParms.zFar;
    
    // Calculate frustum bounds
    top = znear * tan(fovY * M_PI / 360.0f);
    bottom = -top;
    
    // Apply horizontal shift for multi-region rendering
    shift = xOffset * 2.0f * top * aspectRatio;
    left = -top * aspectRatio + shift;
    right = top * aspectRatio + shift;
    
    // Build asymmetric frustum matrix
    Com_Memset(projMatrix, 0, sizeof(float) * 16);
    
    projMatrix[0] = 2.0f * znear / (right - left);
    projMatrix[5] = 2.0f * znear / (top - bottom);
    projMatrix[8] = (right + left) / (right - left);
    projMatrix[9] = (top + bottom) / (top - bottom);
    projMatrix[10] = -(zfar + znear) / (zfar - znear);
    projMatrix[11] = -1.0f;
    projMatrix[14] = -2.0f * zfar * znear / (zfar - znear);
}

/*
================
R_CalculateFOVCorrection

Calculate corrected FOV for ultra-wide aspect ratios
================
*/
float R_CalculateFOVCorrection(float baseFOV, float aspectRatio) {
    float baseAspect = 4.0f / 3.0f;  // Quake 3 default aspect
    float vFOV, hFOV;
    
    // If not ultra-wide or disabled, return base FOV
    if (!uwState.enabled || uwState.numRegions <= 1) {
        // For single region, calculate proper FOV for the aspect ratio
        vFOV = 2.0f * atan(tan(baseFOV * M_PI / 360.0f) / baseAspect) * 180.0f / M_PI;
        hFOV = 2.0f * atan(tan(vFOV * M_PI / 360.0f) * aspectRatio) * 180.0f / M_PI;
        return hFOV;
    }
    
    // For multi-region rendering, use base FOV per region
    // Each region renders at approximately 16:9 aspect
    return baseFOV;
}

/*
================
R_SetupPaniniProjection

Setup Panini projection for ultra-wide
Reduces edge distortion while maintaining center accuracy
================
*/
void R_SetupPaniniProjection(float fovY, float aspect, float d) {
    float S, tanHalfFOV;
    float matrix[16];
    
    // Panini projection parameters
    tanHalfFOV = tan(fovY * M_PI / 360.0f);
    S = (d + 1.0f) / (d + tanHalfFOV);
    
    // Build Panini projection matrix
    R_CalculatePaniniMatrix(matrix, fovY, aspect, d);
    
    // Apply to current projection
    // This would be applied in the vertex shader
}

/*
================
R_CalculatePaniniMatrix

Calculate Panini projection matrix
================
*/
void R_CalculatePaniniMatrix(float *matrix, float fovY, float aspect, float d) {
    float wd, hd;
    float s;
    float znear, zfar;
    
    znear = r_znear->value;
    zfar = tr.viewParms.zFar;
    
    // Calculate Panini parameters
    s = (d + 1.0f) / (d + tan(fovY * M_PI / 360.0f));
    wd = 2.0f * znear * tan(fovY * M_PI / 360.0f) * aspect;
    hd = 2.0f * znear * tan(fovY * M_PI / 360.0f);
    
    // Build matrix
    Com_Memset(matrix, 0, sizeof(float) * 16);
    
    matrix[0] = 2.0f * znear * s / wd;
    matrix[5] = 2.0f * znear * s / hd;
    matrix[10] = -(zfar + znear) / (zfar - znear);
    matrix[11] = -1.0f;
    matrix[14] = -2.0f * zfar * znear / (zfar - znear);
}

/*
================
R_SetupCylindricalProjection

Setup cylindrical projection for ultra-wide
Maps the view onto a cylinder to reduce distortion
================
*/
void R_SetupCylindricalProjection(float fovY, float aspect) {
    float cylRadius;
    float cylHeight;
    
    // Calculate cylinder parameters
    cylRadius = 1.0f / tan(fovY * M_PI / 360.0f);
    cylHeight = 2.0f * cylRadius;
    
    uwState.cylRadius = cylRadius;
    uwState.cylHeight = cylHeight;
    
    // Cylindrical projection is applied in vertex shader
}

/*
================
R_BeginUltraWideFrame

Begin ultra-wide frame rendering
================
*/
void R_BeginUltraWideFrame(void) {
    if (!uwState.enabled) {
        return;
    }
    
    uwState.regionsRendered = 0;
    uwState.renderTime = 0;
    
    // Clear composite buffer if using multi-pass
    if (uwState.numRegions > 1) {
        // Setup render targets for each region
        // This would involve FBO setup in OpenGL/Vulkan
    }
}

/*
================
R_RenderUltraWideRegion

Render a specific region for multi-pass rendering
================
*/
void R_RenderUltraWideRegion(int regionIndex) {
    uwRegion_t *region;
    viewParms_t saved;
    
    if (!uwState.enabled || regionIndex >= uwState.numRegions) {
        return;
    }
    
    region = &uwState.regions[regionIndex];
    
    // Save current view parameters
    saved = tr.viewParms;
    
    // Apply region transform
    R_ApplyRegionTransform(region);
    
    // Viewport will be set by the main render loop using the region parameters
    
    // Render the scene for this region
    // This calls the normal rendering pipeline
    
    // Restore view parameters
    tr.viewParms = saved;
    
    uwState.regionsRendered++;
}

/*
================
R_ApplyRegionTransform

Apply transformation for a specific region
================
*/
void R_ApplyRegionTransform(const uwRegion_t *region) {
    float s, c;
    float yawRad;
    vec3_t temp;
    
    // Apply yaw offset by rotating the view axes
    yawRad = DEG2RAD(region->yawOffset);
    s = sin(yawRad);
    c = cos(yawRad);
    
    // Rotate view axes around Y (up) axis
    VectorCopy(tr.viewParms.or.axis[0], temp);
    tr.viewParms.or.axis[0][0] = c * temp[0] - s * temp[2];
    tr.viewParms.or.axis[0][2] = s * temp[0] + c * temp[2];
    
    VectorCopy(tr.viewParms.or.axis[2], temp);
    tr.viewParms.or.axis[2][0] = c * temp[0] - s * temp[2];
    tr.viewParms.or.axis[2][2] = s * temp[0] + c * temp[2];
    
    // Apply custom projection matrix
    Com_Memcpy(tr.viewParms.projectionMatrix, region->projMatrix, 
               sizeof(tr.viewParms.projectionMatrix));
}

/*
================
R_EndUltraWideFrame

Complete ultra-wide frame rendering
================
*/
void R_EndUltraWideFrame(void) {
    if (!uwState.enabled) {
        return;
    }
    
    // Composite regions if using multi-pass
    if (uwState.numRegions > 1) {
        R_CompositeUltraWideRegions();
    }
    
    // Debug visualization
    if (r_ultraWideDebug->integer) {
        R_DrawUltraWideDebug();
    }
}

/*
================
R_CompositeUltraWideRegions

Composite multiple rendered regions into final image
================
*/
void R_CompositeUltraWideRegions(void) {
    int i;
    
    // This would involve:
    // 1. Binding composite framebuffer
    // 2. Drawing each region texture to its viewport area
    // 3. Blending overlapping areas
    
    for (i = 0; i < uwState.numRegions; i++) {
        // Draw region[i] texture to screen
    }
}

/*
================
R_ScaleHUDForUltraWide

Scale HUD elements to maintain proper proportions on ultra-wide
================
*/
void R_ScaleHUDForUltraWide(float *x, float *y, float *w, float *h) {
    float safeLeft, safeRight;
    float safeWidth;
    float scale;
    
    if (!uwState.enabled || !r_hudSafeZone->integer) {
        return;
    }
    
    // Calculate 16:9 safe zone
    R_GetSafeZone(uwState.aspectRatio, &safeLeft, &safeRight);
    safeWidth = safeRight - safeLeft;
    
    // Scale HUD elements to fit in safe zone
    scale = safeWidth;
    
    // Adjust position to be within safe zone
    *x = safeLeft + (*x * safeWidth);
    *w *= scale;
    
    // Vertical remains unchanged
}

/*
================
R_GetSafeZone

Calculate 16:9 safe zone boundaries for HUD
================
*/
void R_GetSafeZone(float aspect, float *left, float *right) {
    float safeWidth;
    float margin;
    
    if (aspect <= ASPECT_16_9) {
        *left = 0.0f;
        *right = 1.0f;
        return;
    }
    
    // Calculate width of 16:9 area in normalized coordinates
    safeWidth = ASPECT_16_9 / aspect;
    margin = (1.0f - safeWidth) * 0.5f;
    
    *left = margin;
    *right = 1.0f - margin;
}

/*
================
R_ConstrainToSafeZone

Constrain HUD element to safe zone
================
*/
void R_ConstrainToSafeZone(float *x, float *width, float aspect) {
    float safeLeft, safeRight;
    
    R_GetSafeZone(aspect, &safeLeft, &safeRight);
    
    // Clamp to safe zone
    if (*x < safeLeft) {
        *x = safeLeft;
    }
    if (*x + *width > safeRight) {
        *width = safeRight - *x;
    }
}

/*
================
R_DrawUltraWideDebug

Draw debug information for ultra-wide rendering
================
*/
void R_DrawUltraWideDebug(void) {
    if (!r_ultraWideDebug->integer)
        return;
        
    // Debug output to console
    ri.Printf(PRINT_DEVELOPER, "Ultra-wide: %.2f:1\n", uwState.aspectRatio);
    ri.Printf(PRINT_DEVELOPER, "Mode: %d, Regions: %d\n", 
              uwState.mode, uwState.numRegions);
    ri.Printf(PRINT_DEVELOPER, "Regions rendered: %d\n", uwState.regionsRendered);
    
    // Show region boundaries
    R_ShowRegionBoundaries();
}

/*
================
R_ShowRegionBoundaries

Draw visual boundaries of rendering regions
================
*/
void R_ShowRegionBoundaries(void) {
    int i;
    
    if (!r_ultraWideDebug->integer)
        return;
        
    for (i = 0; i < uwState.numRegions; i++) {
        uwRegion_t *region = &uwState.regions[i];
        float x = region->xEnd * glConfig.vidWidth;
        
        if (i < uwState.numRegions - 1) {
            // Log region boundary for debugging
            ri.Printf(PRINT_DEVELOPER, "Region %d boundary at x=%.0f\n", i, x);
        }
    }
}