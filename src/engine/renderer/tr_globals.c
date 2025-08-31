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
#include "tr_scene.h"

// Frame scene management constants
#define MAX_FRAME_SCENES 2
#define FRAME_MEMORY_SIZE (16 * 1024 * 1024)  // 16MB per frame

// Frame scene management globals
frameScene_t frameScenes[MAX_FRAME_SCENES];
int currentFrameScene = 0;

// Debugging cvars
cvar_t *r_showSortOrder = NULL;
cvar_t *r_showBatching = NULL;

// Frame memory allocation
static byte frameMemory[FRAME_MEMORY_SIZE];
static int frameMemoryUsed = 0;

/*
================
R_FrameAlloc

Allocate memory from frame memory pool
================
*/
void *R_FrameAlloc(int size) {
    void *ptr;
    
    // Align to 16 bytes
    size = (size + 15) & ~15;
    
    if (frameMemoryUsed + size > FRAME_MEMORY_SIZE) {
        ri.Error(ERR_DROP, "R_FrameAlloc: out of frame memory");
    }
    
    ptr = frameMemory + frameMemoryUsed;
    frameMemoryUsed += size;
    
    return ptr;
}

/*
================
R_ClearFrameMemory

Clear frame memory pool
================
*/
void R_ClearFrameMemory(void) {
    frameMemoryUsed = 0;
}

/*
================
VectorSet4

Set vec4_t values
================
*/
void VectorSet4(vec4_t v, float x, float y, float z, float w) {
    v[0] = x;
    v[1] = y;
    v[2] = z;
    v[3] = w;
}

/*
================
qglColor4fv

OpenGL function placeholder
================
*/
void qglColor4fv(const float *v) {
    // TODO: Implement or remove when migrating to Vulkan
}

/*
================
qglVertex3fv

OpenGL function placeholder
================
*/
void qglVertex3fv(const float *v) {
    // TODO: Implement or remove when migrating to Vulkan
}

/*
================
qglVertex3f

OpenGL function placeholder
================
*/
void qglVertex3f(float x, float y, float z) {
    // TODO: Implement or remove when migrating to Vulkan
}

/*
================
qglEnd

OpenGL function placeholder
================
*/
void qglEnd(void) {
    // TODO: Implement or remove when migrating to Vulkan
}

// Vulkan globals
#ifdef USE_VULKAN
VkSampler vk_default_sampler = VK_NULL_HANDLE;
#endif