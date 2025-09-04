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
// vk_staging.c - Vulkan staging buffer management

#include "vk.h"

/*
================
vk_push_staging_buffer

Push data to staging buffer for GPU upload
================
*/
void vk_push_staging_buffer( VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void *data ) {
    VkBufferMemoryBarrier barrier;
    VkCommandBuffer cmd;
    
    if ( !data || size == 0 ) {
        return;
    }
    
    // Get current command buffer
    cmd = vk.cmd->command_buffer;
    
    // Copy data to staging buffer
    if ( vk.staging_buffer.handle && vk.staging_buffer.memory ) {
        void *mapped;
        if ( vkMapMemory( vk.device, vk.staging_buffer.memory, 0, size, 0, &mapped ) == VK_SUCCESS ) {
            memcpy( mapped, data, size );
            vkUnmapMemory( vk.device, vk.staging_buffer.memory );
            
            // Copy from staging to destination
            VkBufferCopy copyRegion = {0};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = offset;
            copyRegion.size = size;
            
            vkCmdCopyBuffer( cmd, vk.staging_buffer.handle, buffer, 1, &copyRegion );
            
            // Add barrier to ensure copy completes
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.pNext = NULL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer;
            barrier.offset = offset;
            barrier.size = size;
            
            vkCmdPipelineBarrier( cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                0, 0, NULL, 1, &barrier, 0, NULL );
        }
    }
}

/*
================
vk_begin_command_buffer

Begin recording commands into a command buffer
================
*/
void vk_begin_command_buffer( VkCommandBuffer commandBuffer ) {
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    VK_CHECK( vkBeginCommandBuffer( commandBuffer, &beginInfo ) );
}

/*
================
vk_end_command_buffer

End recording commands into a command buffer
================
*/
void vk_end_command_buffer( VkCommandBuffer commandBuffer ) {
    VK_CHECK( vkEndCommandBuffer( commandBuffer ) );
}