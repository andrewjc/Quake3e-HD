/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Pure Vulkan RTX Hardware Raytracing Implementation
Vulkan Ray Tracing extensions only - no DirectX or OpenGL
===========================================================================
*/

#include "rt_rtx.h"
#include "rt_pathtracer.h"
#include "../tr_local.h"
#include "../vulkan/vk.h"

// External RTX state
extern rtxState_t rtx;

// ============================================================================
// Vulkan Ray Tracing Function Pointers
// ============================================================================

// Define function pointers for RT extensions
static PFN_vkCreateAccelerationStructureKHR qvkCreateAccelerationStructureKHR;
static PFN_vkDestroyAccelerationStructureKHR qvkDestroyAccelerationStructureKHR;
static PFN_vkGetAccelerationStructureBuildSizesKHR qvkGetAccelerationStructureBuildSizesKHR;
static PFN_vkCmdBuildAccelerationStructuresKHR qvkCmdBuildAccelerationStructuresKHR;
static PFN_vkGetAccelerationStructureDeviceAddressKHR qvkGetAccelerationStructureDeviceAddressKHR;
static PFN_vkCmdTraceRaysKHR qvkCmdTraceRaysKHR;

// ============================================================================
// Vulkan Ray Tracing Implementation
// ============================================================================

typedef struct vkrtState_s {
    VkDevice                        device;
    VkPhysicalDevice                physicalDevice;
    VkCommandPool                   commandPool;
    VkCommandBuffer                 commandBuffer;
    
    // Ray tracing pipeline
    VkPipeline                      rtPipeline;
    VkPipelineLayout                pipelineLayout;
    
    // Shader binding table
    VkBuffer                        raygenSBT;
    VkBuffer                        missSBT;
    VkBuffer                        hitSBT;
    VkDeviceMemory                  sbtMemory;
    
    // Acceleration structures
    VkAccelerationStructureKHR      tlas;
    VkBuffer                        tlasBuffer;
    VkDeviceMemory                  tlasMemory;
    
    // BLAS instances
    VkBuffer                        instanceBuffer;
    VkDeviceMemory                  instanceMemory;
    
    // Output image
    VkImage                         rtImage;
    VkImageView                     rtImageView;
    VkDeviceMemory                  rtImageMemory;
    
    // Synchronization
    VkFence                         fence;
    VkSemaphore                     semaphore;
    
    // Ray tracing properties
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties;
} vkrtState_t;

static vkrtState_t vkrt;

