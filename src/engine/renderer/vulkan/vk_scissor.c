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

#include "../core/tr_local.h"
#include "vk.h"
#include "../lighting/tr_light.h"
#include "../lighting/tr_light_scissor.h"
#include "../lighting/tr_interaction.h"

/*
================================================================================
Phase 7: Vulkan Scissor Implementation

This file implements Vulkan-specific scissor rectangle and depth bounds
testing for optimized fragment shader execution.
================================================================================
*/

// Current scissor state
static VkRect2D currentScissor;
static float currentDepthBounds[2];
static qboolean scissorEnabled = qfalse;
static qboolean depthBoundsEnabled = qfalse;

// Vulkan device capabilities
static qboolean vk_depthBoundsSupported = qfalse;

// Forward declarations for internal functions
static void VK_DisableDepthBounds(void);
static void VK_ResetScissor(void);

/*
================
VK_InitScissorSupport

Check for scissor and depth bounds capabilities
================
*/
void VK_InitScissorSupport(VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physicalDevice, &features);
    
    // Check for depth bounds test support
    vk_depthBoundsSupported = features.depthBounds ? qtrue : qfalse;
    
    if (vk_depthBoundsSupported) {
        ri.Printf(PRINT_ALL, "Vulkan: Depth bounds test supported\n");
    } else {
        ri.Printf(PRINT_ALL, "Vulkan: Depth bounds test NOT supported\n");
    }
    
    // Scissor test is always supported in Vulkan
    ri.Printf(PRINT_ALL, "Vulkan: Scissor test supported\n");
}

/*
================
VK_SetScissorRect

Set the scissor rectangle for the current command buffer
================
*/
void VK_SetScissorRect(const scissorRect_t *scissor) {
    VkCommandBuffer cmd;
    VkRect2D vkScissor;
    
    if (!scissor) {
        // Disable scissor by setting to full viewport
        VK_ResetScissor();
        return;
    }
    
    // Convert scissorRect_t to VkRect2D
    vkScissor.offset.x = scissor->x;
    vkScissor.offset.y = scissor->y;
    vkScissor.extent.width = scissor->width;
    vkScissor.extent.height = scissor->height;
    
    // Validate scissor bounds
    if (vkScissor.extent.width <= 0 || vkScissor.extent.height <= 0) {
        // Invalid scissor, reset to full viewport
        VK_ResetScissor();
        return;
    }
    
    // Get current command buffer
    cmd = vk.cmd ? vk.cmd->command_buffer : VK_NULL_HANDLE;
    if (!cmd) {
        ri.Printf(PRINT_WARNING, "VK_SetScissorRect: No active command buffer\n");
        return;
    }
    
    // Check if scissor changed
    if (scissorEnabled &&
        currentScissor.offset.x == vkScissor.offset.x &&
        currentScissor.offset.y == vkScissor.offset.y &&
        currentScissor.extent.width == vkScissor.extent.width &&
        currentScissor.extent.height == vkScissor.extent.height) {
        // Scissor unchanged
        return;
    }
    
    // Set the scissor rectangle
    vkCmdSetScissor(cmd, 0, 1, &vkScissor);
    
    // Update state
    currentScissor = vkScissor;
    scissorEnabled = qtrue;
    
    // Set depth bounds if supported and specified
    if (vk_depthBoundsSupported && r_depthBoundsTest->integer) {
        VK_SetDepthBounds(scissor->depthMin, scissor->depthMax);
    }
}

/*
================
VK_SetDepthBounds

Set depth bounds test range
================
*/
void VK_SetDepthBounds(float minDepth, float maxDepth) {
    VkCommandBuffer cmd;
    
    if (!vk_depthBoundsSupported) {
        return;
    }
    
    // Validate depth bounds
    if (minDepth < 0.0f) minDepth = 0.0f;
    if (minDepth > 1.0f) minDepth = 1.0f;
    if (maxDepth < 0.0f) maxDepth = 0.0f;
    if (maxDepth > 1.0f) maxDepth = 1.0f;
    
    if (minDepth >= maxDepth) {
        // Invalid depth bounds
        VK_DisableDepthBounds();
        return;
    }
    
    // Get current command buffer
    cmd = vk.cmd ? vk.cmd->command_buffer : VK_NULL_HANDLE;
    if (!cmd) {
        ri.Printf(PRINT_WARNING, "VK_SetDepthBounds: No active command buffer\n");
        return;
    }
    
    // Check if depth bounds changed
    if (depthBoundsEnabled &&
        currentDepthBounds[0] == minDepth &&
        currentDepthBounds[1] == maxDepth) {
        // Depth bounds unchanged
        return;
    }
    
    // Set depth bounds test range
    vkCmdSetDepthBounds(cmd, minDepth, maxDepth);
    
    // Update state
    currentDepthBounds[0] = minDepth;
    currentDepthBounds[1] = maxDepth;
    depthBoundsEnabled = qtrue;
}

/*
================
VK_DisableDepthBounds

Disable depth bounds test
================
*/
static void VK_DisableDepthBounds(void) {
    VkCommandBuffer cmd;
    
    if (!vk_depthBoundsSupported || !depthBoundsEnabled) {
        return;
    }
    
    // Get current command buffer
    cmd = vk.cmd ? vk.cmd->command_buffer : VK_NULL_HANDLE;
    if (!cmd) {
        return;
    }
    
    // Set full depth range to effectively disable
    vkCmdSetDepthBounds(cmd, 0.0f, 1.0f);
    
    depthBoundsEnabled = qfalse;
}

