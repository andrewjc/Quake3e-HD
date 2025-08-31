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
#include "tr_cmdbuf.h"

// Global command buffer state
commandBufferState_t cmdBufferState;

// CVars for debugging and control
cvar_t *r_smp;              // Enable/disable multi-threading
cvar_t *r_showCommandBuffer;
cvar_t *r_maxCommandBuffer;

// External synchronization functions (defined in tr_sync.c)
void* Sys_CreateMutex(void);
void Sys_DestroyMutex(void* mutex);
void Sys_LockMutex(void* mutex);
void Sys_UnlockMutex(void* mutex);
void* Sys_CreateSemaphore(void);
void Sys_DestroySemaphore(void* sem);
void Sys_WaitSemaphore(void* sem);
void Sys_SignalSemaphore(void* sem);

/*
================
R_InitCommandBuffers

Initialize the command buffer system
================
*/
void R_InitCommandBuffers(void) {
    int i;

    Com_Memset(&cmdBufferState, 0, sizeof(cmdBufferState));

    // Initialize buffers
    for (i = 0; i < NUM_CMD_BUFFERS; i++) {
        commandBuffer_t *buf = &cmdBufferState.buffers[i];
        buf->used = 0;
        buf->reserved = 0;
        buf->frameNumber = 0;
        buf->ready = qfalse;
        buf->inUse = qfalse;
    }

    // Set initial buffer indices
    cmdBufferState.currentBuffer = 0;
    cmdBufferState.renderBuffer = 1;
    cmdBufferState.buffers[0].inUse = qtrue;

    // Register CVars
    r_smp = ri.Cvar_Get("r_smp", "1", CVAR_ARCHIVE | CVAR_LATCH);
    r_showCommandBuffer = ri.Cvar_Get("r_showCommandBuffer", "0", CVAR_CHEAT);
    r_maxCommandBuffer = ri.Cvar_Get("r_maxCommandBuffer", "0", 0);

    // Create synchronization primitives if SMP is enabled
    if (r_smp->integer) {
        cmdBufferState.swapMutex = Sys_CreateMutex();
        cmdBufferState.renderSemaphore = Sys_CreateSemaphore();
        cmdBufferState.completeSemaphore = Sys_CreateSemaphore();
        
        if (!cmdBufferState.swapMutex || !cmdBufferState.renderSemaphore || 
            !cmdBufferState.completeSemaphore) {
            ri.Printf(PRINT_WARNING, "Failed to create synchronization primitives, disabling SMP\n");
            ri.Cvar_Set("r_smp", "0");
            R_ShutdownCommandBuffers();
            return;
        }
    }

    ri.Printf(PRINT_ALL, "Command buffer system initialized (SMP: %s)\n", 
              r_smp->integer ? "enabled" : "disabled");
}

/*
================
R_ShutdownCommandBuffers

Cleanup command buffer system
================
*/
void R_ShutdownCommandBuffers(void) {
    // Destroy synchronization primitives
    if (cmdBufferState.swapMutex) {
        Sys_DestroyMutex(cmdBufferState.swapMutex);
        cmdBufferState.swapMutex = NULL;
    }
    
    if (cmdBufferState.renderSemaphore) {
        Sys_DestroySemaphore(cmdBufferState.renderSemaphore);
        cmdBufferState.renderSemaphore = NULL;
    }
    
    if (cmdBufferState.completeSemaphore) {
        Sys_DestroySemaphore(cmdBufferState.completeSemaphore);
        cmdBufferState.completeSemaphore = NULL;
    }
}

/*
================
R_GetCommandBufferMT

Thread-safe command buffer allocation
Returns NULL if there is not enough space
================
*/
void* R_GetCommandBufferMT(size_t bytes) {
    commandBuffer_t *buf;
    void *data;
    size_t maxSize;
    
    // Align to pointer size
    bytes = PAD(bytes, sizeof(void*));
    
    // Check against limit
    maxSize = r_maxCommandBuffer->integer > 0 ? 
              r_maxCommandBuffer->integer : CMD_BUFFER_SIZE;
    
    buf = &cmdBufferState.buffers[cmdBufferState.currentBuffer];
    
    // Check if we have enough space (leave room for end-of-list command)
    if (buf->used + bytes + sizeof(int) > maxSize) {
        if (bytes > maxSize - sizeof(int)) {
            ri.Error(ERR_FATAL, "R_GetCommandBufferMT: bad size %i", (int)bytes);
        }
        
        // Out of space - this shouldn't happen with proper sizing
        if (r_showCommandBuffer->integer) {
            ri.Printf(PRINT_WARNING, "R_GetCommandBufferMT: overflow (%d + %d > %d)\n",
                     (int)buf->used, (int)bytes, (int)maxSize);
        }
        return NULL;
    }
    
    // Allocate from current position
    data = buf->data + buf->used;
    buf->used += bytes;
    
    // Update statistics
    cmdBufferState.totalCommands++;
    if (buf->used > cmdBufferState.maxUsed) {
        cmdBufferState.maxUsed = buf->used;
    }
    
    return data;
}

