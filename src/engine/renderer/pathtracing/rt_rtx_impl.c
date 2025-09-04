/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Pure Vulkan RTX Hardware Raytracing Implementation
Vulkan Ray Tracing extensions only - no DirectX or OpenGL
===========================================================================
*/

#include "rt_rtx.h"
#include "rt_pathtracer.h"
#include "../core/tr_local.h"
#include "../vulkan/vk.h"

// External RTX state
extern rtxState_t rtx;

// ============================================================================
// Forward Declarations
// ============================================================================

static VkBuffer RTX_AllocateScratchBuffer(VkDeviceSize size, VkDeviceMemory *memory);

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
static PFN_vkGetBufferDeviceAddress qvkGetBufferDeviceAddress;

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
RTX_FindMemoryType

Find suitable memory type for allocation
================
*/
static uint32_t RTX_FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(vkrt.physicalDevice, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    ri.Printf(PRINT_WARNING, "RTX: Failed to find suitable memory type\n");
    return 0;
}

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
    qvkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)
        vkGetDeviceProcAddr(vkrt.device, "vkGetBufferDeviceAddress");
    if (!qvkGetBufferDeviceAddress) {
        // Try KHR version
        qvkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)
            vkGetDeviceProcAddr(vkrt.device, "vkGetBufferDeviceAddressKHR");
    }
    
    // Check for RT support
    if (!RTX_CheckVulkanRTSupport()) {
        return qfalse;
    }
    
    // Verify function pointers loaded
    if (!qvkCreateAccelerationStructureKHR || !qvkDestroyAccelerationStructureKHR ||
        !qvkGetAccelerationStructureBuildSizesKHR || !qvkCmdBuildAccelerationStructuresKHR ||
        !qvkGetAccelerationStructureDeviceAddressKHR || !qvkCmdTraceRaysKHR ||
        !qvkGetBufferDeviceAddress) {
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
    
    // Destroy SBT buffers
    if (vkrt.raygenSBT) {
        vkDestroyBuffer(vkrt.device, vkrt.raygenSBT, NULL);
    }
    if (vkrt.missSBT) {
        vkDestroyBuffer(vkrt.device, vkrt.missSBT, NULL);
    }
    if (vkrt.hitSBT) {
        vkDestroyBuffer(vkrt.device, vkrt.hitSBT, NULL);
    }
    if (vkrt.sbtMemory) {
        vkFreeMemory(vkrt.device, vkrt.sbtMemory, NULL);
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
                                                       const VkAccelerationStructureBuildRangeInfoKHR *range,
                                                       VkBuffer *blasBuffer, VkDeviceMemory *blasMemory) {
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
    
    if (vkCreateBuffer(vkrt.device, &bufferInfo, NULL, blasBuffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    
    // Allocate memory with device address support
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vkrt.device, *blasBuffer, &memReqs);
    
    VkMemoryAllocateFlagsInfo memoryAllocateFlagsInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &memoryAllocateFlagsInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = RTX_FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, blasMemory) != VK_SUCCESS) {
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    vkBindBufferMemory(vkrt.device, *blasBuffer, *blasMemory, 0);
    
    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = *blasBuffer,
        .size = sizeInfo.accelerationStructureSize,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
    };
    
    VkAccelerationStructureKHR blas;
    if (qvkCreateAccelerationStructureKHR(vkrt.device, &createInfo, NULL, &blas) != VK_SUCCESS) {
        vkFreeMemory(vkrt.device, *blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    // Allocate scratch buffer
    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    scratchBuffer = RTX_AllocateScratchBuffer(sizeInfo.buildScratchSize, &scratchMemory);
    
    if (!scratchBuffer) {
        qvkDestroyAccelerationStructureKHR(vkrt.device, blas, NULL);
        vkFreeMemory(vkrt.device, *blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    // Build the BLAS
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    
    vkBeginCommandBuffer(vkrt.commandBuffer, &beginInfo);
    
    buildInfo.dstAccelerationStructure = blas;
    buildInfo.scratchData.deviceAddress = RTX_GetBufferDeviceAddress(scratchBuffer);
    
    const VkAccelerationStructureBuildRangeInfoKHR *rangeInfos[] = { range };
    qvkCmdBuildAccelerationStructuresKHR(vkrt.commandBuffer, 1, &buildInfo, rangeInfos);
    
    // Add memory barrier for AS build
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
    };
    
    vkCmdPipelineBarrier(vkrt.commandBuffer,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 1, &barrier, 0, NULL, 0, NULL);
    
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
    
    // Clean up scratch buffer
    vkDestroyBuffer(vkrt.device, scratchBuffer, NULL);
    vkFreeMemory(vkrt.device, scratchMemory, NULL);
    
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
    
    float startTime = ri.Milliseconds();
    
    // Build instance data
    VkAccelerationStructureInstanceKHR *instances = Z_Malloc(
        sizeof(VkAccelerationStructureInstanceKHR) * rtx.tlas.numInstances);
    
    for (int i = 0; i < rtx.tlas.numInstances; i++) {
        rtxInstance_t *inst = &rtx.tlas.instances[i];
        VkAccelerationStructureInstanceKHR *vkInst = &instances[i];
        
        // Copy transform matrix (3x4 row-major)
        Com_Memcpy(vkInst->transform.matrix, inst->transform, sizeof(float) * 12);
        
        vkInst->instanceCustomIndex = i;
        vkInst->mask = inst->mask;
        vkInst->instanceShaderBindingTableRecordOffset = inst->shaderOffset;
        vkInst->flags = inst->flags | VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        
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
    
    // Create or update instance buffer
    size_t instanceDataSize = sizeof(VkAccelerationStructureInstanceKHR) * rtx.tlas.numInstances;
    
    if (!vkrt.instanceBuffer) {
        // Create instance buffer
        VkBufferCreateInfo bufferInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = instanceDataSize,
            .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT
        };
        
        if (vkCreateBuffer(vkrt.device, &bufferInfo, NULL, &vkrt.instanceBuffer) != VK_SUCCESS) {
            Z_Free(instances);
            return;
        }
        
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(vkrt.device, vkrt.instanceBuffer, &memReqs);
        
        VkMemoryAllocateFlagsInfo memoryAllocateFlagsInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
        };
        
        VkMemoryAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &memoryAllocateFlagsInfo,
            .allocationSize = memReqs.size,
            .memoryTypeIndex = RTX_FindMemoryType(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        };
        
        if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, &vkrt.instanceMemory) != VK_SUCCESS) {
            vkDestroyBuffer(vkrt.device, vkrt.instanceBuffer, NULL);
            vkrt.instanceBuffer = VK_NULL_HANDLE;
            Z_Free(instances);
            return;
        }
        
        vkBindBufferMemory(vkrt.device, vkrt.instanceBuffer, vkrt.instanceMemory, 0);
    }
    
    // Upload instance data
    void *data;
    vkMapMemory(vkrt.device, vkrt.instanceMemory, 0, instanceDataSize, 0, &data);
    Com_Memcpy(data, instances, instanceDataSize);
    vkUnmapMemory(vkrt.device, vkrt.instanceMemory);
    
    Z_Free(instances);
    
    // Setup TLAS geometry
    VkDeviceAddress instanceBufferAddress = RTX_GetBufferDeviceAddress(vkrt.instanceBuffer);
    
    VkAccelerationStructureGeometryKHR tlasGeometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .arrayOfPointers = VK_FALSE,
            .data.deviceAddress = instanceBufferAddress
        }
    };
    
    // Get TLAS build sizes
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode = vkrt.tlas ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR :
                           VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &tlasGeometry
    };
    
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };
    
    uint32_t instanceCount = rtx.tlas.numInstances;
    qvkGetAccelerationStructureBuildSizesKHR(vkrt.device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &instanceCount, &sizeInfo);
    
    // Create TLAS if needed
    if (!vkrt.tlas) {
        // Create TLAS buffer
        VkBufferCreateInfo bufferInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeInfo.accelerationStructureSize,
            .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        };
        
        if (vkCreateBuffer(vkrt.device, &bufferInfo, NULL, &vkrt.tlasBuffer) != VK_SUCCESS) {
            return;
        }
        
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(vkrt.device, vkrt.tlasBuffer, &memReqs);
        
        VkMemoryAllocateFlagsInfo memoryAllocateFlagsInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
        };
        
        VkMemoryAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &memoryAllocateFlagsInfo,
            .allocationSize = memReqs.size,
            .memoryTypeIndex = RTX_FindMemoryType(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        };
        
        if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, &vkrt.tlasMemory) != VK_SUCCESS) {
            vkDestroyBuffer(vkrt.device, vkrt.tlasBuffer, NULL);
            vkrt.tlasBuffer = VK_NULL_HANDLE;
            return;
        }
        
        vkBindBufferMemory(vkrt.device, vkrt.tlasBuffer, vkrt.tlasMemory, 0);
        
        // Create TLAS
        VkAccelerationStructureCreateInfoKHR createInfo = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = vkrt.tlasBuffer,
            .size = sizeInfo.accelerationStructureSize,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
        };
        
        if (qvkCreateAccelerationStructureKHR(vkrt.device, &createInfo, NULL, &vkrt.tlas) != VK_SUCCESS) {
            vkFreeMemory(vkrt.device, vkrt.tlasMemory, NULL);
            vkDestroyBuffer(vkrt.device, vkrt.tlasBuffer, NULL);
            vkrt.tlasBuffer = VK_NULL_HANDLE;
            vkrt.tlasMemory = VK_NULL_HANDLE;
            return;
        }
    }
    
    // Allocate scratch buffer
    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    scratchBuffer = RTX_AllocateScratchBuffer(sizeInfo.buildScratchSize, &scratchMemory);
    
    if (!scratchBuffer) {
        return;
    }
    
    // Build TLAS
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    
    vkBeginCommandBuffer(vkrt.commandBuffer, &beginInfo);
    
    // Build range info
    VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {
        .primitiveCount = rtx.tlas.numInstances,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0
    };
    
    buildInfo.dstAccelerationStructure = vkrt.tlas;
    buildInfo.scratchData.deviceAddress = RTX_GetBufferDeviceAddress(scratchBuffer);
    
    const VkAccelerationStructureBuildRangeInfoKHR *rangeInfos[] = { &rangeInfo };
    qvkCmdBuildAccelerationStructuresKHR(vkrt.commandBuffer, 1, &buildInfo, rangeInfos);
    
    // Add memory barrier
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
    };
    
    vkCmdPipelineBarrier(vkrt.commandBuffer,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 1, &barrier, 0, NULL, 0, NULL);
    
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
    
    // Clean up scratch buffer
    vkDestroyBuffer(vkrt.device, scratchBuffer, NULL);
    vkFreeMemory(vkrt.device, scratchMemory, NULL);
    
    rtx.buildTime = ri.Milliseconds() - startTime;
}