/*
================
RTX_CheckVulkanRTSupport

Check if Vulkan RT extensions are available
================
*/
static qboolean RTX_CheckVulkanRTSupport(void) {
    // Check for required extensions
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(vkrt.physicalDevice, NULL, &extensionCount, NULL);
    
    if (extensionCount == 0) {
        return qfalse;
    }
    
    VkExtensionProperties *extensions = Z_Malloc(sizeof(VkExtensionProperties) * extensionCount);
    vkEnumerateDeviceExtensionProperties(vkrt.physicalDevice, NULL, &extensionCount, extensions);
    
    qboolean hasRayTracing = qfalse;
    qboolean hasAccelStruct = qfalse;
    qboolean hasRayQuery = qfalse;
    
    for (uint32_t i = 0; i < extensionCount; i++) {
        if (!strcmp(extensions[i].extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
            hasRayTracing = qtrue;
        }
        if (!strcmp(extensions[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
            hasAccelStruct = qtrue;
        }
        if (!strcmp(extensions[i].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME)) {
            hasRayQuery = qtrue;
        }
    }
    
    Z_Free(extensions);
    
    if (!hasRayTracing || !hasAccelStruct) {
        ri.Printf(PRINT_WARNING, "RTX: Required Vulkan RT extensions not available\n");
        ri.Printf(PRINT_WARNING, "RTX: Ray Tracing: %s, Accel Struct: %s, Ray Query: %s\n",
                  hasRayTracing ? "YES" : "NO",
                  hasAccelStruct ? "YES" : "NO", 
                  hasRayQuery ? "YES" : "NO");
        return qfalse;
    }
    
    ri.Printf(PRINT_ALL, "RTX: Vulkan RT extensions detected\n");
    return qtrue;
}

/*
================
RTX_InitVulkanRT

Initialize Vulkan Ray Tracing
================
*/
qboolean RTX_InitVulkanRT(void) {
    // Check if we're using Vulkan renderer
    if (!vk.device || !vk.physical_device) {
        ri.Printf(PRINT_WARNING, "RTX: Vulkan renderer not active\n");
        return qfalse;
    }
    
    // Use the existing Vulkan device
    vkrt.device = vk.device;
    vkrt.physicalDevice = vk.physical_device;
    
    // Load RT extension functions
    qvkCreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR)
        vkGetDeviceProcAddr(vkrt.device, "vkCreateAccelerationStructureKHR");
    qvkDestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR)
        vkGetDeviceProcAddr(vkrt.device, "vkDestroyAccelerationStructureKHR");
    qvkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)
        vkGetDeviceProcAddr(vkrt.device, "vkGetAccelerationStructureBuildSizesKHR");
    qvkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)
        vkGetDeviceProcAddr(vkrt.device, "vkCmdBuildAccelerationStructuresKHR");
    qvkGetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR)
        vkGetDeviceProcAddr(vkrt.device, "vkGetAccelerationStructureDeviceAddressKHR");
    qvkCmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)
        vkGetDeviceProcAddr(vkrt.device, "vkCmdTraceRaysKHR");
    
    // Check for RT support
    if (!RTX_CheckVulkanRTSupport()) {
        return qfalse;
    }
    
    // Verify function pointers loaded
    if (!qvkCreateAccelerationStructureKHR || !qvkDestroyAccelerationStructureKHR ||
        !qvkGetAccelerationStructureBuildSizesKHR || !qvkCmdBuildAccelerationStructuresKHR ||
        !qvkGetAccelerationStructureDeviceAddressKHR || !qvkCmdTraceRaysKHR) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to load RT extension functions\n");
        return qfalse;
    }
    
    // Get RT properties
    vkrt.rtProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    vkrt.asProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    vkrt.rtProperties.pNext = &vkrt.asProperties;
    
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &vkrt.rtProperties
    };
    
    vkGetPhysicalDeviceProperties2(vkrt.physicalDevice, &props2);
    
    ri.Printf(PRINT_ALL, "RTX: Max ray recursion depth: %d\n", vkrt.rtProperties.maxRayRecursionDepth);
    ri.Printf(PRINT_ALL, "RTX: Max primitive count: %llu\n", vkrt.asProperties.maxPrimitiveCount);
    ri.Printf(PRINT_ALL, "RTX: Max instance count: %llu\n", vkrt.asProperties.maxInstanceCount);
    
    // Create command pool for RT commands
    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk.queue_family_index
    };
    
    VkResult result = vkCreateCommandPool(vkrt.device, &poolInfo, NULL, &vkrt.commandPool);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create command pool\n");
        return qfalse;
    }
    
    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vkrt.commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    
    result = vkAllocateCommandBuffers(vkrt.device, &allocInfo, &vkrt.commandBuffer);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to allocate command buffer\n");
        vkDestroyCommandPool(vkrt.device, vkrt.commandPool, NULL);
        return qfalse;
    }
    
    // Create synchronization objects
    VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    
    result = vkCreateFence(vkrt.device, &fenceInfo, NULL, &vkrt.fence);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create fence\n");
        RTX_ShutdownVulkanRT();
        return qfalse;
    }
    
    VkSemaphoreCreateInfo semaphoreInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    
    result = vkCreateSemaphore(vkrt.device, &semaphoreInfo, NULL, &vkrt.semaphore);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create semaphore\n");
        RTX_ShutdownVulkanRT();
        return qfalse;
    }
    
    ri.Printf(PRINT_ALL, "RTX: Vulkan Ray Tracing initialized successfully\n");
    return qtrue;
}

/*
================
RTX_ShutdownVulkanRT

Cleanup Vulkan RT resources
================
*/
void RTX_ShutdownVulkanRT(void) {
    if (!vkrt.device) {
        return;
    }
    
    // Wait for device to idle
    vkDeviceWaitIdle(vkrt.device);
    
    // Destroy RT resources
    if (vkrt.tlas) {
        qvkDestroyAccelerationStructureKHR(vkrt.device, vkrt.tlas, NULL);
    }
    if (vkrt.tlasBuffer) {
        vkDestroyBuffer(vkrt.device, vkrt.tlasBuffer, NULL);
    }
    if (vkrt.tlasMemory) {
        vkFreeMemory(vkrt.device, vkrt.tlasMemory, NULL);
    }
    
    if (vkrt.instanceBuffer) {
        vkDestroyBuffer(vkrt.device, vkrt.instanceBuffer, NULL);
    }
    if (vkrt.instanceMemory) {
        vkFreeMemory(vkrt.device, vkrt.instanceMemory, NULL);
    }
    
    if (vkrt.rtPipeline) {
        vkDestroyPipeline(vkrt.device, vkrt.rtPipeline, NULL);
    }
    if (vkrt.pipelineLayout) {
        vkDestroyPipelineLayout(vkrt.device, vkrt.pipelineLayout, NULL);
    }
    
    if (vkrt.semaphore) {
        vkDestroySemaphore(vkrt.device, vkrt.semaphore, NULL);
    }
    if (vkrt.fence) {
        vkDestroyFence(vkrt.device, vkrt.fence, NULL);
    }
    
    if (vkrt.commandPool) {
        vkDestroyCommandPool(vkrt.device, vkrt.commandPool, NULL);
    }
    
    Com_Memset(&vkrt, 0, sizeof(vkrt));
    ri.Printf(PRINT_ALL, "RTX: Vulkan RT shutdown complete\n");
}