/*
================
R_SwapCommandBuffers

Swap frontend and backend command buffers
This is the synchronization point between threads
================
*/
void R_SwapCommandBuffers(void) {
    commandBuffer_t *currentBuf;
    int temp;
    double startTime = 0;
    
    if (r_showCommandBuffer->integer) {
        startTime = ri.Milliseconds();
    }
    
    // Get current buffer
    currentBuf = &cmdBufferState.buffers[cmdBufferState.currentBuffer];
    
    // Add end-of-list command
    if (currentBuf->used + sizeof(int) <= CMD_BUFFER_SIZE) {
        *(int*)(currentBuf->data + currentBuf->used) = RC_END_OF_LIST;
        currentBuf->used += sizeof(int);
    }
    
    if (r_smp->integer) {
        // Multi-threaded path
        Sys_LockMutex(cmdBufferState.swapMutex);
        
        // Mark current buffer as ready
        currentBuf->ready = qtrue;
        currentBuf->inUse = qfalse;
        currentBuf->frameNumber = tr.frameCount;
        
        // Swap indices
        temp = cmdBufferState.currentBuffer;
        cmdBufferState.currentBuffer = cmdBufferState.renderBuffer;
        cmdBufferState.renderBuffer = temp;
        
        // Reset new current buffer
        currentBuf = &cmdBufferState.buffers[cmdBufferState.currentBuffer];
        currentBuf->used = 0;
        currentBuf->reserved = 0;
        currentBuf->ready = qfalse;
        currentBuf->inUse = qtrue;
        
        Sys_UnlockMutex(cmdBufferState.swapMutex);
        
        // Signal render thread to start processing
        Sys_SignalSemaphore(cmdBufferState.renderSemaphore);
    } else {
        // Single-threaded path - execute immediately
        RB_ExecuteCommandBuffer(currentBuf);
        
        // Reset buffer for next frame
        currentBuf->used = 0;
        currentBuf->reserved = 0;
    }
    
    if (r_showCommandBuffer->integer) {
        cmdBufferState.swapTime = ri.Milliseconds() - startTime;
    }
}

/*
================
R_WaitForBackend

Wait for backend to finish processing
Used to prevent frontend from getting too far ahead
================
*/
void R_WaitForBackend(void) {
    if (!r_smp->integer) {
        return;  // Nothing to wait for in single-threaded mode
    }
    
    // Wait for backend to signal completion
    Sys_WaitSemaphore(cmdBufferState.completeSemaphore);
}

/*
================
R_BackendBusy

Check if backend is still processing
================
*/
qboolean R_BackendBusy(void) {
    commandBuffer_t *renderBuf;
    
    if (!r_smp->integer) {
        return qfalse;  // Never busy in single-threaded mode
    }
    
    renderBuf = &cmdBufferState.buffers[cmdBufferState.renderBuffer];
    return renderBuf->ready && renderBuf->inUse;
}