/*
================
RTX_DispatchRaysVK

Dispatch ray tracing with full pipeline state
================
*/
void RTX_DispatchRaysVK(const rtxDispatchRays_t *params) {
    if (!vkrt.device || !rtx.tlas.numInstances) {
        return;
    }
    
    float startTime = ri.Milliseconds();
    
    // Get pipeline and descriptor set from pipeline system
    VkPipeline rtPipeline = RTX_GetPipeline();
    VkPipelineLayout pipelineLayout = RTX_GetPipelineLayout();
    VkDescriptorSet descriptorSet = RTX_GetDescriptorSet();
    
    if (!rtPipeline || !pipelineLayout || !descriptorSet) {
        ri.Printf(PRINT_WARNING, "RTX: Pipeline not properly initialized\n");
        return;
    }
    
    // Update descriptor sets with current TLAS and output images
    RTX_UpdateDescriptorSets(vkrt.tlas, vkrt.rtImageView, vkrt.rtImageView,
                            vkrt.rtImageView, vkrt.rtImageView, vkrt.rtImageView);
    
    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    
    VkResult result = vkBeginCommandBuffer(vkrt.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to begin command buffer\n");
        return;
    }
    
    // Transition RT output image to general layout
    if (vkrt.rtImage) {
        VkImageMemoryBarrier imageBarrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vkrt.rtImage,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        
        vkCmdPipelineBarrier(vkrt.commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, NULL, 0, NULL, 1, &imageBarrier);
    }
    
    // Bind ray tracing pipeline
    vkCmdBindPipeline(vkrt.commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipeline);
    
    // Bind descriptor sets
    vkCmdBindDescriptorSets(vkrt.commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
    
    // Get shader binding table regions
    VkStridedDeviceAddressRegionKHR raygenRegion, missRegion, hitRegion, callableRegion;
    RTX_GetSBTRegions(&raygenRegion, &missRegion, &hitRegion, &callableRegion);
    
    // Dispatch rays
    qvkCmdTraceRaysKHR(vkrt.commandBuffer,
                       &raygenRegion, &missRegion, &hitRegion, &callableRegion,
                       params->width, params->height, 1);
    
    // Transition RT output image for transfer/presentation
    if (vkrt.rtImage) {
        VkImageMemoryBarrier imageBarrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vkrt.rtImage,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        
        vkCmdPipelineBarrier(vkrt.commandBuffer,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &imageBarrier);
    }
    
    vkEndCommandBuffer(vkrt.commandBuffer);
    
    // Submit command buffer
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &vkrt.commandBuffer
    };
    
    result = vkQueueSubmit(vk.queue, 1, &submitInfo, vkrt.fence);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to submit command buffer\n");
        return;
    }
    
    // Wait for completion
    vkWaitForFences(vkrt.device, 1, &vkrt.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(vkrt.device, 1, &vkrt.fence);
    
    rtx.traceTime = ri.Milliseconds() - startTime;
    
    if (rtx_debug && rtx_debug->integer) {
        ri.Printf(PRINT_ALL, "RTX: Ray dispatch completed in %.2fms (%dx%d)\n", 
                 rtx.traceTime, params->width, params->height);
    }
}