/*
================
VK_ResetScissor

Reset scissor to full viewport
================
*/
static void VK_ResetScissor(void) {
    VkCommandBuffer cmd;
    VkRect2D fullScissor;
    
    // Get current command buffer
    cmd = vk.cmd ? vk.cmd->command_buffer : VK_NULL_HANDLE;
    if (!cmd) {
        return;
    }
    
    // Set scissor to full viewport
    fullScissor.offset.x = 0;
    fullScissor.offset.y = 0;
    fullScissor.extent.width = glConfig.vidWidth;
    fullScissor.extent.height = glConfig.vidHeight;
    
    vkCmdSetScissor(cmd, 0, 1, &fullScissor);
    
    // Disable depth bounds
    VK_DisableDepthBounds();
    
    // Update state
    currentScissor = fullScissor;
    scissorEnabled = qfalse;
}

/*
================
VK_SetLightScissor

Set scissor rectangle for a light
================
*/
void VK_SetLightScissor(renderLight_t *light) {
    scissorRect_t scissor;
    
    if (!light || !r_lightScissoring->integer) {
        VK_ResetScissor();
        return;
    }
    
    // Convert light scissor to scissorRect_t
    scissor.x = light->scissorRect[0];
    scissor.y = light->scissorRect[1];
    scissor.width = light->scissorRect[2];
    scissor.height = light->scissorRect[3];
    scissor.depthMin = 0.0f;
    scissor.depthMax = 1.0f;
    
    // Apply scissor
    VK_SetScissorRect(&scissor);
}

/*
================
VK_SetInteractionScissor

Set scissor rectangle for an interaction
================
*/
void VK_SetInteractionScissor(interaction_t *inter) {
    scissorRect_t scissor;
    
    if (!inter || !r_lightScissoring->integer) {
        return;
    }
    
    // Check scissor level
    if (R_GetScissorLevel() < SCISSOR_INTERACTION) {
        return;
    }
    
    // Convert interaction scissor to scissorRect_t
    scissor.x = inter->scissorRect[0];
    scissor.y = inter->scissorRect[1];
    scissor.width = inter->scissorRect[2];
    scissor.height = inter->scissorRect[3];
    scissor.depthMin = inter->depthBounds[0];
    scissor.depthMax = inter->depthBounds[1];
    
    // Apply scissor
    VK_SetScissorRect(&scissor);
}

/*
================
VK_BeginScissorStats

Begin collecting scissor statistics
================
*/
void VK_BeginScissorStats(void) {
    R_ClearScissorStats();
}

/*
================
VK_EndScissorStats

End collecting and print scissor statistics
================
*/
void VK_EndScissorStats(void) {
    scissorStats_t *stats = R_GetScissorStats();
    
    if (r_scissorStats->integer) {
        R_PrintScissorStats(stats);
    }
}

/*
================
VK_CreateScissorPipelineState

Create pipeline state with scissor test enabled
Note: This would be integrated into the existing pipeline creation
================
*/
VkPipelineViewportStateCreateInfo VK_CreateScissorPipelineState(qboolean enableScissor) {
    static VkViewport viewport;
    static VkRect2D scissor;
    static VkPipelineViewportStateCreateInfo viewportState = {0};
    
    // Setup viewport
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)glConfig.vidWidth;
    viewport.height = (float)glConfig.vidHeight;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    // Setup default scissor (full viewport)
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = glConfig.vidWidth;
    scissor.extent.height = glConfig.vidHeight;
    
    // Create viewport state
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = enableScissor ? NULL : &scissor;  // NULL = dynamic scissor
    
    return viewportState;
}

/*
================
VK_EnableDynamicScissor

Enable dynamic scissor state in pipeline
Note: This would be added to pipeline dynamic state
================
*/
void VK_EnableDynamicScissor(VkPipelineDynamicStateCreateInfo *dynamicState,
                             VkDynamicState *dynamicStates, uint32_t *dynamicStateCount) {
    if (!dynamicState || !dynamicStates || !dynamicStateCount) {
        return;
    }
    
    // Add scissor to dynamic states
    dynamicStates[(*dynamicStateCount)++] = VK_DYNAMIC_STATE_SCISSOR;
    
    // Add depth bounds if supported
    if (vk_depthBoundsSupported && r_depthBoundsTest->integer) {
        dynamicStates[(*dynamicStateCount)++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS;
    }
}

/*
================
VK_GetDepthBoundsSupport

Check if depth bounds test is supported
================
*/
qboolean VK_GetDepthBoundsSupport(void) {
    return vk_depthBoundsSupported;
}

/*
================
VK_DrawScissorDebug

Draw debug visualization for scissor rectangles
================
*/
void VK_DrawScissorDebug(void) {
    if (!r_scissorDebug->integer) {
        return;
    }
    
    // This would require a 2D overlay rendering system
    // Drawing colored rectangles to visualize scissor bounds
    // Implementation depends on existing 2D rendering infrastructure
    
    R_DrawScissorRects();
}