/*
================
RTX_CreateBLASVulkan

Internal function to create Vulkan BLAS
================
*/
static VkAccelerationStructureKHR RTX_CreateBLASVulkan(const VkAccelerationStructureGeometryKHR *geometry,
                                                       const VkAccelerationStructureBuildRangeInfoKHR *range) {
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = geometry
    };
    
    // Get required sizes
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };
    
    uint32_t primitiveCount = range->primitiveCount;
    qvkGetAccelerationStructureBuildSizesKHR(vkrt.device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &primitiveCount, &sizeInfo);
    
    // Create buffer for AS
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeInfo.accelerationStructureSize,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };
    
    VkBuffer blasBuffer;
    VkDeviceMemory blasMemory;
    
    if (vkCreateBuffer(vkrt.device, &bufferInfo, NULL, &blasBuffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    
    // Allocate memory
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vkrt.device, blasBuffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = 0  // Find appropriate memory type
    };
    
    if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, &blasMemory) != VK_SUCCESS) {
        vkDestroyBuffer(vkrt.device, blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    vkBindBufferMemory(vkrt.device, blasBuffer, blasMemory, 0);
    
    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = blasBuffer,
        .size = sizeInfo.accelerationStructureSize,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
    };
    
    VkAccelerationStructureKHR blas;
    if (qvkCreateAccelerationStructureKHR(vkrt.device, &createInfo, NULL, &blas) != VK_SUCCESS) {
        vkFreeMemory(vkrt.device, blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    // Build the BLAS
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    
    vkBeginCommandBuffer(vkrt.commandBuffer, &beginInfo);
    
    buildInfo.dstAccelerationStructure = blas;
    
    const VkAccelerationStructureBuildRangeInfoKHR *rangeInfos[] = { range };
    qvkCmdBuildAccelerationStructuresKHR(vkrt.commandBuffer, 1, &buildInfo, rangeInfos);
    
    vkEndCommandBuffer(vkrt.commandBuffer);
    
    // Submit and wait
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &vkrt.commandBuffer
    };
    
    vkQueueSubmit(vk.queue, 1, &submitInfo, vkrt.fence);
    vkWaitForFences(vkrt.device, 1, &vkrt.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(vkrt.device, 1, &vkrt.fence);
    
    return blas;
}

/*
================
RTX_BuildAccelerationStructureVK

Build TLAS from BLAS instances
================
*/
void RTX_BuildAccelerationStructureVK(void) {
    if (!vkrt.device || rtx.tlas.numInstances == 0) {
        return;
    }
    
    // Build instance data
    VkAccelerationStructureInstanceKHR *instances = Z_Malloc(
        sizeof(VkAccelerationStructureInstanceKHR) * rtx.tlas.numInstances);
    
    for (int i = 0; i < rtx.tlas.numInstances; i++) {
        rtxInstance_t *inst = &rtx.tlas.instances[i];
        VkAccelerationStructureInstanceKHR *vkInst = &instances[i];
        
        // Copy transform matrix
        Com_Memcpy(vkInst->transform.matrix, inst->transform, sizeof(float) * 12);
        
        vkInst->instanceCustomIndex = i;
        vkInst->mask = 0xFF;
        vkInst->instanceShaderBindingTableRecordOffset = 0;
        vkInst->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        
        // Get BLAS device address
        if (inst->blas && inst->blas->handle) {
            VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = (VkAccelerationStructureKHR)inst->blas->handle
            };
            vkInst->accelerationStructureReference = 
                qvkGetAccelerationStructureDeviceAddressKHR(vkrt.device, &addressInfo);
        }
    }
    
    // Upload instance data
    // TODO: Create instance buffer and upload data
    
    Z_Free(instances);
    
    // Build TLAS
    // TODO: Create and build TLAS
    
    rtx.buildTime = 0;  // TODO: Measure build time
}

/*
================
RTX_DispatchRaysVK

Dispatch ray tracing
================
*/
void RTX_DispatchRaysVK(const rtxDispatchRays_t *params) {
    if (!vkrt.device || !vkrt.rtPipeline) {
        return;
    }
    
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    
    vkBeginCommandBuffer(vkrt.commandBuffer, &beginInfo);
    
    // Bind ray tracing pipeline
    vkCmdBindPipeline(vkrt.commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkrt.rtPipeline);
    
    // Dispatch rays
    VkStridedDeviceAddressRegionKHR raygenRegion = {0};
    VkStridedDeviceAddressRegionKHR missRegion = {0};
    VkStridedDeviceAddressRegionKHR hitRegion = {0};
    VkStridedDeviceAddressRegionKHR callableRegion = {0};
    
    qvkCmdTraceRaysKHR(vkrt.commandBuffer,
                       &raygenRegion, &missRegion, &hitRegion, &callableRegion,
                       params->width, params->height, 1);
    
    vkEndCommandBuffer(vkrt.commandBuffer);
    
    // Submit
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &vkrt.commandBuffer
    };
    
    vkQueueSubmit(vk.queue, 1, &submitInfo, vkrt.fence);
    vkWaitForFences(vkrt.device, 1, &vkrt.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(vkrt.device, 1, &vkrt.fence);
    
    rtx.traceTime = 0;  // TODO: Measure trace time
}

// Denoiser and DLSS implementations are in separate files:
// - rt_rtx_denoiser.c
// - rt_rtx_dlss.c