/*
================
RTX_AllocateScratchBuffer

Allocate scratch buffer for acceleration structure builds
================
*/
static VkBuffer RTX_AllocateScratchBuffer(VkDeviceSize size, VkDeviceMemory *memory) {
    VkBuffer buffer = VK_NULL_HANDLE;
    
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };
    
    if (vkCreateBuffer(vkrt.device, &bufferInfo, NULL, &buffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vkrt.device, buffer, &memReqs);
    
    VkMemoryAllocateFlagsInfo memoryAllocateFlagsInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &memoryAllocateFlagsInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits, 
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, memory) != VK_SUCCESS) {
        vkDestroyBuffer(vkrt.device, buffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    vkBindBufferMemory(vkrt.device, buffer, *memory, 0);
    return buffer;
}

/*
================
RTX_GetBufferDeviceAddress

Get device address of a buffer
================
*/
VkDeviceAddress RTX_GetBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer
    };
    if (!qvkGetBufferDeviceAddress) {
        ri.Printf(PRINT_WARNING, "RTX: vkGetBufferDeviceAddress not available\n");
        return 0;
    }
    return qvkGetBufferDeviceAddress(vkrt.device, &addressInfo);
}

/*
================
RTX_CreateRTOutputImages

Create output images for ray tracing
================
*/
static qboolean RTX_CreateRTOutputImages(uint32_t width, uint32_t height) {
    // Create main color output image
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    
    if (vkCreateImage(vkrt.device, &imageInfo, NULL, &vkrt.rtImage) != VK_SUCCESS) {
        return qfalse;
    }
    
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vkrt.device, vkrt.rtImage, &memReqs);
    
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, &vkrt.rtImageMemory) != VK_SUCCESS) {
        vkDestroyImage(vkrt.device, vkrt.rtImage, NULL);
        return qfalse;
    }
    
    vkBindImageMemory(vkrt.device, vkrt.rtImage, vkrt.rtImageMemory, 0);
    
    // Create image view
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = vkrt.rtImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    
    if (vkCreateImageView(vkrt.device, &viewInfo, NULL, &vkrt.rtImageView) != VK_SUCCESS) {
        vkFreeMemory(vkrt.device, vkrt.rtImageMemory, NULL);
        vkDestroyImage(vkrt.device, vkrt.rtImage, NULL);
        return qfalse;
    }
    
    return qtrue;
}

// Denoiser and DLSS implementations are in separate files:
// - rt_rtx_denoiser.c
// - rt_rtx_dlss.c