/*
================
RB_ExecuteCommandBuffer

Execute commands from a command buffer
This runs on the render thread in SMP mode
================
*/
void RB_ExecuteCommandBuffer(const commandBuffer_t *buffer) {
    const byte *data = buffer->data;
    const byte *dataEnd = data + buffer->used;
    const baseCommand_t *cmd;
    
    // Process all commands in the buffer
    while (data < dataEnd) {
        // Align to pointer boundary
        data = PADP(data, sizeof(void*));
        
        cmd = (const baseCommand_t*)data;
        
        switch (cmd->commandId) {
        case RC_DRAW_VIEW:
            {
                const drawViewCommand_t *dvc = (const drawViewCommand_t*)cmd;
                // Call existing draw surfs function with the stored data
                drawSurfsCommand_t oldCmd;
                oldCmd.commandId = RC_DRAW_SURFS;
                oldCmd.drawSurfs = *dvc->drawSurfs;
                oldCmd.numDrawSurfs = dvc->numDrawSurfs;
                oldCmd.refdef = dvc->refdef;
                oldCmd.viewParms = dvc->viewParms;
                RB_DrawSurfs(&oldCmd);
            }
            break;
            
        case RC_SET_COLOR:
            {
                const setColorCommand_t *scc = (const setColorCommand_t*)cmd;
                setColorCommand_t oldCmd;
                oldCmd.commandId = RC_SET_COLOR;
                Com_Memcpy(oldCmd.color, scc->color, sizeof(oldCmd.color));
                RB_SetColor(&oldCmd);
            }
            break;
            
        case RC_STRETCH_PIC:
            {
                const stretchPicCommandMT_t *spc = (const stretchPicCommandMT_t*)cmd;
                stretchPicCommand_t oldCmd;
                oldCmd.commandId = RC_STRETCH_PIC;
                oldCmd.shader = spc->shader;
                oldCmd.x = spc->x;
                oldCmd.y = spc->y;
                oldCmd.w = spc->w;
                oldCmd.h = spc->h;
                oldCmd.s1 = spc->s1;
                oldCmd.t1 = spc->t1;
                oldCmd.s2 = spc->s2;
                oldCmd.t2 = spc->t2;
                RB_StretchPic(&oldCmd);
            }
            break;
            
        case RC_DRAW_BUFFER:
            {
                const drawBufferCommandMT_t *dbc = (const drawBufferCommandMT_t*)cmd;
                drawBufferCommand_t oldCmd;
                oldCmd.commandId = RC_DRAW_BUFFER;
                oldCmd.buffer = dbc->buffer;
                RB_DrawBuffer(&oldCmd);
            }
            break;
            
        case RC_SWAP_BUFFERS:
            {
                const swapBuffersCommandMT_t *sbc = (const swapBuffersCommandMT_t*)cmd;
                swapBuffersCommand_t oldCmd;
                oldCmd.commandId = RC_SWAP_BUFFERS;
                RB_SwapBuffers(&oldCmd);
            }
            break;
            
        case RC_FINISHBLOOM:
            {
                finishBloomCommand_t oldCmd;
                oldCmd.commandId = RC_FINISHBLOOM;
                RB_FinishBloom(&oldCmd);
            }
            break;
            
        case RC_COLORMASK:
            {
                const colorMaskCommandMT_t *cmc = (const colorMaskCommandMT_t*)cmd;
                colorMaskCommand_t oldCmd;
                oldCmd.commandId = RC_COLORMASK;
                Com_Memcpy(oldCmd.rgba, cmc->rgba, sizeof(oldCmd.rgba));
                RB_ColorMask(&oldCmd);
            }
            break;
            
        case RC_CLEARDEPTH:
            {
                clearDepthCommand_t oldCmd;
                oldCmd.commandId = RC_CLEARDEPTH;
                RB_ClearDepth(&oldCmd);
            }
            break;
            
        case RC_CLEARCOLOR:
            {
                clearColorCommand_t oldCmd;
                oldCmd.commandId = RC_CLEARCOLOR;
                RB_ClearColor(&oldCmd);
            }
            break;
            
        case RC_END_OF_LIST:
        default:
            // End of commands
#ifdef USE_VULKAN
            vk_end_frame();
#endif
            return;
        }
        
        // Move to next command
        data += cmd->commandSize;
    }
}

/*
================
R_PrintCommandBufferStats

Print command buffer statistics for debugging
================
*/
void R_PrintCommandBufferStats(void) {
    ri.Printf(PRINT_ALL, "Command Buffer Stats:\n");
    ri.Printf(PRINT_ALL, "  Total commands: %d\n", cmdBufferState.totalCommands);
    ri.Printf(PRINT_ALL, "  Max buffer used: %d KB / %d KB\n", 
             (int)(cmdBufferState.maxUsed / 1024), CMD_BUFFER_SIZE / 1024);
    ri.Printf(PRINT_ALL, "  Swap time: %.2f ms\n", cmdBufferState.swapTime);
    ri.Printf(PRINT_ALL, "  Current buffer: %d\n", cmdBufferState.currentBuffer);
    ri.Printf(PRINT_ALL, "  Render buffer: %d\n", cmdBufferState.renderBuffer);
    
    for (int i = 0; i < NUM_CMD_BUFFERS; i++) {
        commandBuffer_t *buf = &cmdBufferState.buffers[i];
        ri.Printf(PRINT_ALL, "  Buffer %d: used=%d ready=%d inUse=%d frame=%d\n",
                 i, (int)buf->used, buf->ready, buf->inUse, buf->frameNumber);
    }
}