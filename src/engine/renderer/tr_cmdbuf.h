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

#ifndef TR_CMDBUF_H
#define TR_CMDBUF_H

// Command buffer system for multi-threaded rendering

#define CMD_BUFFER_SIZE (16 * 1024 * 1024)  // 16MB per buffer
#define NUM_CMD_BUFFERS 2

typedef struct commandBuffer_s {
    byte        data[CMD_BUFFER_SIZE];
    size_t      used;
    size_t      reserved;      // For in-progress allocations
    int         frameNumber;
    qboolean    ready;         // Ready for backend consumption
    qboolean    inUse;        // Currently being written/read
} commandBuffer_t;

typedef struct commandBufferState_s {
    commandBuffer_t buffers[NUM_CMD_BUFFERS];
    int             currentBuffer;    // Index for frontend writing
    int             renderBuffer;     // Index for backend reading
    
    // Synchronization
    void*           swapMutex;        // Platform-specific mutex
    void*           renderSemaphore;  // Signals backend to start
    void*           completeSemaphore; // Signals frontend completion done
    
    // Statistics
    int             totalCommands;
    size_t          maxUsed;
    double          swapTime;
} commandBufferState_t;

// Base command structure - all commands start with this
typedef struct baseCommand_s {
    renderCommand_t commandId;
    size_t          commandSize;
} baseCommand_t;

// Enhanced draw view command
typedef struct drawViewCommand_s {
    baseCommand_t   header;
    viewParms_t     viewParms;       // Copied, not pointer
    trRefdef_t      refdef;           // Copied refdef
    drawSurf_t**    drawSurfs;       // Pointer to sorted list
    int             numDrawSurfs;
    dlight_t*       dlights;         // Dynamic lights
    int             numDlights;
#ifdef USE_PMLIGHT
    litSurf_t*      litSurfs;
    int             numLitSurfs;
#endif
} drawViewCommand_t;

// Enhanced swap buffers command
typedef struct swapBuffersCommandMT_s {
    baseCommand_t   header;
    int             vsync;
} swapBuffersCommandMT_t;

// Set color command
typedef struct setColorCommandMT_s {
    baseCommand_t   header;
    float           color[4];
} setColorCommandMT_t;

// Stretch pic command
typedef struct stretchPicCommandMT_s {
    baseCommand_t   header;
    shader_t*       shader;
    float           x, y, w, h;
    float           s1, t1, s2, t2;
} stretchPicCommandMT_t;

// Draw buffer command
typedef struct drawBufferCommandMT_s {
    baseCommand_t   header;
    int             buffer;
} drawBufferCommandMT_t;

// Finish bloom command
typedef struct finishBloomCommandMT_s {
    baseCommand_t   header;
} finishBloomCommandMT_t;

// Color mask command
typedef struct colorMaskCommandMT_s {
    baseCommand_t   header;
    byte            rgba[4];
} colorMaskCommandMT_t;

// Clear depth command
typedef struct clearDepthCommandMT_s {
    baseCommand_t   header;
} clearDepthCommandMT_t;

// Clear color command
typedef struct clearColorCommandMT_s {
    baseCommand_t   header;
} clearColorCommandMT_t;

// Global command buffer state
extern commandBufferState_t cmdBufferState;

// Command buffer functions
void R_InitCommandBuffers(void);
void R_ShutdownCommandBuffers(void);
void* R_GetCommandBufferMT(size_t bytes);  // Multi-threaded version
void R_SwapCommandBuffers(void);
void R_WaitForBackend(void);
qboolean R_BackendBusy(void);

// Backend execution
void RB_ExecuteCommandBuffer(const commandBuffer_t *buffer);

// Statistics
void R_PrintCommandBufferStats(void);

#endif // TR_CMDBUF_H