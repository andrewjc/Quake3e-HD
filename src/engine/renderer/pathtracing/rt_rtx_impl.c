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
#include <stdint.h>
#include <string.h>
#include <math.h>

#define RTX_DEBUG_BLAS_LIMIT -1

#if defined(_DEBUG)
#define RTX_DEBUG_LOG_CMD(action, buffer, tag) \
    do { \
        if ((buffer) != VK_NULL_HANDLE) { \
            const char *_tag = (tag) ? (tag) : ""; \
            ri.Printf(PRINT_DEVELOPER, "RTX-CMD %s %p %s\n", action, (void*)(buffer), _tag); \
        } \
    } while (0)
#else
#define RTX_DEBUG_LOG_CMD(action, buffer, tag) ((void)0)
#endif

// External RTX state
extern rtxState_t rtx;
extern cvar_t *r_rtx_gi_bounces;
extern cvar_t *r_rtx_debug;
extern cvar_t *rtx_debug_skip_present;
extern cvar_t *rtx_debug_force_readback;
extern cvar_t *rtx_debug_dispatch_scale;
extern cvar_t *rtx_debug_skip_trace;
extern cvar_t *rtx_debug_skip_all;

typedef struct rtxCopyBarrierDebug_s {
    VkPipelineStageFlags stageMask;
   VkAccessFlags accessMask;
   VkImageLayout oldLayout;
   VkImageLayout newLayout;
    VkImage targetImage;
    qboolean usingSwapchain;
} rtxCopyBarrierDebug_t;

static rtxCopyBarrierDebug_t rtx_copy_debug;

typedef struct rtxCopySupport_s {
    VkFormat sourceFormat;
    VkFormat targetFormat;
    qboolean valid;
    qboolean supported;
    qboolean requiresBlit;
    qboolean warned;
    char reason[128];
} rtxCopySupport_t;

static rtxCopySupport_t rtx_framebuffer_copy;

static uint32_t RTX_FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties);
static qboolean RTX_CreateRTOutputImages(uint32_t width, uint32_t height);
static void RTX_DestroyGBufferImage(VkImage *image, VkImageView *view,
                                    VkDeviceMemory *memory);

static void RTX_ResetFramebufferCopySupport(void) {
    Com_Memset(&rtx_framebuffer_copy, 0, sizeof(rtx_framebuffer_copy));
}

static void RTX_AssessFramebufferCopy(VkFormat sourceFormat, VkFormat targetFormat) {
    if (rtx_framebuffer_copy.valid &&
        rtx_framebuffer_copy.sourceFormat == sourceFormat &&
        rtx_framebuffer_copy.targetFormat == targetFormat) {
        return;
    }

    rtx_framebuffer_copy.valid = qtrue;
    rtx_framebuffer_copy.sourceFormat = sourceFormat;
    rtx_framebuffer_copy.targetFormat = targetFormat;
    rtx_framebuffer_copy.supported = qfalse;
    rtx_framebuffer_copy.requiresBlit = qfalse;
    rtx_framebuffer_copy.warned = qfalse;
    rtx_framebuffer_copy.reason[0] = '\0';

    if (!vk.physical_device) {
        Com_sprintf(rtx_framebuffer_copy.reason,
                    sizeof(rtx_framebuffer_copy.reason),
                    "no Vulkan physical device");
        return;
    }

    if (sourceFormat == VK_FORMAT_UNDEFINED || targetFormat == VK_FORMAT_UNDEFINED) {
        Com_sprintf(rtx_framebuffer_copy.reason,
                    sizeof(rtx_framebuffer_copy.reason),
                    "undefined format (src=%d dst=%d)",
                    (int)sourceFormat,
                    (int)targetFormat);
        return;
    }

    VkFormatProperties srcProps;
    VkFormatProperties dstProps;
    vkGetPhysicalDeviceFormatProperties(vk.physical_device, sourceFormat, &srcProps);
    vkGetPhysicalDeviceFormatProperties(vk.physical_device, targetFormat, &dstProps);

    VkFormatFeatureFlags srcFeatures = srcProps.optimalTilingFeatures;
    VkFormatFeatureFlags dstFeatures = dstProps.optimalTilingFeatures;

    if (sourceFormat == targetFormat) {
        qboolean srcTransfer = (srcFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
        qboolean dstTransfer = (dstFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
        if (!srcTransfer || !dstTransfer) {
            Com_sprintf(rtx_framebuffer_copy.reason,
                        sizeof(rtx_framebuffer_copy.reason),
                        "format %d lacks transfer support (src=0x%08X dst=0x%08X)",
                        (int)sourceFormat,
                        (unsigned int)srcFeatures,
                        (unsigned int)dstFeatures);
            return;
        }

        rtx_framebuffer_copy.supported = qtrue;
        rtx_framebuffer_copy.requiresBlit = qfalse;
        rtx_framebuffer_copy.reason[0] = '\0';
        return;
    }

    qboolean srcBlit = (srcFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
    qboolean dstBlit = (dstFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
    qboolean srcTransfer = (srcFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    qboolean dstTransfer = (dstFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;

    if (!srcBlit || !dstBlit) {
        Com_sprintf(rtx_framebuffer_copy.reason,
                    sizeof(rtx_framebuffer_copy.reason),
                    "formats %d -> %d missing blit support (src=0x%08X dst=0x%08X)",
                    (int)sourceFormat,
                    (int)targetFormat,
                    (unsigned int)srcFeatures,
                    (unsigned int)dstFeatures);
        return;
    }

    if (!srcTransfer || !dstTransfer) {
        Com_sprintf(rtx_framebuffer_copy.reason,
                    sizeof(rtx_framebuffer_copy.reason),
                    "formats %d -> %d missing transfer usage (src=0x%08X dst=0x%08X)",
                    (int)sourceFormat,
                    (int)targetFormat,
                    (unsigned int)srcFeatures,
                    (unsigned int)dstFeatures);
        return;
    }

    rtx_framebuffer_copy.supported = qtrue;
    rtx_framebuffer_copy.requiresBlit = qtrue;
    rtx_framebuffer_copy.reason[0] = '\0';
}

qboolean RTX_FramebufferCopySupported(VkFormat sourceFormat,
                                      VkFormat targetFormat,
                                      const char *contextLabel,
                                      qboolean logWarning) {
    RTX_AssessFramebufferCopy(sourceFormat, targetFormat);
    if (!rtx_framebuffer_copy.supported) {
        if (logWarning && !rtx_framebuffer_copy.warned) {
            const char *reason = rtx_framebuffer_copy.reason[0]
                ? rtx_framebuffer_copy.reason
                : "unsupported format combination";
            if (contextLabel && contextLabel[0]) {
                ri.Printf(PRINT_WARNING,
                          "RTX: %s skipped; %s\n",
                          contextLabel,
                          reason);
            } else {
                ri.Printf(PRINT_WARNING,
                          "RTX: Skipping framebuffer copy; %s\n",
                          reason);
            }
            rtx_framebuffer_copy.warned = qtrue;
        }
        return qfalse;
    }
    return qtrue;
}

// ============================================================================
// Forward Declarations
// ============================================================================

static VkBuffer RTX_AllocateScratchBuffer(VkDeviceSize size, VkDeviceMemory *memory);
static void RTX_LogCapabilitySummary(void);
static void RTX_DetectGPUVendor(const VkPhysicalDeviceProperties *props, const char **outVendorLabel);
static const char *RTX_VendorLabel(rtxGpuType_t type);
static void RTX_SetNVIDIAArchitecture(const char *deviceName);
static void RTX_SetAMDArchitecture(const char *deviceName);
static void RTX_SetIntelArchitecture(const char *deviceName);
static qboolean RTX_EnsureReadbackBuffer(VkDeviceSize size);
static void RTX_DestroyReadbackBuffer(void);
static qboolean RTX_DownloadColorBuffer(uint32_t width, uint32_t height);

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
    qboolean                        fenceSubmitted;
    
    // Ray tracing pipeline
    VkPipeline                      rtPipeline;
    VkPipelineLayout                pipelineLayout;

    // Debug overlay compute pipeline
    VkPipeline                      debugOverlayPipeline;
    VkPipelineLayout                debugOverlayPipelineLayout;
    VkDescriptorSetLayout           debugOverlaySetLayout;
    VkDescriptorPool                debugOverlayDescriptorPool;
    VkDescriptorSet                 debugOverlayDescriptorSet;
    VkSampler                       debugOverlaySampler;

    // Depth-aware raster/traced composite compute pipeline
    VkPipeline                      compositePipeline;
    VkPipelineLayout                compositePipelineLayout;
    VkDescriptorSetLayout           compositeSetLayout;
    VkDescriptorPool                compositeDescriptorPool;
    VkDescriptorSet                 compositeDescriptorSet;
    VkSampler                       compositeSampler;
    VkImageView                     compositeBoundTracedColor;
    VkImageView                     compositeBoundTracedDepth;
    VkImageView                     compositeBoundRasterColor;
    VkImageView                     compositeBoundRasterDepth;
    VkImageView                     compositeBoundMedia;

    // Edge-aware à-trous despeckle pipeline (ping-pongs with rtImage)
    VkPipeline                      denoisePipeline;
    VkPipelineLayout                denoisePipelineLayout;
    VkDescriptorSetLayout           denoiseSetLayout;
    VkDescriptorPool                denoiseDescriptorPool;
    VkDescriptorSet                 denoiseDescriptorSet;
    VkImageView                     denoiseBoundColor;
    VkImageView                     denoiseBoundPing;
    VkImage                         denoisePingImage;
    VkImageView                     denoisePingImageView;
    VkDeviceMemory                  denoisePingImageMemory;

    // Volumetric media along the primary ray (in-scatter rgb, transmittance
    // a): written by raygen, applied to raster pixels by the composite
    VkImage                         mediaImage;
    VkImageView                     mediaImageView;
    VkDeviceMemory                  mediaImageMemory;

    // Temporal accumulation pipeline with ping-ponged history pairs
    VkPipeline                      temporalPipeline;
    VkPipelineLayout                temporalPipelineLayout;
    VkDescriptorSetLayout           temporalSetLayout;
    VkDescriptorPool                temporalDescriptorPool;
    VkDescriptorSet                 temporalDescriptorSet;
    VkImageView                     temporalBoundColor;
    uint32_t                        temporalParity;
    uint32_t                        temporalFramesSinceReset;
    VkImage                         historyIllumImage[2];
    VkImageView                     historyIllumImageView[2];
    VkDeviceMemory                  historyIllumImageMemory[2];
    VkImage                         historyGeomImage[2];
    VkImageView                     historyGeomImageView[2];
    VkDeviceMemory                  historyGeomImageMemory[2];

    // Shader binding table
    VkBuffer                        raygenSBT;
    VkBuffer                        missSBT;
    VkBuffer                        hitSBT;
    VkDeviceMemory                  sbtMemory;
    
    // Acceleration structures
    VkAccelerationStructureKHR      tlas[2];
    VkBuffer                        tlasBuffer[2];
    VkDeviceMemory                  tlasMemory[2];
    int                             activeTLAS;
    
    // BLAS instances
    VkBuffer                        instanceBuffer;
    VkDeviceMemory                  instanceMemory;
    
    // Persistent scratch buffer for AS builds
    VkBuffer                        scratchBuffer;
    VkDeviceMemory                  scratchMemory;
    VkDeviceSize                    scratchSize;
    
    VkDeviceAddress                 lastScratchAddr;
    VkDeviceSize                    lastScratchSize;
    
    // Output image
    VkImage                         rtImage;
    VkImageView                     rtImageView;
    VkDeviceMemory                  rtImageMemory;
    VkFormat                        rtImageFormat;
    VkBuffer                        readbackBuffer;
    VkDeviceMemory                  readbackMemory;
    void                           *readbackMapped;
    VkDeviceSize                   readbackSize;

    // G-buffer images (albedo, normal, motion vectors, depth)
    VkImage                         albedoImage;
    VkImageView                     albedoImageView;
    VkDeviceMemory                  albedoImageMemory;
    VkImage                         normalImage;
    VkImageView                     normalImageView;
    VkDeviceMemory                  normalImageMemory;
    VkImage                         motionImage;
    VkImageView                     motionImageView;
    VkDeviceMemory                  motionImageMemory;
    VkImage                         depthImage;
    VkImageView                     depthImageView;
    VkDeviceMemory                  depthImageMemory;

    // Lighting contribution images (direct and indirect)
    VkImage                         directLightImage;
    VkImageView                     directLightImageView;
    VkDeviceMemory                  directLightImageMemory;
    VkImage                         indirectLightImage;
    VkImageView                     indirectLightImageView;
    VkDeviceMemory                  indirectLightImageMemory;
    
    // Synchronization
    VkFence                         fence;
    VkSemaphore                     semaphore;
    
    // Ray tracing properties
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties;
    VkPhysicalDeviceProperties    deviceProps;

    // Capability flags
    qboolean                    hasRayTracingPipeline;
    qboolean                    hasAccelerationStructure;
    qboolean                    hasRayQuery;
    qboolean                    hasDeferredHostOps;
    qboolean                    hasRTMaintenance1;
    qboolean                    deviceLost;
} vkrtState_t;

static vkrtState_t vkrt;
static uint32_t rtOutputWidth;
static uint32_t rtOutputHeight;
static qboolean rtOutputInitialized;

static void RTX_OnDeviceLost(const char *context) {
    if (!vkrt.deviceLost) {
        vkrt.deviceLost = qtrue;
        vkrt.fenceSubmitted = qfalse;
        rtx.available = qfalse;
        RTX_ResetFramebufferCopySupport();
        ri.Printf(PRINT_ERROR, "RTX: Vulkan device lost during %s; disabling RTX backend\n",
                  context ? context : "unknown operation");
        RTX_DebugLogLiveBuffers(context ? context : "device lost");
    }
}

void RTX_HandleDeviceLoss(const char *context) {
    RTX_OnDeviceLost(context);
}

static const char *RTX_LogLabel(const char *label) {
    return label ? label : "RTX-Immediate";
}

static VkResult RTX_BeginImmediateCommands(const char *label) {
    if (!vkrt.device || vkrt.commandBuffer == VK_NULL_HANDLE || vkrt.fence == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (vkrt.deviceLost) {
        return VK_ERROR_DEVICE_LOST;
    }

    if (vkrt.fenceSubmitted) {
        // 2-second timeout prevents the game from freezing if the GPU hangs
        VkResult waitRes = vkWaitForFences(vkrt.device, 1, &vkrt.fence, VK_TRUE, 2000000000ULL);
        if (waitRes == VK_TIMEOUT) {
            ri.Printf(PRINT_WARNING, "RTX: Fence timeout before command recording (%s) — GPU may be hung\n",
                      RTX_LogLabel(label));
            return VK_TIMEOUT;
        }
        if (waitRes != VK_SUCCESS) {
            ri.Printf(PRINT_WARNING, "RTX: Failed to wait for fence before command recording (%s) err=%d\n",
                      RTX_LogLabel(label), waitRes);
            if (waitRes == VK_ERROR_DEVICE_LOST) {
                RTX_OnDeviceLost("fence wait");
            }
            return waitRes;
        }
        vkrt.fenceSubmitted = qfalse;
    }

    VkResult result = vkResetCommandBuffer(vkrt.commandBuffer, 0);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to reset command buffer before recording (%s) err=%d\n",
                  RTX_LogLabel(label), result);
        if (result == VK_ERROR_DEVICE_LOST) {
            RTX_OnDeviceLost("command buffer reset");
        }
        return result;
    }

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER, "RTX: begin immediate commands (%s)\n", RTX_LogLabel(label));
    }

    result = vkBeginCommandBuffer(vkrt.commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to begin command buffer (%s) err=%d\n",
                  RTX_LogLabel(label), result);
        if (result == VK_ERROR_DEVICE_LOST) {
            RTX_OnDeviceLost("command buffer begin");
        }
        return result;
    }

    // Immediate batches rebuild/refit acceleration structures that in-flight
    // frames may still be tracing against. Barriers are queue-scoped, so this
    // full ordering barrier prevents the GPU from overlapping this batch with
    // previously submitted frame work.
    {
        VkMemoryBarrier orderingBarrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT
        };
        vkCmdPipelineBarrier(vkrt.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 1, &orderingBarrier, 0, NULL, 0, NULL);
    }

    return result;
}

static VkResult RTX_SubmitImmediateCommands(const char *label) {
    if (!vkrt.device || vkrt.commandBuffer == VK_NULL_HANDLE || vkrt.fence == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (vkrt.deviceLost) {
        return VK_ERROR_DEVICE_LOST;
    }

    if (vkrt.fenceSubmitted) {
        // 2-second timeout prevents the game from freezing if the GPU hangs
        VkResult waitRes = vkWaitForFences(vkrt.device, 1, &vkrt.fence, VK_TRUE, 2000000000ULL);
        if (waitRes == VK_TIMEOUT) {
            ri.Printf(PRINT_WARNING, "RTX: Fence timeout before submit (%s) — GPU may be hung\n",
                      RTX_LogLabel(label));
            return VK_TIMEOUT;
        }
        if (waitRes != VK_SUCCESS) {
            ri.Printf(PRINT_WARNING, "RTX: Fence wait before submit failed (%s) err=%d\n",
                      RTX_LogLabel(label), waitRes);
            if (waitRes == VK_ERROR_DEVICE_LOST) {
                RTX_OnDeviceLost("pre-submit fence wait");
            }
            return waitRes;
        }
        vkrt.fenceSubmitted = qfalse;
    }

    VkResult resetRes = vkResetFences(vkrt.device, 1, &vkrt.fence);
    if (resetRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to reset fence before submission (%s) err=%d\n",
                  RTX_LogLabel(label), resetRes);
        if (resetRes == VK_ERROR_DEVICE_LOST) {
            RTX_OnDeviceLost("fence reset");
        }
        return resetRes;
    }

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &vkrt.commandBuffer
    };

    VkResult result = vkQueueSubmit(vk.queue, 1, &submitInfo, vkrt.fence);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: vkQueueSubmit failed (%s) err=%d\n",
                  RTX_LogLabel(label), result);
        if (result == VK_ERROR_DEVICE_LOST) {
            RTX_OnDeviceLost("queue submit");
        }
        vkrt.fenceSubmitted = qfalse;
        return result;
    }

    vkrt.fenceSubmitted = qtrue;

    result = vkWaitForFences(vkrt.device, 1, &vkrt.fence, VK_TRUE, 2000000000ULL);
    if (result == VK_TIMEOUT) {
        ri.Printf(PRINT_WARNING, "RTX: Fence timeout after submit (%s) — GPU may be hung, skipping frame\n",
                  RTX_LogLabel(label));
        // Leave fenceSubmitted = true so the next Begin call will wait/retry
        return VK_TIMEOUT;
    }
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to wait for fence after submission (%s) err=%d\n",
                  RTX_LogLabel(label), result);
        if (result == VK_ERROR_DEVICE_LOST) {
            RTX_OnDeviceLost("post-submit fence wait");
        }
    } else {
        vkrt.fenceSubmitted = qfalse;
        // Ensure the command buffer returns to the INITIAL state before the next recording.
        // Without an explicit reset, re-beginning the buffer after it was executable can be undefined
        // on some drivers even when the pool was created with RESET_COMMAND_BUFFER_BIT.
        vkResetCommandBuffer(vkrt.commandBuffer, 0);

        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER, "RTX: completed immediate commands (%s)\n", RTX_LogLabel(label));
        }
    }

    return result;
}

// Packed vertex matching the shader's Vertex struct in closesthit.rchit (60 bytes)
typedef struct {
    float position[3];  // 12 bytes
    float normal[3];    // 12 bytes
    float texCoord[2];  // 8 bytes
    float tangent[3];   // 12 bytes
    float color[4];     // 16 bytes
} rtxShaderVertex_t;    // 60 bytes total

typedef struct rtxBLASGPU_s {
    VkAccelerationStructureKHR as;
    VkBuffer asBuffer;
    VkDeviceMemory asMemory;
    VkBuffer vertexBuffer;          // Position-only buffer for BLAS AS construction
    VkDeviceMemory vertexMemory;
    VkBuffer shaderVertexBuffer;    // Full vertex data for shader BDA access (60 bytes/vert)
    VkDeviceMemory shaderVertexMemory;
    VkBuffer indexBuffer;
    VkDeviceMemory indexMemory;
    VkBuffer materialBuffer;
    VkDeviceMemory materialMemory;
} rtxBLASGPU_t;

// Helper to print buffer device address (for correlating with device fault dumps)
static void RTX_DebugLogBufferAddress(const char *name, VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) {
        return;
    }
    VkDeviceAddress addr = RTX_GetBufferDeviceAddressVK(buffer);
    if (addr != 0) {
        ri.Printf(PRINT_WARNING, "    %s addr=0x%llx\n",
                  name, (unsigned long long)addr);
    }
}

void RTX_DebugLogLiveBuffers(const char *stage) {
    const char *label = stage ? stage : "RTX";
    qboolean any = qfalse;

    if (!vkrt.device) {
        ri.Printf(PRINT_WARNING, "RTX: live buffer snapshot (%s) skipped (device not initialized)\n", label);
        return;
    }

    ri.Printf(PRINT_WARNING, "RTX: live buffer snapshot (%s)\n", label);

#define LOG_HANDLE(name, handleValue) \
    do { \
        if ((handleValue) != VK_NULL_HANDLE) { \
            ri.Printf(PRINT_WARNING, "    %s = %p\n", name, (void *)(uintptr_t)(handleValue)); \
            any = qtrue; \
        } \
    } while (0)

    LOG_HANDLE("vkrt.instanceBuffer", vkrt.instanceBuffer);
    LOG_HANDLE("vkrt.instanceMemory", vkrt.instanceMemory);
    LOG_HANDLE("vkrt.raygenSBT", vkrt.raygenSBT);
    LOG_HANDLE("vkrt.missSBT", vkrt.missSBT);
    LOG_HANDLE("vkrt.hitSBT", vkrt.hitSBT);
    LOG_HANDLE("vkrt.sbtMemory", vkrt.sbtMemory);

    // Path-tracing material buffers
    VkBuffer matBuf = RTX_GetMaterialBuffer();
    if (matBuf) {
        LOG_HANDLE("rtx.materialBuffer", matBuf);
    }

    VkBuffer triMatBuf = RTX_GetTriangleMaterialBuffer();
    if (triMatBuf) {
        LOG_HANDLE("rtx.triangleMaterialBuffer", triMatBuf);
        RTX_DebugLogBufferAddress("rtx.triangleMaterialBuffer", triMatBuf);
        ri.Printf(PRINT_WARNING, "    rtx.triangleMaterialCount=%u\n",
                  (unsigned int)RTX_GetTriangleMaterialCount());
    }

    RTX_DebugLogBufferAddress("vkrt.instanceBuffer", vkrt.instanceBuffer);

    if (vkrt.lastScratchAddr) {
        ri.Printf(PRINT_WARNING,
                  "    last scratch buffer addr=0x%llx size=%llu\n",
                  (unsigned long long)vkrt.lastScratchAddr,
                  (unsigned long long)vkrt.lastScratchSize);
    }

    for (int i = 0; i < 2; ++i) {
        if (vkrt.tlasBuffer[i] != VK_NULL_HANDLE || vkrt.tlasMemory[i] != VK_NULL_HANDLE) {
            char bufferLabel[64];
            Com_sprintf(bufferLabel, sizeof(bufferLabel), "vkrt.tlasBuffer[%d]", i);
            LOG_HANDLE(bufferLabel, vkrt.tlasBuffer[i]);

            Com_sprintf(bufferLabel, sizeof(bufferLabel), "vkrt.tlasMemory[%d]", i);
            LOG_HANDLE(bufferLabel, vkrt.tlasMemory[i]);

            if (vkrt.tlas[i] && qvkGetAccelerationStructureDeviceAddressKHR) {
                VkAccelerationStructureDeviceAddressInfoKHR info = {
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                    .accelerationStructure = vkrt.tlas[i]
                };
                VkDeviceAddress addr = qvkGetAccelerationStructureDeviceAddressKHR(vkrt.device, &info);
                if (addr) {
                    ri.Printf(PRINT_WARNING, "    vkrt.tlas[%d] addr=0x%llx buffer=%p\n",
                              i, (unsigned long long)addr,
                              (void*)(uintptr_t)vkrt.tlasBuffer[i]);
                }
            }
        }
    }

    if (rt.sceneLightBuffer != VK_NULL_HANDLE || rt.sceneLightBufferMemory != VK_NULL_HANDLE) {
        LOG_HANDLE("rt.sceneLightBuffer", rt.sceneLightBuffer);
        LOG_HANDLE("rt.sceneLightBufferMemory", rt.sceneLightBufferMemory);
        RTX_DebugLogBufferAddress("rt.sceneLightBuffer", rt.sceneLightBuffer);
    }

    VkBuffer lgOffset = RT_GetLightGridOffsetBuffer();
    if (lgOffset != VK_NULL_HANDLE) {
        LOG_HANDLE("rt.lightGrid.offsetBuffer", lgOffset);
        RTX_DebugLogBufferAddress("rt.lightGrid.offsetBuffer", lgOffset);
        VkDeviceSize sz = RT_GetLightGridOffsetBufferSize();
        ri.Printf(PRINT_WARNING, "    rt.lightGrid.offsetSize=%llu\n", (unsigned long long)sz);
    }
    VkBuffer lgIndex = RT_GetLightGridIndexBuffer();
    if (lgIndex != VK_NULL_HANDLE) {
        LOG_HANDLE("rt.lightGrid.indexBuffer", lgIndex);
        RTX_DebugLogBufferAddress("rt.lightGrid.indexBuffer", lgIndex);
        VkDeviceSize sz = RT_GetLightGridIndexBufferSize();
        ri.Printf(PRINT_WARNING, "    rt.lightGrid.indexSize=%llu\n", (unsigned long long)sz);
    }

    if (vkrt.rtImageMemory != VK_NULL_HANDLE) {
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(vkrt.device, vkrt.rtImage, &memReqs);
        ri.Printf(PRINT_WARNING,
                  "    rtImage memory=%p size=%llu typeBits=0x%X\n",
                  (void*)(uintptr_t)vkrt.rtImageMemory,
                  (unsigned long long)memReqs.size,
                  memReqs.memoryTypeBits);
    }

    {
        VkBuffer offsetBuffer = RT_GetLightGridOffsetBuffer();
        VkBuffer indexBuffer = RT_GetLightGridIndexBuffer();
        RTX_DebugLogBufferAddress("rt.lightGridOffset", offsetBuffer);
        RTX_DebugLogBufferAddress("rt.lightGridIndex", indexBuffer);
    }

    VkBuffer offsetBuffer = RT_GetLightGridOffsetBuffer();
    VkBuffer indexBuffer = RT_GetLightGridIndexBuffer();
    RTX_DebugLogBufferAddress("rt.lightGridOffset", offsetBuffer);
    RTX_DebugLogBufferAddress("rt.lightGridIndex", indexBuffer);

    // Shader Binding Table regions
    VkStridedDeviceAddressRegionKHR rg = {0}, ms = {0}, ht = {0}, cl = {0};
    RTX_GetSBTRegions(&rg, &ms, &ht, &cl);
    if (rg.deviceAddress || ms.deviceAddress || ht.deviceAddress || cl.deviceAddress) {
        ri.Printf(PRINT_WARNING,
                  "    SBT: raygen=0x%llx size=%llu miss=0x%llx size=%llu hit=0x%llx size=%llu callable=0x%llx size=%llu\n",
                  (unsigned long long)rg.deviceAddress, (unsigned long long)rg.size,
                  (unsigned long long)ms.deviceAddress, (unsigned long long)ms.size,
                  (unsigned long long)ht.deviceAddress, (unsigned long long)ht.size,
                  (unsigned long long)cl.deviceAddress, (unsigned long long)cl.size);
    }

    if (rtx.blasPool && rtx.numBLAS > 0) {
        for (int i = 0; i < rtx.numBLAS; ++i) {
            rtxBLAS_t *blas = &rtx.blasPool[i];
            if (blas && blas->gpuData) {
                rtxBLASGPU_t *gpu = (rtxBLASGPU_t *)blas->gpuData;
                if (gpu) {
                    char labelBuf[64];
                    if (gpu->asBuffer != VK_NULL_HANDLE) {
                        Com_sprintf(labelBuf, sizeof(labelBuf), "rtx.blasPool[%d].asBuffer", i);
                        LOG_HANDLE(labelBuf, gpu->asBuffer);
                    }
                    if (gpu->vertexBuffer != VK_NULL_HANDLE) {
                        Com_sprintf(labelBuf, sizeof(labelBuf), "rtx.blasPool[%d].vertexBuffer", i);
                        LOG_HANDLE(labelBuf, gpu->vertexBuffer);
                    }
                    if (gpu->indexBuffer != VK_NULL_HANDLE) {
                        Com_sprintf(labelBuf, sizeof(labelBuf), "rtx.blasPool[%d].indexBuffer", i);
                        LOG_HANDLE(labelBuf, gpu->indexBuffer);
                    }
                    if (gpu->materialBuffer != VK_NULL_HANDLE) {
                        Com_sprintf(labelBuf, sizeof(labelBuf), "rtx.blasPool[%d].materialBuffer", i);
                        LOG_HANDLE(labelBuf, gpu->materialBuffer);
                    }

                    if (qvkGetAccelerationStructureDeviceAddressKHR && gpu->as) {
                        VkAccelerationStructureDeviceAddressInfoKHR info = {
                            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                            .accelerationStructure = gpu->as
                        };
                        VkDeviceAddress addr = qvkGetAccelerationStructureDeviceAddressKHR(vkrt.device, &info);
                        if (addr) {
                            ri.Printf(PRINT_WARNING, "        BLAS[%d] asAddr=0x%llx\n",
                                      i, (unsigned long long)addr);
                        }
                    }
                    RTX_DebugLogBufferAddress("        vb", gpu->vertexBuffer);
                    RTX_DebugLogBufferAddress("        shaderVb", gpu->shaderVertexBuffer);
                    RTX_DebugLogBufferAddress("        ib", gpu->indexBuffer);
                    RTX_DebugLogBufferAddress("        mat", gpu->materialBuffer);
                }
            }
        }
    }

    if (!any) {
        ri.Printf(PRINT_WARNING, "    (no tracked Vulkan buffers)\n");
    }

#undef LOG_HANDLE
}

void RTX_DebugLogDescriptorState(const char *stage) {
    if (!rtx.available || !RTX_IsEnabled()) {
        return;
    }

    const char *label = stage ? stage : "RTX";
    VkDescriptorSet descriptorSet = RTX_GetDescriptorSet();
    VkAccelerationStructureKHR activeTLAS = vkrt.tlas[vkrt.activeTLAS];
    VkDeviceAddress instanceAddr = RTX_GetBufferDeviceAddressVK(vkrt.instanceBuffer);
    VkBuffer lightBuffer = RT_GetSceneLightBuffer();
    VkDeviceSize lightSize = RT_GetSceneLightBufferSize();
    VkBuffer offsetBuffer = RT_GetLightGridOffsetBuffer();
    VkDeviceSize offsetSize = RT_GetLightGridOffsetBufferSize();
    VkBuffer indexBuffer = RT_GetLightGridIndexBuffer();
    VkDeviceSize indexSize = RT_GetLightGridIndexBufferSize();

    ri.Printf(PRINT_DEVELOPER,
              "RTX: descriptor state (%s) set=%p layout=%p TLAS=%p activeTLAS=%d\n",
              label,
              (void*)(uintptr_t)descriptorSet,
              (void*)(uintptr_t)vkrt.pipelineLayout,
              (void*)(uintptr_t)activeTLAS,
              vkrt.activeTLAS);

    ri.Printf(PRINT_DEVELOPER,
              "     instanceBuffer=%p deviceAddr=0x%llx lightBuffer=%p (%llu bytes)\n",
              (void*)(uintptr_t)vkrt.instanceBuffer,
              (unsigned long long)instanceAddr,
              (void*)(uintptr_t)lightBuffer,
              (unsigned long long)lightSize);

    ri.Printf(PRINT_DEVELOPER,
              "     lightGrid offsets=%p (%llu bytes) indices=%p (%llu bytes) rtImage=%p\n",
              (void*)(uintptr_t)offsetBuffer,
              (unsigned long long)offsetSize,
              (void*)(uintptr_t)indexBuffer,
              (unsigned long long)indexSize,
              (void*)(uintptr_t)vkrt.rtImage);
}

static void RTX_DestroyReadbackBuffer(void) {
    if (vkrt.readbackMapped) {
        vkUnmapMemory(vkrt.device, vkrt.readbackMemory);
        vkrt.readbackMapped = NULL;
    }
    if (vkrt.readbackBuffer) {
        vkDestroyBuffer(vkrt.device, vkrt.readbackBuffer, NULL);
        vkrt.readbackBuffer = VK_NULL_HANDLE;
    }
    if (vkrt.readbackMemory) {
        vkFreeMemory(vkrt.device, vkrt.readbackMemory, NULL);
        vkrt.readbackMemory = VK_NULL_HANDLE;
    }
    vkrt.readbackSize = 0;
}

static qboolean RTX_EnsureReadbackBuffer(VkDeviceSize size) {
    if (vkrt.readbackBuffer && size <= vkrt.readbackSize) {
        return qtrue;
    }

    RTX_DestroyReadbackBuffer();

    if (size == 0) {
        return qtrue;
    }

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        // Allow device address so we can log/diagnose and keep drivers happy if
        // they peek device addresses internally.
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateBuffer(vkrt.device, &bufferInfo, NULL, &vkrt.readbackBuffer) != VK_SUCCESS) {
        vkrt.readbackBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vkrt.device, vkrt.readbackBuffer, &memReqs);

    // Pad readback allocation to tolerate minor driver overfetch.
    const VkDeviceSize pad = 1024 * 1024; // 1 MiB guard
    VkDeviceSize padAligned = (pad + memReqs.alignment - 1) & ~(memReqs.alignment - 1);

    VkMemoryAllocateFlagsInfo allocFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &allocFlags,
        .allocationSize = memReqs.size + padAligned,
        .memoryTypeIndex = RTX_FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, &vkrt.readbackMemory) != VK_SUCCESS) {
        vkDestroyBuffer(vkrt.device, vkrt.readbackBuffer, NULL);
        vkrt.readbackBuffer = VK_NULL_HANDLE;
        vkrt.readbackMemory = VK_NULL_HANDLE;
        return qfalse;
    }

    if (vkBindBufferMemory(vkrt.device, vkrt.readbackBuffer, vkrt.readbackMemory, 0) != VK_SUCCESS) {
        RTX_DestroyReadbackBuffer();
        return qfalse;
    }

    if (vkMapMemory(vkrt.device, vkrt.readbackMemory, 0, size, 0, &vkrt.readbackMapped) != VK_SUCCESS) {
        RTX_DestroyReadbackBuffer();
        return qfalse;
    }

    vkrt.readbackSize = size;
    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        VkDeviceAddress addr = RTX_GetBufferDeviceAddressVK(vkrt.readbackBuffer);
        ri.Printf(PRINT_DEVELOPER,
                  "RTX_EnsureReadbackBuffer: size=%llu (raw=%llu pad=%llu align=%llu) addr=0x%llx\n",
                  (unsigned long long)allocInfo.allocationSize,
                  (unsigned long long)memReqs.size,
                  (unsigned long long)padAligned,
                  (unsigned long long)memReqs.alignment,
                  (unsigned long long)addr);
    }

    return qtrue;
}

static qboolean RTX_DownloadColorBuffer(uint32_t width, uint32_t height) {
    if (!vkrt.device || vkrt.commandBuffer == VK_NULL_HANDLE || vkrt.rtImage == VK_NULL_HANDLE) {
        return qfalse;
    }

    if (vkrt.rtImageFormat != VK_FORMAT_R32G32B32A32_SFLOAT) {
        return qfalse;
    }

    if (width != (uint32_t)glConfig.vidWidth || height != (uint32_t)glConfig.vidHeight) {
        return qfalse;
    }

    VkDeviceSize requiredSize = (VkDeviceSize)width * (VkDeviceSize)height * sizeof(float) * 4;

    if (!RTX_EnsureReadbackBuffer(requiredSize)) {
        return qfalse;
    }

    VkBufferImageCopy copyRegion = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 }
    };

    vkCmdCopyImageToBuffer(vkrt.commandBuffer,
                           vkrt.rtImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           vkrt.readbackBuffer, 1, &copyRegion);

    VkBufferMemoryBarrier bufferBarrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = vkrt.readbackBuffer,
        .offset = 0,
        .size = requiredSize
    };

    vkCmdPipelineBarrier(vkrt.commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bufferBarrier, 0, NULL);

    return qtrue;
}

static qboolean RTX_EnsureDebugOverlayPipeline(void);
static void RTX_DestroyDebugOverlayPipeline(void);
static qboolean RTX_UpdateDebugOverlayDescriptors(void);
static qboolean RTX_EnsureCompositePipeline(void);
static void RTX_DestroyCompositePipeline(void);
static void RTX_RecordDepthAwareComposite(VkCommandBuffer cmd, uint32_t width, uint32_t height);
static qboolean RTX_EnsureDenoisePipeline(void);
static void RTX_DestroyDenoisePipeline(void);
static void RTX_RecordDenoise(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                              qboolean filter, qboolean inputIsIllum);
static qboolean RTX_EnsureTemporalPipeline(void);
static void RTX_DestroyTemporalPipeline(void);
static qboolean RTX_RecordTemporal(VkCommandBuffer cmd, uint32_t width, uint32_t height);

static const char *RTX_VendorLabel(rtxGpuType_t type) {
    switch (type) {
    case RTX_GPU_NVIDIA:
        return "NVIDIA";
    case RTX_GPU_AMD:
        return "AMD";
    case RTX_GPU_INTEL:
        return "Intel";
    default:
        return "Unknown";
    }
}

static void RTX_SetNVIDIAArchitecture(const char *deviceName) {
    rtx.rayTracingTier = 1;
    Q_strncpyz(rtx.gpuArchitecture, "NVIDIA RT", sizeof(rtx.gpuArchitecture));

    if (!deviceName || !deviceName[0]) {
        return;
    }

    if (Q_stristr(deviceName, "RTX 40") ||
        Q_stristr(deviceName, "4090") ||
        Q_stristr(deviceName, "4080") ||
        Q_stristr(deviceName, "4070") ||
        Q_stristr(deviceName, "Ada")) {
        rtx.rayTracingTier = 3;
        Q_strncpyz(rtx.gpuArchitecture, "Ada Lovelace", sizeof(rtx.gpuArchitecture));
        return;
    }

    if (Q_stristr(deviceName, "RTX 30") ||
        Q_stristr(deviceName, "3090") ||
        Q_stristr(deviceName, "3080") ||
        Q_stristr(deviceName, "3070") ||
        Q_stristr(deviceName, "Ampere") ||
        Q_stristr(deviceName, "RTX A") ||
        Q_stristr(deviceName, "A40") ||
        Q_stristr(deviceName, "A5000") ||
        Q_stristr(deviceName, "L40")) {
        rtx.rayTracingTier = 2;
        Q_strncpyz(rtx.gpuArchitecture, "Ampere", sizeof(rtx.gpuArchitecture));
        return;
    }

    if (Q_stristr(deviceName, "RTX 20") ||
        Q_stristr(deviceName, "2080") ||
        Q_stristr(deviceName, "2070") ||
        Q_stristr(deviceName, "2060") ||
        Q_stristr(deviceName, "TITAN RTX") ||
        Q_stristr(deviceName, "Quadro RTX") ||
        Q_stristr(deviceName, "Turing")) {
        rtx.rayTracingTier = 1;
        Q_strncpyz(rtx.gpuArchitecture, "Turing", sizeof(rtx.gpuArchitecture));
        return;
    }
}

static void RTX_SetAMDArchitecture(const char *deviceName) {
    rtx.rayTracingTier = 1;
    Q_strncpyz(rtx.gpuArchitecture, "RDNA", sizeof(rtx.gpuArchitecture));

    if (!deviceName || !deviceName[0]) {
        return;
    }

    if (Q_stristr(deviceName, "7900") ||
        Q_stristr(deviceName, "7800") ||
        Q_stristr(deviceName, "7700")) {
        Q_strncpyz(rtx.gpuArchitecture, "RDNA 3", sizeof(rtx.gpuArchitecture));
        return;
    }

    if (Q_stristr(deviceName, "RX 6") ||
        Q_stristr(deviceName, "6900") ||
        Q_stristr(deviceName, "6800") ||
        Q_stristr(deviceName, "6700") ||
        Q_stristr(deviceName, "6600")) {
        Q_strncpyz(rtx.gpuArchitecture, "RDNA 2", sizeof(rtx.gpuArchitecture));
        return;
    }

    if (Q_stristr(deviceName, "5700") ||
        Q_stristr(deviceName, "5600") ||
        Q_stristr(deviceName, "5500")) {
        Q_strncpyz(rtx.gpuArchitecture, "RDNA 1", sizeof(rtx.gpuArchitecture));
    }
}

static void RTX_SetIntelArchitecture(const char *deviceName) {
    rtx.rayTracingTier = 1;
    Q_strncpyz(rtx.gpuArchitecture, "Xe", sizeof(rtx.gpuArchitecture));

    if (!deviceName || !deviceName[0]) {
        return;
    }

    if (Q_stristr(deviceName, "Arc")) {
        Q_strncpyz(rtx.gpuArchitecture, "Xe-HPG", sizeof(rtx.gpuArchitecture));
    }
}

static void RTX_DetectGPUVendor(const VkPhysicalDeviceProperties *props, const char **outVendorLabel) {
    const char *vendorLabel = "Unknown";

    rtx.gpuType = RTX_GPU_UNKNOWN;
    rtx.rayTracingTier = 0;
    rtx.gpuName[0] = '\0';
    rtx.gpuArchitecture[0] = '\0';

    if (props) {
        Q_strncpyz(rtx.gpuName, props->deviceName, sizeof(rtx.gpuName));

        switch (props->vendorID) {
        case 0x10DE:
            vendorLabel = "NVIDIA";
            rtx.gpuType = RTX_GPU_NVIDIA;
            RTX_SetNVIDIAArchitecture(props->deviceName);
            break;
        case 0x1002:
        case 0x1022:
            vendorLabel = "AMD";
            rtx.gpuType = RTX_GPU_AMD;
            RTX_SetAMDArchitecture(props->deviceName);
            break;
        case 0x8086:
            vendorLabel = "Intel";
            rtx.gpuType = RTX_GPU_INTEL;
            RTX_SetIntelArchitecture(props->deviceName);
            break;
        default:
            vendorLabel = "Unknown";
            break;
        }

        vkrt.deviceProps = *props;
    }

    if (rtx.rayTracingTier <= 0) {
        rtx.rayTracingTier = 1;
    }

    if (!rtx.gpuArchitecture[0]) {
        Q_strncpyz(rtx.gpuArchitecture, "Unknown", sizeof(rtx.gpuArchitecture));
    }

    if (outVendorLabel) {
        *outVendorLabel = vendorLabel;
    }
}

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
    const char *vendorLabel = "Unknown";
    VkPhysicalDeviceProperties props;
    Com_Memset(&props, 0, sizeof(props));

    if (!vkrt.physicalDevice) {
        return qfalse;
    }

    vkGetPhysicalDeviceProperties(vkrt.physicalDevice, &props);
    RTX_DetectGPUVendor(&props, &vendorLabel);

    rtx.features = RTX_FEATURE_NONE;
    vkrt.hasRayTracingPipeline = qfalse;
    vkrt.hasAccelerationStructure = qfalse;
    vkrt.hasRayQuery = qfalse;
    vkrt.hasDeferredHostOps = qfalse;
    vkrt.hasRTMaintenance1 = qfalse;

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
    qboolean hasDeferredOps = qfalse;
    qboolean hasMaintenance1 = qfalse;
    
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
#ifdef VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
        if (!strcmp(extensions[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
            hasDeferredOps = qtrue;
        }
#endif
#ifdef VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME
        if (!strcmp(extensions[i].extensionName, VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME)) {
            hasMaintenance1 = qtrue;
        }
#endif
    }
    
    Z_Free(extensions);

    vkrt.hasRayTracingPipeline = hasRayTracing;
    vkrt.hasAccelerationStructure = hasAccelStruct;
    vkrt.hasRayQuery = hasRayQuery;
    vkrt.hasDeferredHostOps = hasDeferredOps;
    vkrt.hasRTMaintenance1 = hasMaintenance1;
    
    if (!hasRayTracing || !hasAccelStruct) {
        ri.Printf(PRINT_WARNING, "RTX: Required Vulkan RT extensions not available\n");
        ri.Printf(PRINT_WARNING, "RTX: Ray Tracing: %s, Accel Struct: %s, Ray Query: %s\n",
                  hasRayTracing ? "YES" : "NO",
                  hasAccelStruct ? "YES" : "NO", 
                  hasRayQuery ? "YES" : "NO");
        return qfalse;
    }
    
    rtx.features |= (RTX_FEATURE_BASIC | RTX_FEATURE_RAY_TRACING);
    if (hasRayQuery) {
        rtx.features |= RTX_FEATURE_RAY_QUERY;
    }

#ifdef USE_OPTIX
    if (rtx.gpuType == RTX_GPU_NVIDIA) {
        rtx.features |= RTX_FEATURE_DENOISER;
    }
#endif
#ifdef USE_DLSS
    if (rtx.gpuType == RTX_GPU_NVIDIA) {
        rtx.features |= RTX_FEATURE_DLSS;
    }
#endif
#ifdef USE_REFLEX
    if (rtx.gpuType == RTX_GPU_NVIDIA) {
        rtx.features |= RTX_FEATURE_REFLEX;
    }
#endif

    ri.Printf(PRINT_ALL, "RTX: GPU detected: %s (%s)\n",
              rtx.gpuName[0] ? rtx.gpuName : "Unknown",
              vendorLabel);
    if (rtx.gpuArchitecture[0]) {
        ri.Printf(PRINT_ALL, "RTX: Architecture: %s (Tier %d)\n", rtx.gpuArchitecture, rtx.rayTracingTier);
    }
    ri.Printf(PRINT_ALL, "RTX: Vulkan RT extensions detected:\n");
    ri.Printf(PRINT_ALL, "  Ray Tracing Pipeline   : %s\n", hasRayTracing ? "YES" : "NO");
    ri.Printf(PRINT_ALL, "  Acceleration Structure : %s\n", hasAccelStruct ? "YES" : "NO");
    ri.Printf(PRINT_ALL, "  Ray Query              : %s\n", hasRayQuery ? "YES" : "NO");
#ifdef VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    ri.Printf(PRINT_ALL, "  Deferred Host Ops      : %s\n", hasDeferredOps ? "YES" : "NO");
#endif
#ifdef VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME
    ri.Printf(PRINT_ALL, "  RT Maintenance 1       : %s\n", hasMaintenance1 ? "YES" : "NO");
#endif

    return qtrue;
}

static void RTX_LogCapabilitySummary(void) {
    const char *vendor = RTX_VendorLabel(rtx.gpuType);

    ri.Printf(PRINT_ALL, "\n========================================\n");
    ri.Printf(PRINT_ALL, "RTX Capability Summary\n");
    ri.Printf(PRINT_ALL, "========================================\n");
    ri.Printf(PRINT_ALL, "GPU: %s\n", rtx.gpuName[0] ? rtx.gpuName : "Unknown");
    ri.Printf(PRINT_ALL, "Vendor: %s (0x%04X)\n", vendor, vkrt.deviceProps.vendorID);
    if (rtx.gpuArchitecture[0]) {
        ri.Printf(PRINT_ALL, "Architecture: %s\n", rtx.gpuArchitecture);
    }
    ri.Printf(PRINT_ALL, "Ray Tracing Tier: %d\n", rtx.rayTracingTier);

    ri.Printf(PRINT_ALL, "\nExtensions:\n");
    ri.Printf(PRINT_ALL, "  Ray Tracing Pipeline   : %s\n", vkrt.hasRayTracingPipeline ? "YES" : "NO");
    ri.Printf(PRINT_ALL, "  Acceleration Structure : %s\n", vkrt.hasAccelerationStructure ? "YES" : "NO");
    ri.Printf(PRINT_ALL, "  Ray Query              : %s\n", vkrt.hasRayQuery ? "YES" : "NO");
#ifdef VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    ri.Printf(PRINT_ALL, "  Deferred Host Ops      : %s\n", vkrt.hasDeferredHostOps ? "YES" : "NO");
#endif
#ifdef VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME
    ri.Printf(PRINT_ALL, "  RT Maintenance 1       : %s\n", vkrt.hasRTMaintenance1 ? "YES" : "NO");
#endif

    ri.Printf(PRINT_ALL, "\nFeatures:\n");
    ri.Printf(PRINT_ALL, "  [%c] Ray Tracing\n", (rtx.features & RTX_FEATURE_RAY_TRACING) ? 'x' : ' ');
    ri.Printf(PRINT_ALL, "  [%c] Ray Query\n", (rtx.features & RTX_FEATURE_RAY_QUERY) ? 'x' : ' ');
    ri.Printf(PRINT_ALL, "  [%c] Denoiser\n", (rtx.features & RTX_FEATURE_DENOISER) ? 'x' : ' ');
    ri.Printf(PRINT_ALL, "  [%c] DLSS\n", (rtx.features & RTX_FEATURE_DLSS) ? 'x' : ' ');
    ri.Printf(PRINT_ALL, "  [%c] Reflex\n", (rtx.features & RTX_FEATURE_REFLEX) ? 'x' : ' ');

    if (rtx.shaderGroupHandleSize > 0) {
        ri.Printf(PRINT_ALL, "\nRay Tracing Limits:\n");
        ri.Printf(PRINT_ALL, "  Max Recursion Depth    : %u\n", rtx.maxRayRecursionDepth);
        ri.Printf(PRINT_ALL, "  Shader Handle Size     : %u\n", rtx.shaderGroupHandleSize);
        ri.Printf(PRINT_ALL, "  Handle Alignment       : %u\n", rtx.shaderGroupHandleAlignment);
        ri.Printf(PRINT_ALL, "  Base Alignment         : %u\n", rtx.shaderGroupBaseAlignment);
        ri.Printf(PRINT_ALL, "  Max Primitive Count    : %llu\n", (unsigned long long)rtx.maxPrimitiveCount);
        ri.Printf(PRINT_ALL, "  Max Instance Count     : %llu\n", (unsigned long long)rtx.maxInstanceCount);
        ri.Printf(PRINT_ALL, "  Max Geometry Count     : %llu\n", (unsigned long long)rtx.maxGeometryCount);
    }

    ri.Printf(PRINT_ALL, "========================================\n");
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
    vkrt.deviceLost = qfalse;
    vkrt.fenceSubmitted = qfalse;
    RTX_ResetFramebufferCopySupport();
    
	// Load RT extension functions
	PFN_vkCreateAccelerationStructureKHR createAccel = (PFN_vkCreateAccelerationStructureKHR)
		vkGetDeviceProcAddr(vkrt.device, "vkCreateAccelerationStructureKHR");
	PFN_vkDestroyAccelerationStructureKHR destroyAccel = (PFN_vkDestroyAccelerationStructureKHR)
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
	if (!createAccel || !destroyAccel ||
		!qvkGetAccelerationStructureBuildSizesKHR || !qvkCmdBuildAccelerationStructuresKHR ||
		!qvkGetAccelerationStructureDeviceAddressKHR || !qvkCmdTraceRaysKHR ||
		!qvkGetBufferDeviceAddress) {
		ri.Printf(PRINT_WARNING, "RTX: Failed to load RT extension functions\n");
		return qfalse;
	}

	qvkCreateAccelerationStructureKHR = createAccel;
	qvkDestroyAccelerationStructureKHR = destroyAccel;

	vk_register_acceleration_structure_dispatch(createAccel, destroyAccel);
    
    // Get RT properties
    vkrt.rtProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    vkrt.asProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    vkrt.rtProperties.pNext = &vkrt.asProperties;
    
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &vkrt.rtProperties
    };
    
    vkGetPhysicalDeviceProperties2(vkrt.physicalDevice, &props2);
    
    rtx.maxRayRecursionDepth = vkrt.rtProperties.maxRayRecursionDepth;
    rtx.shaderGroupHandleSize = vkrt.rtProperties.shaderGroupHandleSize;
    rtx.shaderGroupHandleAlignment = vkrt.rtProperties.shaderGroupHandleAlignment;
    rtx.shaderGroupBaseAlignment = vkrt.rtProperties.shaderGroupBaseAlignment;
    rtx.maxPrimitiveCount = vkrt.asProperties.maxPrimitiveCount;
    rtx.maxInstanceCount = vkrt.asProperties.maxInstanceCount;
    rtx.maxGeometryCount = vkrt.asProperties.maxGeometryCount;

    RTX_LogCapabilitySummary();
    
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
    if (result == VK_SUCCESS) {
        RTX_DEBUG_LOG_CMD("alloc", vkrt.commandBuffer, "RTX_InitVulkanRT");
        vk_cmd_register("rtx_main", vkrt.commandBuffer, vkrt.commandPool);
    }
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
    vkrt.fenceSubmitted = qfalse;

    RTX_DestroyDebugOverlayPipeline();
    RTX_DestroyCompositePipeline();
    RTX_DestroyDenoisePipeline();
    RTX_DestroyTemporalPipeline();
    RTX_DestroyReadbackBuffer();

    // Destroy RT resources
    for (int i = 0; i < 2; i++) {
        if (vkrt.tlas[i]) {
            qvkDestroyAccelerationStructureKHR(vkrt.device, vkrt.tlas[i], NULL);
            vkrt.tlas[i] = VK_NULL_HANDLE;
        }
        if (vkrt.tlasBuffer[i]) {
            vkDestroyBuffer(vkrt.device, vkrt.tlasBuffer[i], NULL);
            vkrt.tlasBuffer[i] = VK_NULL_HANDLE;
        }
        if (vkrt.tlasMemory[i]) {
            vkFreeMemory(vkrt.device, vkrt.tlasMemory[i], NULL);
            vkrt.tlasMemory[i] = VK_NULL_HANDLE;
        }
    }
    vkrt.activeTLAS = 0;
    
    if (vkrt.instanceBuffer) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Destroying TLAS instance buffer %p\n",
                      (void*)vkrt.instanceBuffer);
        }
        vkDestroyBuffer(vkrt.device, vkrt.instanceBuffer, NULL);
        vkrt.instanceBuffer = VK_NULL_HANDLE;
    }
    if (vkrt.scratchBuffer) {
        vkDestroyBuffer(vkrt.device, vkrt.scratchBuffer, NULL);
        vkrt.scratchBuffer = VK_NULL_HANDLE;
    }
    if (vkrt.scratchMemory) {
        vkFreeMemory(vkrt.device, vkrt.scratchMemory, NULL);
        vkrt.scratchMemory = VK_NULL_HANDLE;
    }
    if (vkrt.instanceMemory) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Freeing TLAS instance memory %p\n",
                      (void*)vkrt.instanceMemory);
        }
        vkFreeMemory(vkrt.device, vkrt.instanceMemory, NULL);
        vkrt.instanceMemory = VK_NULL_HANDLE;
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
    
    if (vkrt.commandBuffer != VK_NULL_HANDLE && vkrt.commandPool != VK_NULL_HANDLE) {
        RTX_DEBUG_LOG_CMD("free", vkrt.commandBuffer, "RTX_Shutdown");
        vk_cmd_unregister(vkrt.commandBuffer);
        vkFreeCommandBuffers(vkrt.device, vkrt.commandPool, 1, &vkrt.commandBuffer);
        vkrt.commandBuffer = VK_NULL_HANDLE;
    }

    // Destroy G-buffer images
    for (int h = 0; h < 2; h++) {
        RTX_DestroyGBufferImage(&vkrt.historyIllumImage[h], &vkrt.historyIllumImageView[h], &vkrt.historyIllumImageMemory[h]);
        RTX_DestroyGBufferImage(&vkrt.historyGeomImage[h], &vkrt.historyGeomImageView[h], &vkrt.historyGeomImageMemory[h]);
    }
    RTX_DestroyGBufferImage(&vkrt.denoisePingImage, &vkrt.denoisePingImageView, &vkrt.denoisePingImageMemory);
    RTX_DestroyGBufferImage(&vkrt.mediaImage, &vkrt.mediaImageView, &vkrt.mediaImageMemory);
    RTX_DestroyGBufferImage(&vkrt.albedoImage, &vkrt.albedoImageView, &vkrt.albedoImageMemory);
    RTX_DestroyGBufferImage(&vkrt.normalImage, &vkrt.normalImageView, &vkrt.normalImageMemory);
    RTX_DestroyGBufferImage(&vkrt.motionImage, &vkrt.motionImageView, &vkrt.motionImageMemory);
    RTX_DestroyGBufferImage(&vkrt.depthImage, &vkrt.depthImageView, &vkrt.depthImageMemory);
    RTX_DestroyGBufferImage(&vkrt.directLightImage, &vkrt.directLightImageView, &vkrt.directLightImageMemory);
    RTX_DestroyGBufferImage(&vkrt.indirectLightImage, &vkrt.indirectLightImageView, &vkrt.indirectLightImageMemory);

    if (vkrt.commandPool) {
        vkDestroyCommandPool(vkrt.device, vkrt.commandPool, NULL);
        vkrt.commandPool = VK_NULL_HANDLE;
	}
	
	Com_Memset(&vkrt, 0, sizeof(vkrt));
	RTX_ResetFramebufferCopySupport();
	ri.Printf(PRINT_ALL, "RTX: Vulkan RT shutdown complete\n");
}

void RTX_ResetTLASGPU(void) {
	if (!vkrt.device) {
		return;
	}

	vkDeviceWaitIdle(vkrt.device);

	for (int i = 0; i < 2; i++) {
		if (vkrt.tlas[i]) {
			qvkDestroyAccelerationStructureKHR(vkrt.device, vkrt.tlas[i], NULL);
			vkrt.tlas[i] = VK_NULL_HANDLE;
		}
		if (vkrt.tlasBuffer[i]) {
			vkDestroyBuffer(vkrt.device, vkrt.tlasBuffer[i], NULL);
			vkrt.tlasBuffer[i] = VK_NULL_HANDLE;
		}
		if (vkrt.tlasMemory[i]) {
			vkFreeMemory(vkrt.device, vkrt.tlasMemory[i], NULL);
			vkrt.tlasMemory[i] = VK_NULL_HANDLE;
		}
	}

	vkrt.activeTLAS = 0;
}

qboolean RTX_RayQuerySupported(void) {
    return vkrt.hasRayQuery ? qtrue : qfalse;
}

qboolean RTX_DispatchShadowQueries(rtShadowQuery_t *queries, int count) {
    if (!vkrt.hasRayQuery || !queries || count <= 0) {
        return qfalse;
    }

    if (!RTX_RayQueryUpload(queries, count)) {
        return qfalse;
    }

    VkPipeline pipeline = RTX_GetRayQueryPipelineHandle();
    VkPipelineLayout layout = RTX_GetPipelineLayout();
    VkDescriptorSet descriptorSet = RTX_GetDescriptorSet();
    VkBuffer queryBuffer = RTX_RayQueryGetBuffer();
    VkAccelerationStructureKHR activeTLAS = vkrt.tlas[vkrt.activeTLAS];
    VkBuffer materialBuffer = RTX_GetMaterialBuffer();
    VkBuffer triangleBuffer = RTX_GetTriangleMaterialBuffer();
    uint32_t triangleCount = RTX_GetTriangleMaterialCount();

    if (!pipeline || !layout || !descriptorSet || queryBuffer == VK_NULL_HANDLE ||
        activeTLAS == VK_NULL_HANDLE || materialBuffer == VK_NULL_HANDLE ||
        triangleBuffer == VK_NULL_HANDLE || triangleCount == 0) {
        return qfalse;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: DispatchShadowQueries count=%d set=%p tlas=%p queryBuf=%p matBuf=%p triBuf=%p triCount=%u\n",
                  count,
                  (void*)descriptorSet,
                  (void*)activeTLAS,
                  (void*)queryBuffer,
                  (void*)materialBuffer,
                  (void*)triangleBuffer,
                  triangleCount);
    }

    if (!vkrt.commandBuffer) {
        return qfalse;
    }

    if (!RTX_UpdateRayQueryDescriptors(activeTLAS)) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: RayQuery dispatch aborted (descriptor set not ready)\n");
        }
        return qfalse;
    }

    if (RTX_BeginImmediateCommands("RayQueryDispatch") != VK_SUCCESS) {
        return qfalse;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 3) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: RayQuery descriptors bound set=%p tlas=%p\n",
                  (void*)descriptorSet,
                  (void*)activeTLAS);
    }

    vkCmdBindPipeline(vkrt.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(vkrt.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            layout, 0, 1, &descriptorSet, 0, NULL);

    uint32_t queryCount = (uint32_t)count;
    vkCmdPushConstants(vkrt.commandBuffer, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &queryCount);

    uint32_t groupCount = (queryCount + 63) / 64;
    if (groupCount == 0) {
        groupCount = 1;
    }

    vkCmdDispatch(vkrt.commandBuffer, groupCount, 1, 1);

    VkBufferMemoryBarrier bufferBarrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = queryBuffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE
    };

    vkCmdPipelineBarrier(vkrt.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &bufferBarrier, 0, NULL);

    if (vkEndCommandBuffer(vkrt.commandBuffer) != VK_SUCCESS) {
        return qfalse;
    }

    if (RTX_SubmitImmediateCommands("RayQueryDispatch") != VK_SUCCESS) {
        return qfalse;
    }

    RTX_RayQueryDownload(queries, count);
    return qtrue;
}

/*
================
RTX_CreateBLASVulkan

Internal function to create Vulkan BLAS
================
*/
static VkAccelerationStructureKHR RTX_CreateBLASVulkan(const VkAccelerationStructureGeometryKHR *geometry,
                                                       const VkAccelerationStructureBuildRangeInfoKHR *range,
                                                       VkBuildAccelerationStructureFlagsKHR flags,
                                                       VkBuffer *blasBuffer, VkDeviceMemory *blasMemory) {
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = flags,
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
    ri.Printf(PRINT_ALL,
              "RTX_CreateBLASVulkan: buildSizes accelSize=%llu scratch=%llu prim=%u\n",
              (unsigned long long)sizeInfo.accelerationStructureSize,
              (unsigned long long)sizeInfo.buildScratchSize,
              primitiveCount);
    
    // Create buffer for AS
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeInfo.accelerationStructureSize,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };
    
    VkResult createBufRes = vkCreateBuffer(vkrt.device, &bufferInfo, NULL, blasBuffer);
    if (createBufRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBLASVulkan: vkCreateBuffer(size=%llu) failed err=%d\n",
                  (unsigned long long)bufferInfo.size, createBufRes);
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
        // Some drivers appear to underestimate the backing store required for
        // certain BLAS builds (especially large batches).  Pad the allocation
        // to give the builder extra headroom and avoid WRITE_INVALID faults on
        // vkQueueSubmit.  256 KiB is cheap relative to typical BLAS sizes and
        // prevents out-of-bounds writes observed around the BLAS buffer end.
        .allocationSize = memReqs.size,
        .memoryTypeIndex = RTX_FindMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };

    // Pad allocation to be safe against under-sized memReqs/sizeInfo.
    // Align the padding to the device requirement to avoid wasting space.
    const VkDeviceSize padSize = 1024 * 1024; // 1 MiB safety margin
    VkDeviceSize alignedPad = (padSize + memReqs.alignment - 1) &
                              ~(memReqs.alignment - 1);
    allocInfo.allocationSize += alignedPad;
    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX_CreateBLASVulkan: allocating %llu bytes (raw=%llu pad=%llu align=%llu)\n",
                  (unsigned long long)allocInfo.allocationSize,
                  (unsigned long long)memReqs.size,
                  (unsigned long long)alignedPad,
                  (unsigned long long)memReqs.alignment);
    }
    
    VkResult allocRes = vkAllocateMemory(vkrt.device, &allocInfo, NULL, blasMemory);
    if (allocRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBLASVulkan: vkAllocateMemory(size=%llu typeBits=0x%X) failed err=%d\n",
                  (unsigned long long)allocInfo.allocationSize,
                  memReqs.memoryTypeBits,
                  allocRes);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    VkResult bindRes = vkBindBufferMemory(vkrt.device, *blasBuffer, *blasMemory, 0);
    if (bindRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBLASVulkan: vkBindBufferMemory failed err=%d\n",
                  bindRes);
        vkFreeMemory(vkrt.device, *blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = *blasBuffer,
        .size = sizeInfo.accelerationStructureSize,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
    };
    
    VkAccelerationStructureKHR blas;
    VkResult createRes = qvkCreateAccelerationStructureKHR(vkrt.device, &createInfo, NULL, &blas);
    if (createRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBLASVulkan: CreateAccelerationStructure(size=%llu) failed err=%d\n",
                  (unsigned long long)createInfo.size,
                  createRes);
        vkFreeMemory(vkrt.device, *blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    // Allocate scratch buffer
    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    scratchBuffer = RTX_AllocateScratchBuffer(sizeInfo.buildScratchSize, &scratchMemory);
    
    if (!scratchBuffer) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBLASVulkan: failed to allocate scratch (size=%llu)\n",
                  (unsigned long long)sizeInfo.buildScratchSize);
        qvkDestroyAccelerationStructureKHR(vkrt.device, blas, NULL);
        vkFreeMemory(vkrt.device, *blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    // Build the BLAS
    VkResult beginRes = RTX_BeginImmediateCommands("BuildBLAS");
    if (beginRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBLASVulkan: BeginImmediateCommands failed err=%d\n",
                  beginRes);
        qvkDestroyAccelerationStructureKHR(vkrt.device, blas, NULL);
        vkFreeMemory(vkrt.device, *blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        *blasBuffer = VK_NULL_HANDLE;
        *blasMemory = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    
    buildInfo.dstAccelerationStructure = blas;
    buildInfo.scratchData.deviceAddress = RTX_GetBufferDeviceAddressVK(scratchBuffer);
    
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

    VkResult endRes = vkEndCommandBuffer(vkrt.commandBuffer);
    if (endRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBLASVulkan: vkEndCommandBuffer failed err=%d\n",
                  endRes);
        qvkDestroyAccelerationStructureKHR(vkrt.device, blas, NULL);
        vkFreeMemory(vkrt.device, *blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        *blasBuffer = VK_NULL_HANDLE;
        *blasMemory = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    VkResult submitRes = RTX_SubmitImmediateCommands("BuildBLAS");
    if (submitRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBLASVulkan: SubmitImmediateCommands failed err=%d\n",
                  submitRes);
        qvkDestroyAccelerationStructureKHR(vkrt.device, blas, NULL);
        vkFreeMemory(vkrt.device, *blasMemory, NULL);
        vkDestroyBuffer(vkrt.device, *blasBuffer, NULL);
        *blasBuffer = VK_NULL_HANDLE;
        *blasMemory = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    // Clean up scratch buffer
    
    return blas;
}

static qboolean RTX_CreateBufferWithData(VkDeviceSize size, VkBufferUsageFlags usage,
                                         VkMemoryPropertyFlags properties, const void *srcData,
                                         VkBuffer *outBuffer, VkDeviceMemory *outMemory) {
    if (vkCreateBuffer == NULL) {
        return qfalse;
    }

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VkResult result = vkCreateBuffer(vkrt.device, &bufferInfo, NULL, outBuffer);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: vkCreateBuffer failed size=%llu usage=0x%X err=%d\n",
                  (unsigned long long)size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, result);
        return qfalse;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vkrt.device, *outBuffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &allocFlags,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = RTX_FindMemoryType(memReqs.memoryTypeBits, properties)
    };

    ri.Printf(PRINT_DEVELOPER,
              "RTX_CreateBufferWithData: size=%llu usage=0x%X props=0x%X memTypeBits=0x%X chosenType=%u\n",
              (unsigned long long)size,
              usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
              properties,
              memReqs.memoryTypeBits,
              allocInfo.memoryTypeIndex);

    result = vkAllocateMemory(vkrt.device, &allocInfo, NULL, outMemory);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: vkAllocateMemory failed size=%llu typeBits=0x%X props=0x%X err=%d\n",
                  (unsigned long long)allocInfo.allocationSize,
                  memReqs.memoryTypeBits,
                  properties,
                  result);
        vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
        *outBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    result = vkBindBufferMemory(vkrt.device, *outBuffer, *outMemory, 0);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: vkBindBufferMemory failed err=%d\n", result);
        vkFreeMemory(vkrt.device, *outMemory, NULL);
        vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
        *outMemory = VK_NULL_HANDLE;
        *outBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    if (!srcData) {
        return qtrue;
    }

    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void *mapped = NULL;
        VkResult mapRes = vkMapMemory(vkrt.device, *outMemory, 0, size, 0, &mapped);
        if (mapRes != VK_SUCCESS) {
            ri.Printf(PRINT_WARNING,
                      "RTX_CreateBufferWithData: vkMapMemory failed (host-visible path) err=%d\n",
                      mapRes);
            vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
            vkFreeMemory(vkrt.device, *outMemory, NULL);
            *outMemory = VK_NULL_HANDLE;
            *outBuffer = VK_NULL_HANDLE;
            return qfalse;
        }

        memcpy(mapped, srcData, size);
        vkUnmapMemory(vkrt.device, *outMemory);
        return qtrue;
    }

    // Device-local path: create staging buffer
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;

    VkBufferCreateInfo stagingInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    result = vkCreateBuffer(vkrt.device, &stagingInfo, NULL, &stagingBuffer);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: staging vkCreateBuffer failed size=%llu err=%d\n",
                  (unsigned long long)size, result);
        vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
        vkFreeMemory(vkrt.device, *outMemory, NULL);
        *outMemory = VK_NULL_HANDLE;
        *outBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    VkMemoryRequirements stagingReqs;
    vkGetBufferMemoryRequirements(vkrt.device, stagingBuffer, &stagingReqs);

    VkMemoryAllocateInfo stagingAlloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = stagingReqs.size,
        .memoryTypeIndex = RTX_FindMemoryType(
            stagingReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    ri.Printf(PRINT_DEVELOPER,
              "RTX_CreateBufferWithData: staging size=%llu memTypeBits=0x%X chosenType=%u\n",
              (unsigned long long)stagingAlloc.allocationSize,
              stagingReqs.memoryTypeBits,
              stagingAlloc.memoryTypeIndex);

    result = vkAllocateMemory(vkrt.device, &stagingAlloc, NULL, &stagingMemory);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: staging vkAllocateMemory failed size=%llu typeBits=0x%X err=%d\n",
                  (unsigned long long)stagingAlloc.allocationSize,
                  stagingReqs.memoryTypeBits,
                  result);
        vkDestroyBuffer(vkrt.device, stagingBuffer, NULL);
        vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
        vkFreeMemory(vkrt.device, *outMemory, NULL);
        *outMemory = VK_NULL_HANDLE;
        *outBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    vkBindBufferMemory(vkrt.device, stagingBuffer, stagingMemory, 0);

    void *mapped = NULL;
    result = vkMapMemory(vkrt.device, stagingMemory, 0, size, 0, &mapped);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: staging vkMapMemory failed size=%llu err=%d\n",
                  (unsigned long long)size,
                  result);
        vkDestroyBuffer(vkrt.device, stagingBuffer, NULL);
        vkFreeMemory(vkrt.device, stagingMemory, NULL);
        vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
        vkFreeMemory(vkrt.device, *outMemory, NULL);
        *outMemory = VK_NULL_HANDLE;
        *outBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    memcpy(mapped, srcData, size);
    vkUnmapMemory(vkrt.device, stagingMemory);

    // Record transfer
    result = RTX_BeginImmediateCommands("UploadBuffer");
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: BeginImmediateCommands failed err=%d\n",
                  result);
        vkDestroyBuffer(vkrt.device, stagingBuffer, NULL);
        vkFreeMemory(vkrt.device, stagingMemory, NULL);
        vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
        vkFreeMemory(vkrt.device, *outMemory, NULL);
        *outMemory = VK_NULL_HANDLE;
        *outBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    VkBufferCopy copyRegion = {
        .srcOffset = 0,
        .dstOffset = 0,
        .size = size
    };

    vkCmdCopyBuffer(vkrt.commandBuffer, stagingBuffer, *outBuffer, 1, &copyRegion);

    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                         VK_ACCESS_SHADER_READ_BIT
    };

    vkCmdPipelineBarrier(vkrt.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 1, &barrier, 0, NULL, 0, NULL);

    result = vkEndCommandBuffer(vkrt.commandBuffer);
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: vkEndCommandBuffer failed err=%d\n",
                  result);
        vkDestroyBuffer(vkrt.device, stagingBuffer, NULL);
        vkFreeMemory(vkrt.device, stagingMemory, NULL);
        vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
        vkFreeMemory(vkrt.device, *outMemory, NULL);
        *outMemory = VK_NULL_HANDLE;
        *outBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    result = RTX_SubmitImmediateCommands("UploadBuffer");
    if (result != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_CreateBufferWithData: SubmitImmediateCommands failed err=%d\n",
                  result);
        vkDestroyBuffer(vkrt.device, stagingBuffer, NULL);
        vkFreeMemory(vkrt.device, stagingMemory, NULL);
        vkDestroyBuffer(vkrt.device, *outBuffer, NULL);
        vkFreeMemory(vkrt.device, *outMemory, NULL);
        *outMemory = VK_NULL_HANDLE;
        *outBuffer = VK_NULL_HANDLE;
        return qfalse;
    }

    vkDestroyBuffer(vkrt.device, stagingBuffer, NULL);
    vkFreeMemory(vkrt.device, stagingMemory, NULL);

    return qtrue;
}

qboolean RTX_BuildBLASGPU(rtxBLAS_t *blas) {
    if (!blas || blas->gpuData) {
        return blas ? qtrue : qfalse;
    }

    ri.Printf(PRINT_WARNING,
              "RTX_BuildBLASGPU: enter verts=%d tris=%d dynamic=%d device=%p createAS=%p\n",
              blas->numVertices, blas->numTriangles, blas->isDynamic ? 1 : 0,
              (void*)vkrt.device, (void*)qvkCreateAccelerationStructureKHR);

    if (!vkrt.device || !qvkCreateAccelerationStructureKHR) {
        ri.Printf(PRINT_WARNING,
                  "RTX_BuildBLASGPU: missing device (%p) or AS function (%p)\n",
                  (void*)vkrt.device, (void*)qvkCreateAccelerationStructureKHR);
        return qfalse;
    }

    if (blas->numVertices <= 0 || blas->numTriangles <= 0) {
        ri.Printf(PRINT_WARNING, "RTX: Skipping BLAS build for empty geometry (verts=%d tris=%d)\n",
                  blas->numVertices, blas->numTriangles);
        return qfalse;
    }

    VkDeviceSize vertexSize = sizeof(vec3_t) * (VkDeviceSize)blas->numVertices;
    VkDeviceSize indexSize = sizeof(uint32_t) * (VkDeviceSize)blas->numTriangles * 3;

    uint32_t computedMaxIndex = 0;
    uint32_t *indexSrc = blas->indices;
    if (!indexSrc) {
        ri.Printf(PRINT_WARNING, "RTX: BLAS has no index buffer; skipping (%d tris)\n", blas->numTriangles);
        return qfalse;
    }

    const int indexCount = blas->numTriangles * 3;
    for (int i = 0; i < indexCount; ++i) {
        if (indexSrc[i] > computedMaxIndex) {
            computedMaxIndex = indexSrc[i];
        }
    }

    if (computedMaxIndex >= (uint32_t)blas->numVertices) {
        ri.Printf(PRINT_WARNING,
                  "RTX: BLAS index overflow (max=%u verts=%d tris=%d dynamic=%d)\n",
                  computedMaxIndex,
                  blas->numVertices,
                  blas->numTriangles,
                  blas->isDynamic ? 1 : 0);
        if (indexCount > 0) {
            for (int i = 0; i < indexCount; ++i) {
                if (indexSrc[i] >= (uint32_t)blas->numVertices) {
                    uint32_t clamped = (uint32_t)((blas->numVertices > 0) ? (blas->numVertices - 1) : 0);
                    ri.Printf(PRINT_WARNING,
                              "RTX:   clamping index[%d]=%u -> %u (BLAS verts=%d)\n",
                              i, indexSrc[i], clamped, blas->numVertices);
                    indexSrc[i] = clamped;
                }
            }
        }
        computedMaxIndex = (uint32_t)((blas->numVertices > 0) ? (blas->numVertices - 1) : 0);
    }

    ri.Printf(PRINT_ALL,
              "RTX: BLAS build verts=%d tris=%d maxIndex=%u dynamic=%d "
              "aabbMin=(%.2f,%.2f,%.2f) aabbMax=(%.2f,%.2f,%.2f)\n",
              blas->numVertices, blas->numTriangles, computedMaxIndex,
              blas->isDynamic ? 1 : 0,
              blas->aabbMin[0], blas->aabbMin[1], blas->aabbMin[2],
              blas->aabbMax[0], blas->aabbMax[1], blas->aabbMax[2]);
    if (indexCount >= 3) {
        ri.Printf(PRINT_ALL,
                  "RTX:   first triangle indices=(%u,%u,%u)\n",
                  indexSrc[0], indexSrc[1], indexSrc[2]);
    }

    VkMemoryPropertyFlags vertexProps = blas->isDynamic
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkBufferUsageFlags vertexUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (!(vertexProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        vertexUsage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    if (!RTX_CreateBufferWithData(vertexSize,
                                  vertexUsage,
                                  vertexProps,
                                  blas->vertices,
                                  &vertexBuffer, &vertexMemory)) {
        ri.Printf(PRINT_WARNING,
                  "RTX: CreateBufferWithData (vertex) failed size=%llu usage=0x%X props=0x%X\n",
                  (unsigned long long)vertexSize,
                  vertexUsage,
                  vertexProps);
        return qfalse;
    }

    VkMemoryPropertyFlags indexProps = blas->isDynamic
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkBufferUsageFlags indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (!(indexProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        indexUsage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    if (!RTX_CreateBufferWithData(indexSize,
                                  indexUsage,
                                  indexProps,
                                  blas->indices,
                                  &indexBuffer, &indexMemory)) {
        vkDestroyBuffer(vkrt.device, vertexBuffer, NULL);
        vkFreeMemory(vkrt.device, vertexMemory, NULL);
        ri.Printf(PRINT_WARNING,
                  "RTX: CreateBufferWithData (index) failed size=%llu usage=0x%X props=0x%X\n",
                  (unsigned long long)indexSize,
                  indexUsage,
                  indexProps);
        return qfalse;
    }

    VkBuffer materialBuffer = VK_NULL_HANDLE;
    VkDeviceMemory materialMemory = VK_NULL_HANDLE;
    if (blas->triangleMaterials && blas->numTriangles > 0) {
        VkDeviceSize materialSize = sizeof(uint32_t) * (VkDeviceSize)blas->numTriangles;
        VkBufferUsageFlags materialUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        if (!RTX_CreateBufferWithData(materialSize,
                                      materialUsage,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                      blas->triangleMaterials,
                                      &materialBuffer, &materialMemory)) {
            vkDestroyBuffer(vkrt.device, indexBuffer, NULL);
            vkFreeMemory(vkrt.device, indexMemory, NULL);
            vkDestroyBuffer(vkrt.device, vertexBuffer, NULL);
            vkFreeMemory(vkrt.device, vertexMemory, NULL);
            ri.Printf(PRINT_WARNING,
                      "RTX: CreateBufferWithData (material) failed size=%llu usage=0x%X\n",
                      (unsigned long long)materialSize,
                      materialUsage);
            return qfalse;
        }
    }

    // Build interleaved shader vertex buffer (60 bytes/vertex) for BDA access.
    // The closesthit shader reads full Vertex structs via buffer device address,
    // so we must provide position+normal+texcoord+tangent+color per vertex.
    VkDeviceSize shaderVertexSize = sizeof(rtxShaderVertex_t) * (VkDeviceSize)blas->numVertices;
    rtxShaderVertex_t *packedVerts = Z_Malloc((int)shaderVertexSize);
    for (int i = 0; i < blas->numVertices; i++) {
        VectorCopy(blas->vertices[i], packedVerts[i].position);

        if (blas->normals) {
            VectorCopy(blas->normals[i], packedVerts[i].normal);
        } else {
            VectorSet(packedVerts[i].normal, 0.0f, 0.0f, 1.0f);
        }

        if (blas->texCoords) {
            packedVerts[i].texCoord[0] = blas->texCoords[i][0];
            packedVerts[i].texCoord[1] = blas->texCoords[i][1];
        } else {
            packedVerts[i].texCoord[0] = 0.0f;
            packedVerts[i].texCoord[1] = 0.0f;
        }

        // Tangent: default to (1,0,0) — proper tangent generation can be added later
        VectorSet(packedVerts[i].tangent, 1.0f, 0.0f, 0.0f);

        if (blas->colors) {
            Vector4Copy(blas->colors[i], packedVerts[i].color);
        } else {
            packedVerts[i].color[0] = 1.0f;
            packedVerts[i].color[1] = 1.0f;
            packedVerts[i].color[2] = 1.0f;
            packedVerts[i].color[3] = 1.0f;
        }
    }

    VkBuffer shaderVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory shaderVertexMemory = VK_NULL_HANDLE;
    VkBufferUsageFlags shaderVBUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    if (!RTX_CreateBufferWithData(shaderVertexSize,
                                  shaderVBUsage,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                  packedVerts,
                                  &shaderVertexBuffer, &shaderVertexMemory)) {
        Z_Free(packedVerts);
        if (materialBuffer) {
            vkDestroyBuffer(vkrt.device, materialBuffer, NULL);
            vkFreeMemory(vkrt.device, materialMemory, NULL);
        }
        vkDestroyBuffer(vkrt.device, indexBuffer, NULL);
        vkFreeMemory(vkrt.device, indexMemory, NULL);
        vkDestroyBuffer(vkrt.device, vertexBuffer, NULL);
        vkFreeMemory(vkrt.device, vertexMemory, NULL);
        ri.Printf(PRINT_WARNING,
                  "RTX: CreateBufferWithData (shaderVertex) failed size=%llu\n",
                  (unsigned long long)shaderVertexSize);
        return qfalse;
    }
    Z_Free(packedVerts);

    VkDeviceAddress vertexAddress = RTX_GetBufferDeviceAddressVK(vertexBuffer);
    VkDeviceAddress indexAddress = RTX_GetBufferDeviceAddressVK(indexBuffer);

    if (!vertexAddress || !indexAddress) {
        ri.Printf(PRINT_WARNING,
                  "RTX: BLAS GPU address invalid (vertex=0x%llx index=0x%llx verts=%d tris=%d)\n",
                  (unsigned long long)vertexAddress,
                  (unsigned long long)indexAddress,
                  blas->numVertices,
                  blas->numTriangles);
        if (materialBuffer) {
            vkDestroyBuffer(vkrt.device, materialBuffer, NULL);
            vkFreeMemory(vkrt.device, materialMemory, NULL);
        }
        vkDestroyBuffer(vkrt.device, shaderVertexBuffer, NULL);
        vkFreeMemory(vkrt.device, shaderVertexMemory, NULL);
        vkDestroyBuffer(vkrt.device, indexBuffer, NULL);
        vkFreeMemory(vkrt.device, indexMemory, NULL);
        vkDestroyBuffer(vkrt.device, vertexBuffer, NULL);
        vkFreeMemory(vkrt.device, vertexMemory, NULL);
        return qfalse;
    }

    VkAccelerationStructureGeometryTrianglesDataKHR triangles = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .vertexData.deviceAddress = vertexAddress,
        .vertexStride = sizeof(vec3_t),
        .maxVertex = computedMaxIndex,
        .indexType = VK_INDEX_TYPE_UINT32,
        .indexData.deviceAddress = indexAddress,
        .transformData.deviceAddress = 0
    };

    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = triangles,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR
    };

    VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {
        .primitiveCount = (uint32_t)blas->numTriangles,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0
    };

    VkBuffer blasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory blasMemory = VK_NULL_HANDLE;
    VkBuildAccelerationStructureFlagsKHR buildFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (blas->isDynamic) {
        buildFlags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }

    VkAccelerationStructureKHR asHandle = RTX_CreateBLASVulkan(&geometry, &rangeInfo,
                                                                buildFlags,
                                                                &blasBuffer, &blasMemory);

    if (asHandle == VK_NULL_HANDLE) {
        ri.Printf(PRINT_WARNING,
                  "RTX: CreateBLASVulkan returned NULL (verts=%d tris=%d)\n",
                  blas->numVertices, blas->numTriangles);
        vkDestroyBuffer(vkrt.device, shaderVertexBuffer, NULL);
        vkFreeMemory(vkrt.device, shaderVertexMemory, NULL);
        vkDestroyBuffer(vkrt.device, indexBuffer, NULL);
        vkFreeMemory(vkrt.device, indexMemory, NULL);
        vkDestroyBuffer(vkrt.device, vertexBuffer, NULL);
        vkFreeMemory(vkrt.device, vertexMemory, NULL);
        if (materialBuffer) {
            vkDestroyBuffer(vkrt.device, materialBuffer, NULL);
            vkFreeMemory(vkrt.device, materialMemory, NULL);
        }
        return qfalse;
    }

    rtxBLASGPU_t *gpu = Z_Malloc(sizeof(*gpu));
    gpu->as = asHandle;
    gpu->asBuffer = blasBuffer;
    gpu->asMemory = blasMemory;
    gpu->vertexBuffer = vertexBuffer;
    gpu->vertexMemory = vertexMemory;
    gpu->shaderVertexBuffer = shaderVertexBuffer;
    gpu->shaderVertexMemory = shaderVertexMemory;
    gpu->indexBuffer = indexBuffer;
    gpu->indexMemory = indexMemory;
    gpu->materialBuffer = materialBuffer;
    gpu->materialMemory = materialMemory;

    blas->handle = (void*)(uintptr_t)asHandle;
    blas->gpuData = gpu;

    ri.Printf(PRINT_WARNING,
              "RTX_BuildBLASGPU: success verts=%d tris=%d as=%p vb=%p ib=%p mat=%p\n",
              blas->numVertices, blas->numTriangles,
              (void*)asHandle, (void*)vertexBuffer, (void*)indexBuffer, (void*)materialBuffer);

    return qtrue;
}

void RTX_DestroyBLASGPU(rtxBLAS_t *blas) {
    if (!blas || !blas->gpuData) {
        return;
    }

    rtxBLASGPU_t *gpu = (rtxBLASGPU_t *)blas->gpuData;

    if (gpu->as) {
        qvkDestroyAccelerationStructureKHR(vkrt.device, gpu->as, NULL);
    }
    if (gpu->asBuffer) {
        vkDestroyBuffer(vkrt.device, gpu->asBuffer, NULL);
    }
    if (gpu->asMemory) {
        vkFreeMemory(vkrt.device, gpu->asMemory, NULL);
    }
    if (gpu->vertexBuffer) {
        vkDestroyBuffer(vkrt.device, gpu->vertexBuffer, NULL);
    }
    if (gpu->vertexMemory) {
        vkFreeMemory(vkrt.device, gpu->vertexMemory, NULL);
    }
    if (gpu->shaderVertexBuffer) {
        vkDestroyBuffer(vkrt.device, gpu->shaderVertexBuffer, NULL);
    }
    if (gpu->shaderVertexMemory) {
        vkFreeMemory(vkrt.device, gpu->shaderVertexMemory, NULL);
    }
    if (gpu->indexBuffer) {
        vkDestroyBuffer(vkrt.device, gpu->indexBuffer, NULL);
    }
    if (gpu->indexMemory) {
        vkFreeMemory(vkrt.device, gpu->indexMemory, NULL);
    }
    if (gpu->materialBuffer) {
        vkDestroyBuffer(vkrt.device, gpu->materialBuffer, NULL);
    }
    if (gpu->materialMemory) {
        vkFreeMemory(vkrt.device, gpu->materialMemory, NULL);
    }

    Z_Free(gpu);
    blas->gpuData = NULL;
    blas->handle = NULL;
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
    ri.Printf(PRINT_DEVELOPER, "RTX: Building TLAS for %d instances\n", rtx.tlas.numInstances);
    
    // Build instance data
    VkAccelerationStructureInstanceKHR *instances = Z_Malloc(
        sizeof(VkAccelerationStructureInstanceKHR) * rtx.tlas.numInstances);
    rtxInstanceGpuData_t *gpuInstances = Z_Malloc(
        sizeof(rtxInstanceGpuData_t) * rtx.tlas.numInstances);

    uint32_t totalTriangleMaterials = 0;
    for (int i = 0; i < rtx.tlas.numInstances; i++) {
        rtxInstance_t *inst = &rtx.tlas.instances[i];
        if (inst->blas) {
            totalTriangleMaterials += inst->blas->numTriangles;
        }
    }

    uint32_t *triangleMaterialAtlas = NULL;
    if (totalTriangleMaterials > 0) {
        triangleMaterialAtlas = Z_Malloc(sizeof(uint32_t) * totalTriangleMaterials);
    }
    uint32_t currentMaterialOffset = 0;
    
#define RTX_CLEANUP_INSTANCE_TEMPORARIES()                          \
    do {                                                            \
        if (triangleMaterialAtlas) {                                \
            Z_Free(triangleMaterialAtlas);                          \
            triangleMaterialAtlas = NULL;                           \
        }                                                           \
        if (gpuInstances) {                                         \
            Z_Free(gpuInstances);                                   \
            gpuInstances = NULL;                                    \
        }                                                           \
        if (instances) {                                            \
            Z_Free(instances);                                      \
            instances = NULL;                                       \
        }                                                           \
    } while (0)
    
    for (int i = 0; i < rtx.tlas.numInstances; i++) {
        rtxInstance_t *inst = &rtx.tlas.instances[i];
        VkAccelerationStructureInstanceKHR *vkInst = &instances[i];
        rtxInstanceGpuData_t *gpuInst = &gpuInstances[i];

        Com_Memset(gpuInst, 0, sizeof(*gpuInst));

        // Copy transform matrix (3x4 row-major)
        Com_Memcpy(vkInst->transform.matrix, inst->transform, sizeof(float) * 12);

        inst->triangleMaterialOffset = currentMaterialOffset;
        inst->triangleMaterialCount = (inst->blas) ? inst->blas->numTriangles : 0;

        if (inst->triangleMaterialCount > 0 && triangleMaterialAtlas) {
            if (inst->blas && inst->blas->triangleMaterials) {
                Com_Memcpy(triangleMaterialAtlas + currentMaterialOffset,
                           inst->blas->triangleMaterials,
                           sizeof(uint32_t) * inst->triangleMaterialCount);
            } else {
                Com_Memset(triangleMaterialAtlas + currentMaterialOffset, 0,
                           sizeof(uint32_t) * inst->triangleMaterialCount);
            }
        }
        // Use the instance's index for gl_InstanceCustomIndexEXT so the shader
        // can safely index the instance data buffer (binding 10).  Using the
        // triangleMaterialOffset here caused the shader to read far past the
        // end of the buffer, producing invalid vertex/index buffer addresses
        // and eventually a device fault (VK_ERROR_DEVICE_LOST).
        vkInst->instanceCustomIndex = i;
        currentMaterialOffset += inst->triangleMaterialCount;
        vkInst->mask = inst->mask;
        vkInst->instanceShaderBindingTableRecordOffset = inst->shaderOffset;
        vkInst->flags = inst->flags | VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;

        // Get BLAS device address
        if (inst->blas) {
            if (!inst->blas->gpuData) {
                RTX_BuildBLASGPU(inst->blas);
            }

            if (inst->blas->gpuData && inst->blas->handle) {
            VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = (VkAccelerationStructureKHR)inst->blas->handle
            };
            vkInst->accelerationStructureReference = 
                qvkGetAccelerationStructureDeviceAddressKHR(vkrt.device, &addressInfo);
            if (vkInst->accelerationStructureReference == 0) {
                ri.Printf(PRINT_WARNING, "RTX: BLAS %d returned device address 0 (handle=%p)\n",
                    i, inst->blas->handle);
            }
            } else {
                vkInst->accelerationStructureReference = 0;
            }

            if (inst->blas->gpuData) {
                rtxBLASGPU_t *gpu = (rtxBLASGPU_t *)inst->blas->gpuData;
                // Point BDA to the shader vertex buffer (60 bytes/vertex) which
                // matches the Vertex struct in closesthit.rchit, NOT the
                // position-only buffer used for BLAS AS construction.
                if (gpu->shaderVertexBuffer != VK_NULL_HANDLE) {
                    gpuInst->vertexBufferAddress = RTX_GetBufferDeviceAddressVK(gpu->shaderVertexBuffer);
                }
                if (gpu->indexBuffer != VK_NULL_HANDLE) {
                    gpuInst->indexBufferAddress = RTX_GetBufferDeviceAddressVK(gpu->indexBuffer);
                }
            }
        } else {
            vkInst->accelerationStructureReference = 0;
        }

        if (vkInst->accelerationStructureReference == 0) {
            ri.Printf(PRINT_WARNING, "RTX: Instance %d has no acceleration structure reference (blas=%p)\n",
                i, (void*)inst->blas);
        }

        // World batches shade per-triangle via the material atlas; the
        // whole-instance materialIndex is the fallback for geometry without
        // per-triangle materials (index 0 = default material).
        gpuInst->materialIndex = 0;
        gpuInst->triangleMaterialOffset = inst->triangleMaterialOffset;
        gpuInst->triangleMaterialCount = inst->triangleMaterialCount;
        gpuInst->instanceFlags = 0;
        for (int m = 0; m < 16; ++m) {
            gpuInst->normalMatrix[m] = (m % 5 == 0) ? 1.0f : 0.0f;
        }
        for (int m = 0; m < 4; ++m) {
            gpuInst->customData[m] = 0.0f;
        }

        if (gpuInst->vertexBufferAddress == 0 || gpuInst->indexBufferAddress == 0) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Instance %d missing GPU buffer addresses (vertex=0x%llx index=0x%llx)\n",
                      i,
                      (unsigned long long)gpuInst->vertexBufferAddress,
                      (unsigned long long)gpuInst->indexBufferAddress);
        }
    }

    RTX_UpdateInstanceDataBuffer(gpuInstances, rtx.tlas.numInstances);
    
    // Create or update instance buffer
    size_t instanceDataSize = sizeof(VkAccelerationStructureInstanceKHR) * rtx.tlas.numInstances;
    
    if (!vkrt.instanceBuffer) {
        // Create instance buffer
        // A number of NVIDIA drivers have been observed to write a little past the
        // reported VkMemoryRequirements for the instance buffer during TLAS builds,
        // which can trigger WRITE_INVALID device faults on small buffers (few
        // instances).  Pad the backing allocation generously to give the driver
        // headroom while keeping the exposed buffer size unchanged.
        const VkDeviceSize instanceGuard = 1024 * 1024; // 1 MiB safety margin

        VkBufferCreateInfo bufferInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = instanceDataSize,
            .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT
        };
        
        if (vkCreateBuffer(vkrt.device, &bufferInfo, NULL, &vkrt.instanceBuffer) != VK_SUCCESS) {
            RTX_CLEANUP_INSTANCE_TEMPORARIES();
            return;
        }

        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Created TLAS instance buffer %p (%llu bytes)\n",
                      (void*)vkrt.instanceBuffer,
                      (unsigned long long)instanceDataSize);
        }
        
        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(vkrt.device, vkrt.instanceBuffer, &memReqs);
        
        VkMemoryAllocateFlagsInfo memoryAllocateFlagsInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
        };
        
        VkDeviceSize guardAligned = (instanceGuard + memReqs.alignment - 1) &
                                    ~(memReqs.alignment - 1);

        VkMemoryAllocateInfo allocInfo = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &memoryAllocateFlagsInfo,
            // pad the allocation to tolerate driver overfetch/overwrites reported in
            // device fault captures (WRITE_INVALID at instanceBuffer+0x1000).
            .allocationSize = memReqs.size + guardAligned,
            .memoryTypeIndex = RTX_FindMemoryType(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        };
        
        if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, &vkrt.instanceMemory) != VK_SUCCESS) {
            vkDestroyBuffer(vkrt.device, vkrt.instanceBuffer, NULL);
            vkrt.instanceBuffer = VK_NULL_HANDLE;
            RTX_CLEANUP_INSTANCE_TEMPORARIES();
            return;
        }
        
        vkBindBufferMemory(vkrt.device, vkrt.instanceBuffer, vkrt.instanceMemory, 0);

        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: Bound TLAS instance memory %p\n",
                      (void*)vkrt.instanceMemory);
        }
    }
    
    // Upload instance data
    void *data;
    vkMapMemory(vkrt.device, vkrt.instanceMemory, 0, instanceDataSize, 0, &data);
    Com_Memcpy(data, instances, instanceDataSize);
    vkUnmapMemory(vkrt.device, vkrt.instanceMemory);
    
    Z_Free(instances);
    instances = NULL;
    if (gpuInstances) {
        Z_Free(gpuInstances);
        gpuInstances = NULL;
    }
    
    // Setup TLAS geometry
    VkDeviceAddress instanceBufferAddress = RTX_GetBufferDeviceAddressVK(vkrt.instanceBuffer);
    if (instanceBufferAddress == 0) {
        ri.Printf(PRINT_WARNING,
                  "RTX: Instance buffer device address is zero (instance count=%d); aborting TLAS build\n",
                  rtx.tlas.numInstances);
        RTX_CLEANUP_INSTANCE_TEMPORARIES();
        return;
    }
    
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
    
    int buildIndex = (vkrt.activeTLAS + 1) & 1;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                 VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
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

    qboolean needsAllocate = (vkrt.tlas[buildIndex] == VK_NULL_HANDLE) ||
                             (sizeInfo.accelerationStructureSize > rtx.tlas.scratchSize);

    if (needsAllocate) {
        if (vkrt.tlas[buildIndex]) {
            qvkDestroyAccelerationStructureKHR(vkrt.device, vkrt.tlas[buildIndex], NULL);
            vkrt.tlas[buildIndex] = VK_NULL_HANDLE;
        }
        if (vkrt.tlasBuffer[buildIndex]) {
            vkDestroyBuffer(vkrt.device, vkrt.tlasBuffer[buildIndex], NULL);
            vkrt.tlasBuffer[buildIndex] = VK_NULL_HANDLE;
        }
        if (vkrt.tlasMemory[buildIndex]) {
            vkFreeMemory(vkrt.device, vkrt.tlasMemory[buildIndex], NULL);
            vkrt.tlasMemory[buildIndex] = VK_NULL_HANDLE;
        }

        VkBufferCreateInfo bufferInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeInfo.accelerationStructureSize,
            .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        };

        if (vkCreateBuffer(vkrt.device, &bufferInfo, NULL, &vkrt.tlasBuffer[buildIndex]) != VK_SUCCESS) {
            RTX_CLEANUP_INSTANCE_TEMPORARIES();
            return;
        }

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(vkrt.device, vkrt.tlasBuffer[buildIndex], &memReqs);

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

        // Pad TLAS allocations slightly to avoid driver underestimation.
    const VkDeviceSize tlasPad = 1024 * 1024; // 1 MiB safety margin
        VkDeviceSize alignedTlasPad = (tlasPad + memReqs.alignment - 1) &
                                      ~(memReqs.alignment - 1);
        allocInfo.allocationSize += alignedTlasPad;
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RTX: TLAS alloc size=%llu (raw=%llu pad=%llu align=%llu)\n",
                      (unsigned long long)allocInfo.allocationSize,
                      (unsigned long long)memReqs.size,
                      (unsigned long long)alignedTlasPad,
                      (unsigned long long)memReqs.alignment);
        }

        if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, &vkrt.tlasMemory[buildIndex]) != VK_SUCCESS) {
            vkDestroyBuffer(vkrt.device, vkrt.tlasBuffer[buildIndex], NULL);
            vkrt.tlasBuffer[buildIndex] = VK_NULL_HANDLE;
            RTX_CLEANUP_INSTANCE_TEMPORARIES();
            return;
        }

        vkBindBufferMemory(vkrt.device, vkrt.tlasBuffer[buildIndex], vkrt.tlasMemory[buildIndex], 0);

        VkAccelerationStructureCreateInfoKHR createInfo = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = vkrt.tlasBuffer[buildIndex],
            .size = sizeInfo.accelerationStructureSize,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
        };

        if (qvkCreateAccelerationStructureKHR(vkrt.device, &createInfo, NULL, &vkrt.tlas[buildIndex]) != VK_SUCCESS) {
            vkFreeMemory(vkrt.device, vkrt.tlasMemory[buildIndex], NULL);
            vkDestroyBuffer(vkrt.device, vkrt.tlasBuffer[buildIndex], NULL);
            vkrt.tlasBuffer[buildIndex] = VK_NULL_HANDLE;
            vkrt.tlasMemory[buildIndex] = VK_NULL_HANDLE;
            return;
        }

        rtx.tlas.scratchSize = sizeInfo.accelerationStructureSize;
    }
    
    // Allocate scratch buffer
    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    scratchBuffer = RTX_AllocateScratchBuffer(sizeInfo.buildScratchSize, &scratchMemory);
    
    if (!scratchBuffer) {
        RTX_CLEANUP_INSTANCE_TEMPORARIES();
        return;
    }
    
    // Build TLAS
    if (RTX_BeginImmediateCommands("BuildTLAS") != VK_SUCCESS) {
        RTX_CLEANUP_INSTANCE_TEMPORARIES();
        return;
    }

    RTX_UploadTriangleMaterials(vkrt.commandBuffer,
                                triangleMaterialAtlas,
                                totalTriangleMaterials);
    
    // Build range info
    VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {
        .primitiveCount = rtx.tlas.numInstances,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0
    };
    
    buildInfo.dstAccelerationStructure = vkrt.tlas[buildIndex];
    buildInfo.scratchData.deviceAddress = RTX_GetBufferDeviceAddressVK(scratchBuffer);
    if (buildInfo.scratchData.deviceAddress == 0) {
        ri.Printf(PRINT_WARNING,
                  "RTX: Scratch buffer device address is zero; aborting TLAS build (instances=%d)\n",
                  rtx.tlas.numInstances);
        RTX_CLEANUP_INSTANCE_TEMPORARIES();
        return;
    }
    
    const VkAccelerationStructureBuildRangeInfoKHR *rangeInfos[] = { &rangeInfo };
    qvkCmdBuildAccelerationStructuresKHR(vkrt.commandBuffer, 1, &buildInfo, rangeInfos);

    ri.Printf(PRINT_ALL,
              "RTX: TLAS build enqueued (dstIndex=%d instances=%d scratch=0x%llx instanceBuffer=0x%llx)\n",
              buildIndex,
              rtx.tlas.numInstances,
              (unsigned long long)buildInfo.scratchData.deviceAddress,
              (unsigned long long)instanceBufferAddress);
    
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

    if (vkEndCommandBuffer(vkrt.commandBuffer) != VK_SUCCESS) {
        RTX_CLEANUP_INSTANCE_TEMPORARIES();
        return;
    }

    if (RTX_SubmitImmediateCommands("BuildTLAS") != VK_SUCCESS) {
        RTX_CLEANUP_INSTANCE_TEMPORARIES();
        return;
    }

    // Clean up scratch buffer
    
    RTX_CLEANUP_INSTANCE_TEMPORARIES();

    rtx.buildTime = ri.Milliseconds() - startTime;
    vkrt.activeTLAS = buildIndex;

    ri.Printf(PRINT_ALL, "RTX: BuildTLAS completed (active index=%d)\n", vkrt.activeTLAS);
    if (!qvkGetAccelerationStructureDeviceAddressKHR) {
        ri.Printf(PRINT_ALL, "RTX: TLAS[%d] device address unavailable (function not loaded)\n",
                  vkrt.activeTLAS);
    } else {
        VkAccelerationStructureDeviceAddressInfoKHR tlasAddrInfo = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = vkrt.tlas[vkrt.activeTLAS]
        };
        VkDeviceAddress tlasDeviceAddress = qvkGetAccelerationStructureDeviceAddressKHR(vkrt.device, &tlasAddrInfo);
        ri.Printf(PRINT_ALL,
                  "RTX: TLAS[%d] device address=0x%llx size=%llu\n",
                  vkrt.activeTLAS,
                  (unsigned long long)tlasDeviceAddress,
                  (unsigned long long)sizeInfo.accelerationStructureSize);
    }

    VkAccelerationStructureKHR activeTLAS = vkrt.tlas[vkrt.activeTLAS];
    rtx.tlas.handle = (void*)(uintptr_t)activeTLAS;
    rtx.tlas.handles[vkrt.activeTLAS] = rtx.tlas.handle;
    rtx.tlas.activeHandle = vkrt.activeTLAS;
    rtx.tlas.needsRebuild = qfalse;
}

#undef RTX_CLEANUP_INSTANCE_TEMPORARIES

/*
================
RTX_ReportDispatchFailure

Emit detailed state when a dispatch submission fails so we can diagnose
device-loss issues from logs.
================
*/
static void RTX_ReportDispatchFailure(const rtxDispatchRays_t *params,
                                      uint32_t dispatchWidth,
                                      uint32_t dispatchHeight,
                                      VkAccelerationStructureKHR activeTLAS,
                                      const VkStridedDeviceAddressRegionKHR *raygenRegion,
                                      const VkStridedDeviceAddressRegionKHR *missRegion,
                                      const VkStridedDeviceAddressRegionKHR *hitRegion,
                                      VkResult submitResult) {
    ri.Printf(PRINT_WARNING,
              "RTX: Dispatch submission failed (result=%d) params=(%d x %d x %d) dispatch=(%u x %u)\n",
              submitResult,
              params ? params->width : -1,
              params ? params->height : -1,
              params ? params->depth : -1,
              dispatchWidth,
              dispatchHeight);

    ri.Printf(PRINT_WARNING,
              "     TLAS=%p activeTLASIndex=%d fenceSubmitted=%d deviceLost=%d commandBuffer=%p\n",
              (void*)activeTLAS,
              vkrt.activeTLAS,
              vkrt.fenceSubmitted ? 1 : 0,
              vkrt.deviceLost ? 1 : 0,
              (void*)vkrt.commandBuffer);

    VkDeviceAddress instanceAddr = RTX_GetBufferDeviceAddressVK(vkrt.instanceBuffer);
    ri.Printf(PRINT_WARNING,
              "     vkrt.instanceBuffer=%p deviceAddr=0x%llx memory=%p\n",
              (void*)vkrt.instanceBuffer,
              (unsigned long long)instanceAddr,
              (void*)vkrt.instanceMemory);

    VkBuffer lightBuffer = RT_GetSceneLightBuffer();
    VkDeviceSize lightSize = RT_GetSceneLightBufferSize();
    ri.Printf(PRINT_WARNING,
              "     SceneLightBuffer=%p size=%llu\n",
              (void*)lightBuffer,
              (unsigned long long)lightSize);

    ri.Printf(PRINT_WARNING,
              "     RT output image=%p view=%p storedSize=%ux%u\n",
              (void*)vkrt.rtImage,
              (void*)vkrt.rtImageView,
              rtOutputWidth,
              rtOutputHeight);

    if (raygenRegion) {
        ri.Printf(PRINT_WARNING,
                  "     SBT raygen addr=0x%llx stride=0x%llx size=0x%llx\n",
                  (unsigned long long)raygenRegion->deviceAddress,
                  (unsigned long long)raygenRegion->stride,
                  (unsigned long long)raygenRegion->size);
    }

    if (missRegion) {
        ri.Printf(PRINT_WARNING,
                  "     SBT miss   addr=0x%llx stride=0x%llx size=0x%llx\n",
                  (unsigned long long)missRegion->deviceAddress,
                  (unsigned long long)missRegion->stride,
                  (unsigned long long)missRegion->size);
    }

    if (hitRegion) {
        ri.Printf(PRINT_WARNING,
                  "     SBT hit    addr=0x%llx stride=0x%llx size=0x%llx\n",
                  (unsigned long long)hitRegion->deviceAddress,
                  (unsigned long long)hitRegion->stride,
                  (unsigned long long)hitRegion->size);
    }
}

/*
================
RTX_DispatchRaysVK

Record the ray tracing dispatch into the frame command buffer. The trace,
its uniform updates, and the subsequent framebuffer copy all execute in
submission order within the frame — no mid-frame submits or fence waits.

The only exception is GPU validation/readback (rt_gpuValidate or
rtx_debug_force_readback): those need the traced pixels on the CPU the same
frame, so they run on the immediate command buffer with a blocking wait.
That is an explicitly synchronous diagnostic path.
================
*/
void RTX_DispatchRaysVK(VkCommandBuffer frameCmd, const rtxDispatchRays_t *params) {
    if (!vkrt.device || !rtx.tlas.numInstances) {
        return;
    }

    if (vkrt.deviceLost) {
        return;
    }

    float startTime = ri.Milliseconds();
    const qboolean wantsGpuValidation = (rt_gpuValidate && rt_gpuValidate->integer > 0);
    const qboolean wantsDebugReadback = (rtx_debug_force_readback && rtx_debug_force_readback->integer > 0);
    const qboolean wantsReadback = wantsGpuValidation || wantsDebugReadback;
    qboolean recordedReadback = qfalse;
    uint32_t readbackWidth = 0;
    uint32_t readbackHeight = 0;

    // Get pipeline and descriptor set from pipeline system
    VkPipeline rtPipeline = RTX_GetPipeline();
    VkPipelineLayout pipelineLayout = RTX_GetPipelineLayout();
    VkDescriptorSet descriptorSet = RTX_GetDescriptorSet();
    VkAccelerationStructureKHR activeTLAS = vkrt.tlas[vkrt.activeTLAS];

    if (!rtPipeline || !pipelineLayout || !descriptorSet || activeTLAS == VK_NULL_HANDLE) {
        ri.Printf(PRINT_WARNING, "RTX: Pipeline not properly initialized\n");
        return;
    }

    uint32_t dispatchWidth = (params->width > 0) ? (uint32_t)params->width : rtOutputWidth;
    uint32_t dispatchHeight = (params->height > 0) ? (uint32_t)params->height : rtOutputHeight;
    uint32_t dispatchDepth = (params->depth > 0) ? (uint32_t)params->depth : 1u;
    float dispatchScale = 1.0f;
    if (rtx_debug_dispatch_scale) {
        dispatchScale = rtx_debug_dispatch_scale->value;
    }
    if (dispatchScale > 0.0f && dispatchScale < 0.999f) {
        uint32_t scaledWidth = (uint32_t)floorf(dispatchWidth * dispatchScale);
        uint32_t scaledHeight = (uint32_t)floorf(dispatchHeight * dispatchScale);
        if (scaledWidth == 0u) {
            scaledWidth = 1u;
        }
        if (scaledHeight == 0u) {
            scaledHeight = 1u;
        }
        if (scaledWidth != dispatchWidth || scaledHeight != dispatchHeight) {
            if (r_rtx_debug && r_rtx_debug->integer >= 1) {
                ri.Printf(PRINT_WARNING,
                          "RTX: Dispatch scaled by %.2f -> %ux%u (was %ux%u)\n",
                          dispatchScale,
                          scaledWidth, scaledHeight,
                          dispatchWidth, dispatchHeight);
            }
            dispatchWidth = scaledWidth;
            dispatchHeight = scaledHeight;
        }
    }
    uint64_t maxInvocations = vkrt.rtProperties.maxRayDispatchInvocationCount;
    if (maxInvocations > 0) {
        uint64_t invocationCount = (uint64_t)dispatchWidth * dispatchHeight * dispatchDepth;
        if (invocationCount > maxInvocations) {
            double scale = sqrt((double)maxInvocations / (double)invocationCount);
            uint32_t clampedWidth = (uint32_t)floor((double)dispatchWidth * scale);
            uint32_t clampedHeight = (uint32_t)floor((double)dispatchHeight * scale);

            if (clampedWidth == 0u) {
                clampedWidth = 1u;
            }
            if (clampedHeight == 0u) {
                clampedHeight = 1u;
            }

            while ((uint64_t)clampedWidth * clampedHeight * dispatchDepth > maxInvocations &&
                   (clampedWidth > 1u || clampedHeight > 1u)) {
                if (clampedWidth >= clampedHeight && clampedWidth > 1u) {
                    --clampedWidth;
                } else if (clampedHeight > 1u) {
                    --clampedHeight;
                } else {
                    break;
                }
            }

            if (r_rtx_debug && r_rtx_debug->integer >= 1) {
                ri.Printf(PRINT_WARNING,
                          "RTX: Clamping dispatch dimensions from %ux%u to %ux%u (depth=%u, max invocations=%llu)\n",
                          dispatchWidth, dispatchHeight,
                          clampedWidth, clampedHeight,
                          dispatchDepth,
                          (unsigned long long)maxInvocations);
            }

            dispatchWidth = clampedWidth;
            dispatchHeight = clampedHeight;
        }
    }
    if (dispatchWidth == 0 || dispatchHeight == 0) {
        ri.Printf(PRINT_WARNING, "RTX: Invalid dispatch dimensions (%d x %d); skipping\n",
                  params->width, params->height);
        return;
    }

    if (vkrt.rtImageView == VK_NULL_HANDLE ||
        rtOutputWidth != dispatchWidth ||
        rtOutputHeight != dispatchHeight) {
        if (!RTX_CreateRTOutputImages(dispatchWidth, dispatchHeight)) {
            ri.Printf(PRINT_WARNING, "RTX: Unable to create RT output image (%ux%u); deferring dispatch\n",
                      dispatchWidth, dispatchHeight);
            return;
        }
        rtOutputWidth = dispatchWidth;
        rtOutputHeight = dispatchHeight;
        rtOutputInitialized = qfalse;
    }

    // Validation/readback needs the traced pixels on the CPU this frame, and
    // diagnostic callers have no frame command buffer at all; both cases
    // record on the immediate command buffer and block on its fence. The
    // normal path records straight into the frame command buffer.
    const qboolean immediate = (wantsReadback || frameCmd == VK_NULL_HANDLE);
    VkCommandBuffer cmd;
    if (immediate) {
        if (RTX_BeginImmediateCommands("DispatchRays") != VK_SUCCESS) {
            ri.Printf(PRINT_WARNING, "RTX: Failed to begin dispatch command buffer\n");
            return;
        }
        cmd = vkrt.commandBuffer;
    } else {
        cmd = frameCmd;
    }

    // Refresh per-frame uniform data so the shader sees current camera/lights
    RTX_PrepareFrameData(cmd);

    // Update descriptor sets with current TLAS and output images (no-op
    // unless a bound resource changed)
    RTX_UpdateDescriptorSets(activeTLAS, vkrt.rtImageView, vkrt.albedoImageView,
                            vkrt.normalImageView, vkrt.motionImageView, vkrt.depthImageView);
    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        RTX_DebugLogDescriptorState("DispatchRays");
    }

    // Transition RT output image and G-buffer images to general layout
    if (vkrt.rtImage) {
        VkImageMemoryBarrier imageBarriers[7];
        int barrierCount = 0;
        VkImage gbufferImages[7] = {
            vkrt.rtImage, vkrt.albedoImage, vkrt.normalImage,
            vkrt.motionImage, vkrt.depthImage,
            vkrt.directLightImage, vkrt.indirectLightImage
        };
        for (int i = 0; i < 7; i++) {
            if (gbufferImages[i] == VK_NULL_HANDLE) continue;
            // Only the color image (index 0) is read back via transfer; G-buffers stay GENERAL
            qboolean isColorImage = (i == 0);
            // Color image: after transfer readback its layout is TRANSFER_SRC_OPTIMAL;
            // on first use (or after reset) the layout is unknown so use UNDEFINED.
            // G-buffer images always stay in GENERAL layout between dispatches.
            VkImageLayout oldLayout;
            VkAccessFlags srcAccess;
            if (isColorImage && rtOutputInitialized) {
                oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
            } else if (isColorImage) {
                oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                srcAccess = 0;
            } else {
                oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                srcAccess = 0;
            }
            imageBarriers[barrierCount++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = srcAccess,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = oldLayout,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = gbufferImages[i],
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                }
            };
        }

        vkCmdPipelineBarrier(cmd,
            rtOutputInitialized ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, NULL, 0, NULL, barrierCount, imageBarriers);
    }

    // Bind ray tracing pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipeline);

    // Bind descriptor sets
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
    
    // Get shader binding table regions
    VkStridedDeviceAddressRegionKHR raygenRegion, missRegion, hitRegion, callableRegion;
    RTX_GetSBTRegions(&raygenRegion, &missRegion, &hitRegion, &callableRegion);
    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: SBT regions - raygen(addr=0x%llx stride=%llu size=%llu) "
                  "miss(addr=0x%llx stride=%llu size=%llu) "
                  "hit(addr=0x%llx stride=%llu size=%llu) "
                  "callable(addr=0x%llx stride=%llu size=%llu)\n",
                  (unsigned long long)raygenRegion.deviceAddress,
                  (unsigned long long)raygenRegion.stride,
                  (unsigned long long)raygenRegion.size,
                  (unsigned long long)missRegion.deviceAddress,
                  (unsigned long long)missRegion.stride,
                  (unsigned long long)missRegion.size,
                  (unsigned long long)hitRegion.deviceAddress,
                  (unsigned long long)hitRegion.stride,
                  (unsigned long long)hitRegion.size,
                  (unsigned long long)callableRegion.deviceAddress,
                  (unsigned long long)callableRegion.stride,
                  (unsigned long long)callableRegion.size);
    }
    
    qboolean skippedTrace = qfalse;
    // Dispatch rays
    if (rtx_debug_skip_trace && rtx_debug_skip_trace->integer > 0) {
        // Runtime diagnostic: clear to magenta instead of tracing to isolate crash source
        VkClearColorValue clearColor = { .float32 = {1.0f, 0.0f, 1.0f, 1.0f} };
        VkImageSubresourceRange clearRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        };
        vkCmdClearColorImage(cmd, vkrt.rtImage, VK_IMAGE_LAYOUT_GENERAL,
                             &clearColor, 1, &clearRange);
        skippedTrace = qtrue;
    } else {
        qvkCmdTraceRaysKHR(cmd,
                           &raygenRegion, &missRegion, &hitRegion, &callableRegion,
                           dispatchWidth, dispatchHeight, dispatchDepth);
    }
    
    // The raygen writes linear HDR radiance; the despeckle chain filters it
    // and applies tonemap + gamma, then the depth-aware composite merges
    // rasterized dynamic content (entities, weapon, pickups). All of this
    // happens while the traced image is still in GENERAL layout.
    qboolean compositeRan = qfalse;
    if (!skippedTrace) {
        VkMemoryBarrier traceToCompute = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
        };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &traceToCompute, 0, NULL, 0, NULL);

        // Temporal accumulation first (converts radiance to illumination),
        // then the spatial chain. Both are skipped on the immediate path so
        // the CPU readback sees the raw (but still tonemapped) trace.
        qboolean temporalRan = qfalse;
        if (!immediate && rt_temporal && rt_temporal->integer) {
            temporalRan = RTX_RecordTemporal(cmd, dispatchWidth, dispatchHeight);
            if (temporalRan) {
                VkMemoryBarrier temporalToDenoise = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                };
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &temporalToDenoise, 0, NULL, 0, NULL);
            }
        }

        qboolean wantFilter = (!immediate && rt_denoise && rt_denoise->integer);
        RTX_RecordDenoise(cmd, dispatchWidth, dispatchHeight, wantFilter, temporalRan);

        if (!immediate) {
            VkMemoryBarrier denoiseToComposite = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
            };
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &denoiseToComposite, 0, NULL, 0, NULL);

            RTX_RecordDepthAwareComposite(cmd, dispatchWidth, dispatchHeight);
        }
        compositeRan = qtrue;
    }

    // Transition RT output image for transfer/presentation
    // Use correct stage/access masks depending on what wrote it last
    if (vkrt.rtImage) {
        VkPipelineStageFlags srcStage;
        VkAccessFlags srcAccess;
        if (compositeRan) {
            srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
        } else if (skippedTrace) {
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
        } else {
            srcStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
            srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
        }

        VkImageMemoryBarrier imageBarrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcAccess,
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
        
        vkCmdPipelineBarrier(cmd,
            srcStage,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &imageBarrier);

        if (wantsReadback) {
            recordedReadback = RTX_DownloadColorBuffer(dispatchWidth, dispatchHeight);
            if (recordedReadback) {
                readbackWidth = dispatchWidth;
                readbackHeight = dispatchHeight;
            } else if (r_rtx_debug && r_rtx_debug->integer >= 1) {
                ri.Printf(PRINT_WARNING,
                          "RTX: Readback request failed (%ux%u, swap=%dx%d)\n",
                          dispatchWidth, dispatchHeight,
                          glConfig.vidWidth, glConfig.vidHeight);
            }
        }
    }

    if (immediate) {
        // Diagnostic path: submit now and block so the readback data is valid.
        if (vkEndCommandBuffer(vkrt.commandBuffer) != VK_SUCCESS) {
            ri.Printf(PRINT_WARNING, "RTX: Failed to finalize dispatch commands\n");
            return;
        }

        VkResult submitResult = RTX_SubmitImmediateCommands("DispatchRays");
        if (submitResult != VK_SUCCESS) {
            RTX_ReportDispatchFailure(params, dispatchWidth, dispatchHeight,
                                      activeTLAS, &raygenRegion, &missRegion,
                                      &hitRegion, submitResult);
            return;
        }

        if (recordedReadback && vkrt.readbackMapped) {
            RT_ProcessGpuFrame((const float *)vkrt.readbackMapped,
                               (int)readbackWidth,
                               (int)readbackHeight);
        }
    }

    rtx.traceTime = ri.Milliseconds() - startTime;

    rtOutputInitialized = qtrue;
    rtOutputWidth = dispatchWidth;
    rtOutputHeight = dispatchHeight;

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER, "RTX: Ray dispatch recorded (%ux%u, %.2fms CPU)\n",
                  dispatchWidth, dispatchHeight, rtx.traceTime);
    }
}

void RTX_WaitForCompletion_Impl(void) {
    if (vkrt.fence != VK_NULL_HANDLE) {
        VkResult waitRes = vkWaitForFences(vkrt.device, 1, &vkrt.fence, VK_TRUE, 2000000000ULL);
        if (waitRes == VK_SUCCESS) {
            vkrt.fenceSubmitted = qfalse;
        } else if (waitRes == VK_TIMEOUT) {
            ri.Printf(PRINT_WARNING, "RTX: WaitForCompletion timed out\n");
        }
    }
}

/*
================
RTX_AllocateScratchBuffer

Allocate scratch buffer for acceleration structure builds
================
*/
static VkBuffer RTX_AllocateScratchBuffer(VkDeviceSize size, VkDeviceMemory *memory) {
    // If existing buffer is large enough, reuse it
    if (vkrt.scratchBuffer != VK_NULL_HANDLE && vkrt.scratchSize >= size) {
        *memory = vkrt.scratchMemory;
        return vkrt.scratchBuffer;
    }

    // Destroy old buffer if it exists but is too small
    if (vkrt.scratchBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vkrt.device, vkrt.scratchBuffer, NULL);
        vkFreeMemory(vkrt.device, vkrt.scratchMemory, NULL);
        vkrt.scratchBuffer = VK_NULL_HANDLE;
        vkrt.scratchMemory = VK_NULL_HANDLE;
        vkrt.scratchSize = 0;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };
    
    VkResult createRes = vkCreateBuffer(vkrt.device, &bufferInfo, NULL, &buffer);
    if (createRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_AllocateScratchBuffer: vkCreateBuffer(size=%llu) failed err=%d\n",
                  (unsigned long long)bufferInfo.size,
                  createRes);
        return VK_NULL_HANDLE;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vkrt.device, buffer, &memReqs);
    
    VkMemoryAllocateFlagsInfo memoryAllocateFlagsInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    
    // Pad scratch allocations to tolerate driver underestimation.
    const VkDeviceSize scratchPad = 1024 * 1024; // 1 MiB safety margin
    VkDeviceSize padAligned = (scratchPad + memReqs.alignment - 1) &
                              ~(memReqs.alignment - 1);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &memoryAllocateFlagsInfo,
        .allocationSize = memReqs.size + padAligned,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits, 
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    VkResult allocRes = vkAllocateMemory(vkrt.device, &allocInfo, NULL, &vkrt.scratchMemory);
    if (allocRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_AllocateScratchBuffer: vkAllocateMemory(size=%llu typeBits=0x%X) failed err=%d\n",
                  (unsigned long long)allocInfo.allocationSize,
                  memReqs.memoryTypeBits,
                  allocRes);
        vkDestroyBuffer(vkrt.device, buffer, NULL);
        return VK_NULL_HANDLE;
    }
    
    VkResult bindRes = vkBindBufferMemory(vkrt.device, buffer, vkrt.scratchMemory, 0);
    if (bindRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING,
                  "RTX_AllocateScratchBuffer: vkBindBufferMemory failed err=%d\n",
                  bindRes);
        vkFreeMemory(vkrt.device, vkrt.scratchMemory, NULL);
        vkDestroyBuffer(vkrt.device, buffer, NULL);
        vkrt.scratchMemory = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    vkrt.scratchBuffer = buffer;
    vkrt.scratchSize = allocInfo.allocationSize;
    *memory = vkrt.scratchMemory;

    VkDeviceAddress scratchAddr = RTX_GetBufferDeviceAddressVK(buffer);
    vkrt.lastScratchAddr = scratchAddr;
    vkrt.lastScratchSize = vkrt.scratchSize;

    return buffer;
}

/*
================
RTX_GetBufferDeviceAddress

Get device address of a buffer
================
*/
VkDeviceAddress RTX_GetBufferDeviceAddressVK(VkBuffer buffer) {
    if (buffer == VK_NULL_HANDLE) {
        return 0;
    }

    VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer
    };
    if (!qvkGetBufferDeviceAddress) {
        ri.Printf(PRINT_WARNING, "RTX: vkGetBufferDeviceAddress not available\n");
        return 0;
    }
    VkDeviceAddress addr = qvkGetBufferDeviceAddress(vkrt.device, &addressInfo);

    if (r_rtx_debug && r_rtx_debug->integer >= 1) {
        ri.Printf(PRINT_WARNING,
                  "RTX_GetBufferDeviceAddressVK: buffer=%p addr=0x%llx\n",
                  (void*)(uintptr_t)buffer, (unsigned long long)addr);
    }

    return addr;
}

VkDeviceAddress RTX_GetBufferDeviceAddress(VkBuffer buffer) {
    return RTX_GetBufferDeviceAddressVK(buffer);
}

/*
================
RTX_CreateGBufferImage

Helper: create a single G-buffer image with view and memory.
Returns qfalse on failure; on success the image is transitioned
to VK_IMAGE_LAYOUT_GENERAL via setupCmd (may be VK_NULL_HANDLE
if the caller batches barriers separately).
================
*/
static qboolean RTX_CreateGBufferImage(uint32_t width, uint32_t height,
                                       VkFormat format,
                                       VkImage *outImage,
                                       VkImageView *outView,
                                       VkDeviceMemory *outMemory) {
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    if (vkCreateImage(vkrt.device, &imageInfo, NULL, outImage) != VK_SUCCESS) {
        return qfalse;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vkrt.device, *outImage, &memReqs);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };

    if (vkAllocateMemory(vkrt.device, &allocInfo, NULL, outMemory) != VK_SUCCESS) {
        vkDestroyImage(vkrt.device, *outImage, NULL);
        *outImage = VK_NULL_HANDLE;
        return qfalse;
    }

    vkBindImageMemory(vkrt.device, *outImage, *outMemory, 0);

    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *outImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    if (vkCreateImageView(vkrt.device, &viewInfo, NULL, outView) != VK_SUCCESS) {
        vkFreeMemory(vkrt.device, *outMemory, NULL);
        vkDestroyImage(vkrt.device, *outImage, NULL);
        *outImage = VK_NULL_HANDLE;
        *outMemory = VK_NULL_HANDLE;
        return qfalse;
    }

    return qtrue;
}

/*
================
RTX_DestroyGBufferImage

Helper: destroy a single G-buffer image, view and memory.
================
*/
static void RTX_DestroyGBufferImage(VkImage *image, VkImageView *view,
                                    VkDeviceMemory *memory) {
    if (*view != VK_NULL_HANDLE) {
        vkDestroyImageView(vkrt.device, *view, NULL);
        *view = VK_NULL_HANDLE;
    }
    if (*image != VK_NULL_HANDLE) {
        vkDestroyImage(vkrt.device, *image, NULL);
        *image = VK_NULL_HANDLE;
    }
    if (*memory != VK_NULL_HANDLE) {
        vkFreeMemory(vkrt.device, *memory, NULL);
        *memory = VK_NULL_HANDLE;
    }
}

/*
================
RTX_CreateRTOutputImages

Create output images for ray tracing
================
*/
static qboolean RTX_CreateRTOutputImages(uint32_t width, uint32_t height) {
    VkFormat rtFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
    RTX_ResetFramebufferCopySupport();

    ri.Printf(PRINT_WARNING,
              "RTX: Creating RT output image %ux%u format=%s (swap=%s)\n",
              width, height,
              vk_format_string(rtFormat),
              vk_format_string(vk.color_format));

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = rtFormat,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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
        vkrt.rtImage = VK_NULL_HANDLE;
        return qfalse;
    }

    vkBindImageMemory(vkrt.device, vkrt.rtImage, vkrt.rtImageMemory, 0);
    vkrt.rtImageFormat = rtFormat;

    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = vkrt.rtImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = rtFormat,
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
        vkrt.rtImage = VK_NULL_HANDLE;
        return qfalse;
    }

    // Create G-buffer images for albedo, normals, motion vectors, and depth
    if (!RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.albedoImage, &vkrt.albedoImageView, &vkrt.albedoImageMemory) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.normalImage, &vkrt.normalImageView, &vkrt.normalImageMemory) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.motionImage, &vkrt.motionImageView, &vkrt.motionImageMemory) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.depthImage, &vkrt.depthImageView, &vkrt.depthImageMemory) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.directLightImage, &vkrt.directLightImageView, &vkrt.directLightImageMemory) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.indirectLightImage, &vkrt.indirectLightImageView, &vkrt.indirectLightImageMemory) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.denoisePingImage, &vkrt.denoisePingImageView, &vkrt.denoisePingImageMemory) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.historyIllumImage[0], &vkrt.historyIllumImageView[0], &vkrt.historyIllumImageMemory[0]) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.historyIllumImage[1], &vkrt.historyIllumImageView[1], &vkrt.historyIllumImageMemory[1]) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.historyGeomImage[0], &vkrt.historyGeomImageView[0], &vkrt.historyGeomImageMemory[0]) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.historyGeomImage[1], &vkrt.historyGeomImageView[1], &vkrt.historyGeomImageMemory[1]) ||
        !RTX_CreateGBufferImage(width, height, rtFormat,
            &vkrt.mediaImage, &vkrt.mediaImageView, &vkrt.mediaImageMemory)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create G-buffer images\n");
        RTX_DestroyGBufferImage(&vkrt.albedoImage, &vkrt.albedoImageView, &vkrt.albedoImageMemory);
        RTX_DestroyGBufferImage(&vkrt.normalImage, &vkrt.normalImageView, &vkrt.normalImageMemory);
        RTX_DestroyGBufferImage(&vkrt.motionImage, &vkrt.motionImageView, &vkrt.motionImageMemory);
        RTX_DestroyGBufferImage(&vkrt.depthImage, &vkrt.depthImageView, &vkrt.depthImageMemory);
        RTX_DestroyGBufferImage(&vkrt.directLightImage, &vkrt.directLightImageView, &vkrt.directLightImageMemory);
        RTX_DestroyGBufferImage(&vkrt.indirectLightImage, &vkrt.indirectLightImageView, &vkrt.indirectLightImageMemory);
        RTX_DestroyGBufferImage(&vkrt.denoisePingImage, &vkrt.denoisePingImageView, &vkrt.denoisePingImageMemory);
        RTX_DestroyGBufferImage(&vkrt.mediaImage, &vkrt.mediaImageView, &vkrt.mediaImageMemory);
        for (int h = 0; h < 2; h++) {
            RTX_DestroyGBufferImage(&vkrt.historyIllumImage[h], &vkrt.historyIllumImageView[h], &vkrt.historyIllumImageMemory[h]);
            RTX_DestroyGBufferImage(&vkrt.historyGeomImage[h], &vkrt.historyGeomImageView[h], &vkrt.historyGeomImageMemory[h]);
        }
        vkDestroyImageView(vkrt.device, vkrt.rtImageView, NULL);
        vkFreeMemory(vkrt.device, vkrt.rtImageMemory, NULL);
        vkDestroyImage(vkrt.device, vkrt.rtImage, NULL);
        vkrt.rtImage = VK_NULL_HANDLE;
        vkrt.rtImageView = VK_NULL_HANDLE;
        return qfalse;
    }

    // Transition all output images to GENERAL layout
    VkCommandBuffer setupCmd = vk_begin_one_time_commands();
    if (setupCmd != VK_NULL_HANDLE) {
        VkImageMemoryBarrier barriers[13];
        VkImage images[13] = {
            vkrt.rtImage, vkrt.albedoImage, vkrt.normalImage,
            vkrt.motionImage, vkrt.depthImage,
            vkrt.directLightImage, vkrt.indirectLightImage,
            vkrt.denoisePingImage,
            vkrt.historyIllumImage[0], vkrt.historyIllumImage[1],
            vkrt.historyGeomImage[0], vkrt.historyGeomImage[1],
            vkrt.mediaImage
        };
        for (int i = 0; i < 13; i++) {
            barriers[i] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = images[i],
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
            };
        }

        vkCmdPipelineBarrier(setupCmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                             0, 0, NULL, 0, NULL, 13, barriers);

        vk_end_one_time_commands(setupCmd);
    }

    // Fresh (or resized) history images contain garbage
    vkrt.temporalFramesSinceReset = 0;

    rtOutputInitialized = qfalse;

    return qtrue;
}

void RTX_RecordCommands(VkCommandBuffer cmd) {
    if (!RTX_IsEnabled() || !rtx.available) {
        return;
    }

    if (vkrt.deviceLost) {
        return;
    }

    if (rtx_debug_skip_all && rtx_debug_skip_all->integer > 0) {
        return;
    }

    if (cmd == VK_NULL_HANDLE) {
        return;
    }

    // Trace at the offscreen color target's resolution — vk.renderWidth is
    // transient state mutated by bloom/blur/screenmap passes and must not
    // size the RT output (mismatched sizes here caused per-frame image
    // recreation and partial-screen copies).
    uint32_t width = (uint32_t)glConfig.vidWidth;
    uint32_t height = (uint32_t)glConfig.vidHeight;
    if (vk.fboActive && vk.color_image_width && vk.color_image_height) {
        width = vk.color_image_width;
        height = vk.color_image_height;
    }

    if (width == 0 || height == 0) {
        return;
    }

    if (!vkrt.rtImage || rtOutputWidth != width || rtOutputHeight != height) {
        if (!RTX_CreateRTOutputImages(width, height)) {
            ri.Printf(PRINT_WARNING, "RTX: Failed to create ray tracing output image (%ux%u)\n", width, height);
            return;
        }
        rtOutputWidth = width;
        rtOutputHeight = height;
        rtOutputInitialized = qfalse;
    }

    if (rtx.tlas.needsRebuild) {
        vk_cmd_set_checkpoint(cmd, "RTX:tlas:rebuild");
        RTX_BuildTLAS(&rtx.tlas);
        vk_cmd_set_checkpoint(cmd, "RTX:tlas:rebuilt");
    }

    if (!rtx.tlas.numInstances) {
        return;
    }

    if (!RTX_HasValidViewParms()) {
        if (r_rtx_debug && r_rtx_debug->integer >= 1) {
            ri.Printf(PRINT_WARNING, "RTX: No valid 3D view captured this frame! Rendering with default camera.\n");
        }
    }

    rtxDispatchRays_t params = {
        .width = (int)width,
        .height = (int)height,
        .depth = 1,
        .shaderTable = NULL,
        .maxRecursion = r_rtx_gi_bounces ? r_rtx_gi_bounces->integer : 1
    };

    if (params.maxRecursion < 1) {
        params.maxRecursion = 1;
    }

    qboolean hadOutput = rtOutputInitialized;
    vk_cmd_set_checkpoint(cmd, "RTX:dispatch:begin");
    RTX_DispatchRaysVK(cmd, &params);
    vk_cmd_set_checkpoint(cmd, "RTX:dispatch:end");

    if (!rtOutputInitialized) {
        if (hadOutput || (r_rtx_debug && r_rtx_debug->integer >= 1)) {
            ri.Printf(PRINT_WARNING, "RTX: Ray dispatch did not produce output this frame\n");
        }
        return;
    }

    VkImage targetImage = vk.color_image;
    VkImageLayout targetOriginalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkPipelineStageFlags targetSrcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    qboolean usingSwapchain = qfalse;
    VkFormat targetFormat = vk.color_format;

    if (targetImage == VK_NULL_HANDLE && vk.cmd) {
        uint32_t imageIndex = vk.cmd->swapchain_image_index;
        if (vk.swapchain_image_count > 0 && imageIndex < vk.swapchain_image_count) {
            targetImage = vk.swapchain_images[imageIndex];
            targetOriginalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            targetSrcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            usingSwapchain = qtrue;
            targetFormat = vk.present_format.format;
        }
    }

    if (!vkrt.rtImage || targetImage == VK_NULL_HANDLE) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Skipping framebuffer copy (rtImage=%p, targetImage=%p)\n",
                  (void*)vkrt.rtImage, (void*)targetImage);
        return;
    }

    // Clamp blit dimensions to target image size to avoid out-of-bounds writes
    uint32_t dstWidth = width;
    uint32_t dstHeight = height;
    if (!usingSwapchain && vk.color_image_width && vk.color_image_height) {
        if (dstWidth > vk.color_image_width) dstWidth = vk.color_image_width;
        if (dstHeight > vk.color_image_height) dstHeight = vk.color_image_height;
        if (dstWidth != width || dstHeight != height) {
            ri.Printf(PRINT_WARNING,
                      "RTX: Clamped blit dimensions from %ux%u to %ux%u (color_image %ux%u)\n",
                      width, height, dstWidth, dstHeight,
                      vk.color_image_width, vk.color_image_height);
        }
    }

    const qboolean skipPresent = (rtx_debug_skip_present && rtx_debug_skip_present->integer > 0);

    if (!RTX_FramebufferCopySupported(
            vkrt.rtImageFormat,
            targetFormat,
            usingSwapchain ? "swapchain framebuffer copy" : "framebuffer copy",
            qtrue)) {
        return;
    }

    if (skipPresent) {
        if (r_rtx_debug && r_rtx_debug->integer >= 1) {
            ri.Printf(PRINT_WARNING,
                      "RTX: Present copy skipped (rtx_debug_skip_present=1)\n");
        }
        vk_cmd_set_checkpoint(cmd, "RTX:copy:skip");
        return;
    }

    vk_cmd_set_checkpoint(cmd, "RTX:copy:prepare");

    // Barrier for RT source image: the dispatch left it in
    // TRANSFER_SRC_OPTIMAL (recorded earlier in this command buffer, or in
    // the already-fenced immediate buffer on the validation path).
    VkImageMemoryBarrier rtSrcBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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

    VkAccessFlags srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (usingSwapchain) {
        srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        targetSrcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: copy barrier stageMask=0x%X accessMask=0x%X (usingSwapchain=%d)\n",
                  targetSrcStage, srcAccessMask, usingSwapchain ? 1 : 0);
    }

    VkImageLayout currentLayout = vk_image_get_layout_or(targetImage, targetOriginalLayout);

    VkImageMemoryBarrier colorBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccessMask,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = currentLayout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = targetImage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    rtx_copy_debug.stageMask = targetSrcStage;
    rtx_copy_debug.accessMask = srcAccessMask;
    rtx_copy_debug.oldLayout = currentLayout;
    rtx_copy_debug.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    rtx_copy_debug.targetImage = targetImage;
    rtx_copy_debug.usingSwapchain = usingSwapchain;

    VkImageMemoryBarrier preCopyBarriers[2] = { rtSrcBarrier, colorBarrier };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 2, preCopyBarriers);

    vk_image_set_layout(targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_cmd_set_checkpoint(cmd, "RTX:copy:dst-ready");

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: copy barrier target=%p oldLayout=%d -> %d (usingSwapchain=%d)\n",
                  (void*)targetImage, currentLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, usingSwapchain ? 1 : 0);
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Framebuffer copy path uses %s (srcFormat=%d dstFormat=%d)\n",
                  rtx_framebuffer_copy.requiresBlit ? "blit" : "copy",
                  (int)vkrt.rtImageFormat,
                  (int)targetFormat);
    }

    if (!rtx_framebuffer_copy.requiresBlit) {
        uint32_t copyW = (width < dstWidth) ? width : dstWidth;
        uint32_t copyH = (height < dstHeight) ? height : dstHeight;
        VkImageCopy copyRegion = {
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .extent = { copyW, copyH, 1 }
        };

        vkCmdCopyImage(cmd,
                      vkrt.rtImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &copyRegion);
    } else {
        VkImageBlit blitRegion = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = {
                { 0, 0, 0 },
                { (int32_t)width, (int32_t)height, 1 }
            },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = {
                { 0, 0, 0 },
                { (int32_t)dstWidth, (int32_t)dstHeight, 1 }
            }
        };

        vkCmdBlitImage(cmd,
                       vkrt.rtImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blitRegion,
                       VK_FILTER_NEAREST);
    }

    vk_cmd_set_checkpoint(cmd, "RTX:copy:issued");

    colorBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    colorBarrier.dstAccessMask = usingSwapchain
        ? VK_ACCESS_MEMORY_READ_BIT
        : VK_ACCESS_SHADER_READ_BIT;
    colorBarrier.oldLayout = vk_image_get_layout_or(targetImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    colorBarrier.newLayout = targetOriginalLayout;

    VkPipelineStageFlags targetDstStage = usingSwapchain
        ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
        : (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        targetDstStage,
        0, 0, NULL, 0, NULL, 1, &colorBarrier);

    vk_image_set_layout(targetImage, targetOriginalLayout);
    vk_cmd_set_checkpoint(cmd, "RTX:copy:restored");

    if (usingSwapchain && r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RTX: Framebuffer copy targeted swapchain image %u\n",
                  vk.cmd ? vk.cmd->swapchain_image_index : 0u);
    }
}

// Denoiser and DLSS implementations are in separate files:
// - rt_rtx_denoiser.c
// - rt_rtx_dlss.c

VkImage RTX_GetRTImage(void) {
    return vkrt.rtImage;
}

VkImageView RTX_GetRTImageView(void) {
    return vkrt.rtImageView;
}

VkFormat RTX_GetRTImageFormat(void) {
    return vkrt.rtImageFormat;
}

VkBuffer RTX_GetDebugSettingsBuffer(void) {
    return VK_NULL_HANDLE;
}

void RTX_GetLightingContributionViews(VkImageView *directView, VkImageView *indirectView) {
    if (directView) {
        *directView = vkrt.directLightImageView;
    }
    if (indirectView) {
        *indirectView = vkrt.indirectLightImageView;
    }
}

VkImageView RTX_GetMediaImageView(void) {
    return vkrt.mediaImageView;
}

void RTX_CompositeHybridAdd(VkCommandBuffer cmd, uint32_t width, uint32_t height, float intensity) {
    if (!vkrt.rtImage || cmd == VK_NULL_HANDLE) {
        return;
    }

    if (intensity <= 0.0f) {
        return;
    }

    if (!rtOutputInitialized) {
        return;
    }

    VkImage dstImage = vk.color_image;
    if (dstImage == VK_NULL_HANDLE) {
        return;
    }

    if (!RTX_FramebufferCopySupported(
            vkrt.rtImageFormat,
            vk.color_format,
            "hybrid composite copy",
            qtrue)) {
        return;
    }

    VkImageMemoryBarrier barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vkrt.rtImage,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = vk_image_get_layout_or( dstImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ),
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dstImage,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        }
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 2, barriers);

    vk_image_set_layout( dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

    if ( vk.color_format == VK_FORMAT_R32G32B32A32_SFLOAT ) {
        VkImageCopy copyRegion = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { width, height, 1 }
        };

        vkCmdCopyImage(cmd,
                      vkrt.rtImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1, &copyRegion);
    } else {
        VkImageBlit blitRegion = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = {
                { 0, 0, 0 },
                { (int32_t)width, (int32_t)height, 1 }
            },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = {
                { 0, 0, 0 },
                { (int32_t)width, (int32_t)height, 1 }
            }
        };

        vkCmdBlitImage(cmd,
                       vkrt.rtImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blitRegion,
                       VK_FILTER_NEAREST);
    }

    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout = vk_image_get_layout_or( dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barriers[1]);

    vk_image_set_layout( dstImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
}

static qboolean RTX_EnsureDebugOverlayPipeline(void) {
    if (vkrt.debugOverlayPipeline) {
        return qtrue;
    }

    if (!vkrt.device) {
        return qfalse;
    }

    VkDescriptorSetLayoutBinding bindings[8] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 6, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_LEN(bindings),
        .pBindings = bindings
    };

    if (vkCreateDescriptorSetLayout(vkrt.device, &layoutInfo, NULL, &vkrt.debugOverlaySetLayout) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create debug overlay descriptor set layout\n");
        return qfalse;
    }

    VkPushConstantRange pcRange = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(uint32_t) * 4
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vkrt.debugOverlaySetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcRange
    };

    if (vkCreatePipelineLayout(vkrt.device, &pipelineLayoutInfo, NULL, &vkrt.debugOverlayPipelineLayout) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create debug overlay pipeline layout\n");
        RTX_DestroyDebugOverlayPipeline();
        return qfalse;
    }

    uint32_t codeSize = 0;
    uint32_t *shaderCode = R_LoadSPIRV("shaders/compute/rtx_debug_overlay.spv", &codeSize);
    if (!shaderCode) {
        ri.Printf(PRINT_WARNING, "RTX: Missing rtx_debug_overlay.spv - run compile_rtx_debug_shader.bat\n");
        return qfalse;
    }

    VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = codeSize,
        .pCode = shaderCode
    };

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(vkrt.device, &moduleInfo, NULL, &shaderModule) != VK_SUCCESS) {
        Z_Free(shaderCode);
        ri.Printf(PRINT_WARNING, "RTX: Failed to create debug overlay shader module\n");
        return qfalse;
    }

    VkComputePipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = vkrt.debugOverlayPipelineLayout,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shaderModule,
            .pName = "main"
        }
    };

    if (vkCreateComputePipelines(vkrt.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkrt.debugOverlayPipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(vkrt.device, shaderModule, NULL);
        Z_Free(shaderCode);
        ri.Printf(PRINT_WARNING, "RTX: Failed to create debug overlay compute pipeline\n");
        RTX_DestroyDebugOverlayPipeline();
        return qfalse;
    }

    vkDestroyShaderModule(vkrt.device, shaderModule, NULL);
    Z_Free(shaderCode);

    VkDescriptorPoolSize poolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }
    };

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = ARRAY_LEN(poolSizes),
        .pPoolSizes = poolSizes
    };

    if (vkCreateDescriptorPool(vkrt.device, &poolInfo, NULL, &vkrt.debugOverlayDescriptorPool) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create debug overlay descriptor pool\n");
        RTX_DestroyDebugOverlayPipeline();
        return qfalse;
    }

    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vkrt.debugOverlayDescriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vkrt.debugOverlaySetLayout
    };

    if (vkAllocateDescriptorSets(vkrt.device, &allocInfo, &vkrt.debugOverlayDescriptorSet) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to allocate debug overlay descriptor set\n");
        RTX_DestroyDebugOverlayPipeline();
        return qfalse;
    }

    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .minLod = 0.0f,
        .maxLod = 0.0f
    };

    if (vkCreateSampler(vkrt.device, &samplerInfo, NULL, &vkrt.debugOverlaySampler) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create debug overlay sampler\n");
        RTX_DestroyDebugOverlayPipeline();
        return qfalse;
    }

    return qtrue;
}

/*
================
Depth-aware composite pipeline

Merges rasterized dynamic content (entities, view weapon, pickups) into the
path-traced world image using depth comparison: raster pixels that are
clearly in front of the traced world surface survive; everything else shows
the traced result. Runs in place on the traced color image before the blit
into the offscreen color target.
================
*/
static qboolean RTX_EnsureCompositePipeline(void) {
    if (vkrt.compositePipeline) {
        return qtrue;
    }

    if (!vkrt.device) {
        return qfalse;
    }

    VkDescriptorSetLayoutBinding bindings[5] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_LEN(bindings),
        .pBindings = bindings
    };

    if (vkCreateDescriptorSetLayout(vkrt.device, &layoutInfo, NULL, &vkrt.compositeSetLayout) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create composite descriptor set layout\n");
        return qfalse;
    }

    VkPushConstantRange pcRange = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(float) * 6
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vkrt.compositeSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcRange
    };

    if (vkCreatePipelineLayout(vkrt.device, &pipelineLayoutInfo, NULL, &vkrt.compositePipelineLayout) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create composite pipeline layout\n");
        RTX_DestroyCompositePipeline();
        return qfalse;
    }

    uint32_t codeSize = 0;
    uint32_t *shaderCode = R_LoadSPIRV("shaders/compute/rt_composite.spv", &codeSize);
    if (!shaderCode) {
        ri.Printf(PRINT_WARNING, "RTX: Missing rt_composite.spv\n");
        RTX_DestroyCompositePipeline();
        return qfalse;
    }

    VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = codeSize,
        .pCode = shaderCode
    };

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(vkrt.device, &moduleInfo, NULL, &shaderModule) != VK_SUCCESS) {
        Z_Free(shaderCode);
        ri.Printf(PRINT_WARNING, "RTX: Failed to create composite shader module\n");
        RTX_DestroyCompositePipeline();
        return qfalse;
    }

    VkComputePipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = vkrt.compositePipelineLayout,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shaderModule,
            .pName = "main"
        }
    };

    VkResult pipelineRes = vkCreateComputePipelines(vkrt.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkrt.compositePipeline);
    vkDestroyShaderModule(vkrt.device, shaderModule, NULL);
    Z_Free(shaderCode);
    if (pipelineRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create composite compute pipeline\n");
        RTX_DestroyCompositePipeline();
        return qfalse;
    }

    VkDescriptorPoolSize poolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }
    };

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = ARRAY_LEN(poolSizes),
        .pPoolSizes = poolSizes
    };

    if (vkCreateDescriptorPool(vkrt.device, &poolInfo, NULL, &vkrt.compositeDescriptorPool) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create composite descriptor pool\n");
        RTX_DestroyCompositePipeline();
        return qfalse;
    }

    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vkrt.compositeDescriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vkrt.compositeSetLayout
    };

    if (vkAllocateDescriptorSets(vkrt.device, &allocInfo, &vkrt.compositeDescriptorSet) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to allocate composite descriptor set\n");
        RTX_DestroyCompositePipeline();
        return qfalse;
    }

    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .minLod = 0.0f,
        .maxLod = 0.0f
    };

    if (vkCreateSampler(vkrt.device, &samplerInfo, NULL, &vkrt.compositeSampler) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create composite sampler\n");
        RTX_DestroyCompositePipeline();
        return qfalse;
    }

    return qtrue;
}

static void RTX_DestroyCompositePipeline(void) {
    if (vkrt.compositeSampler) {
        vkDestroySampler(vkrt.device, vkrt.compositeSampler, NULL);
        vkrt.compositeSampler = VK_NULL_HANDLE;
    }
    if (vkrt.compositeDescriptorPool) {
        vkDestroyDescriptorPool(vkrt.device, vkrt.compositeDescriptorPool, NULL);
        vkrt.compositeDescriptorPool = VK_NULL_HANDLE;
        vkrt.compositeDescriptorSet = VK_NULL_HANDLE;
    }
    if (vkrt.compositeSetLayout) {
        vkDestroyDescriptorSetLayout(vkrt.device, vkrt.compositeSetLayout, NULL);
        vkrt.compositeSetLayout = VK_NULL_HANDLE;
    }
    if (vkrt.compositePipeline) {
        vkDestroyPipeline(vkrt.device, vkrt.compositePipeline, NULL);
        vkrt.compositePipeline = VK_NULL_HANDLE;
    }
    if (vkrt.compositePipelineLayout) {
        vkDestroyPipelineLayout(vkrt.device, vkrt.compositePipelineLayout, NULL);
        vkrt.compositePipelineLayout = VK_NULL_HANDLE;
    }
    vkrt.compositeBoundTracedColor = VK_NULL_HANDLE;
    vkrt.compositeBoundTracedDepth = VK_NULL_HANDLE;
    vkrt.compositeBoundRasterColor = VK_NULL_HANDLE;
    vkrt.compositeBoundRasterDepth = VK_NULL_HANDLE;
    vkrt.compositeBoundMedia = VK_NULL_HANDLE;
}

static void RTX_RecordDepthAwareComposite(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!vk.fboActive || vk.color_image_view == VK_NULL_HANDLE ||
        vk.depth_image_view_depth_only == VK_NULL_HANDLE ||
        vkrt.rtImageView == VK_NULL_HANDLE || vkrt.depthImageView == VK_NULL_HANDLE) {
        return;
    }

    if (!RTX_EnsureCompositePipeline()) {
        return;
    }

    // Rebind descriptors only when the underlying views changed (resize)
    if (vkrt.compositeBoundTracedColor != vkrt.rtImageView ||
        vkrt.compositeBoundTracedDepth != vkrt.depthImageView ||
        vkrt.compositeBoundRasterColor != vk.color_image_view ||
        vkrt.compositeBoundRasterDepth != vk.depth_image_view_depth_only ||
        vkrt.compositeBoundMedia != vkrt.mediaImageView) {

        vkQueueWaitIdle(vk.queue);

        VkDescriptorImageInfo tracedColorInfo = {
            .imageView = vkrt.rtImageView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL
        };
        VkDescriptorImageInfo tracedDepthInfo = {
            .imageView = vkrt.depthImageView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL
        };
        VkDescriptorImageInfo rasterColorInfo = {
            .sampler = vkrt.compositeSampler,
            .imageView = vk.color_image_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        VkDescriptorImageInfo rasterDepthInfo = {
            .sampler = vkrt.compositeSampler,
            .imageView = vk.depth_image_view_depth_only,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        // Media falls back to the traced color view only if creation failed;
        // the shader then reads transmittance from a color alpha of 1.0
        VkDescriptorImageInfo mediaInfo = {
            .imageView = (vkrt.mediaImageView != VK_NULL_HANDLE) ? vkrt.mediaImageView : vkrt.rtImageView,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL
        };

        VkWriteDescriptorSet writes[5] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = vkrt.compositeDescriptorSet,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &tracedColorInfo
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = vkrt.compositeDescriptorSet,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &tracedDepthInfo
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = vkrt.compositeDescriptorSet,
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &rasterColorInfo
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = vkrt.compositeDescriptorSet,
                .dstBinding = 3,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &rasterDepthInfo
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = vkrt.compositeDescriptorSet,
                .dstBinding = 4,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &mediaInfo
            }
        };

        vkUpdateDescriptorSets(vkrt.device, ARRAY_LEN(writes), writes, 0, NULL);

        vkrt.compositeBoundTracedColor = vkrt.rtImageView;
        vkrt.compositeBoundTracedDepth = vkrt.depthImageView;
        vkrt.compositeBoundRasterColor = vk.color_image_view;
        vkrt.compositeBoundRasterDepth = vk.depth_image_view_depth_only;
        vkrt.compositeBoundMedia = vkrt.mediaImageView;
    }

    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (glConfig.stencilBits > 0) {
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    // Raster depth: attachment -> sampled; raster color writes -> compute reads
    VkImageMemoryBarrier preBarriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vk.depth_image,
            .subresourceRange = { depthAspect, 0, 1, 0, 1 }
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vk.color_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        }
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, ARRAY_LEN(preBarriers), preBarriers);

    float zNear = 4.0f, zFar = 4096.0f;
    RTX_GetLastCameraPlanes(&zNear, &zFar);

    // Tolerances are deliberately loose: the traced depth carries per-sample
    // AA jitter, so near-equal surfaces (the world itself) must resolve to
    // the traced result; only clearly-foreground raster content survives.
    float pushData[6] = {
        zNear,
        zFar,
        0.94f,  // relative depth tolerance
        6.0f,   // absolute tolerance in world units
        (rt_exposure && rt_exposure->value > 0.0f) ? rt_exposure->value : 1.0f,
        0.0f
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkrt.compositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            vkrt.compositePipelineLayout, 0, 1, &vkrt.compositeDescriptorSet, 0, NULL);
    vkCmdPushConstants(cmd, vkrt.compositePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pushData), pushData);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    // Raster depth back to attachment layout for the resumed render pass
    VkImageMemoryBarrier postBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = vk.depth_image,
        .subresourceRange = { depthAspect, 0, 1, 0, 1 }
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, NULL, 0, NULL, 1, &postBarrier);
}

/*
================
À-trous despeckle pipeline

Edge-aware wavelet filter over the linear HDR traced image (see
rt_denoise.comp). The final pass applies tonemap + gamma, so this chain
must run before the depth-aware composite, which blends display-space
raster pixels.
================
*/

// Must match rt_denoise.comp
#define RTX_DENOISE_FLAG_PING_TO_COLOR  1u
#define RTX_DENOISE_FLAG_DEMODULATE     2u
#define RTX_DENOISE_FLAG_FINALIZE       4u
#define RTX_DENOISE_FLAG_PASSTHROUGH    8u
#define RTX_DENOISE_FLAG_REMODULATE     16u

// Must match rt_temporal.comp
typedef struct {
    float prevViewProj[16];
    float zFar;
    float alphaMin;
    uint32_t parity;
    uint32_t resetHistory;
} rtxTemporalPush_t;

typedef struct {
    uint32_t stepSize;
    uint32_t flags;
    float sigmaLum;
    float sigmaDepth;
    float exposure;
} rtxDenoisePush_t;

static qboolean RTX_EnsureDenoisePipeline(void) {
    if (vkrt.denoisePipeline) {
        return qtrue;
    }

    if (!vkrt.device) {
        return qfalse;
    }

    VkDescriptorSetLayoutBinding bindings[5] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_LEN(bindings),
        .pBindings = bindings
    };

    if (vkCreateDescriptorSetLayout(vkrt.device, &layoutInfo, NULL, &vkrt.denoiseSetLayout) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create denoise descriptor set layout\n");
        return qfalse;
    }

    VkPushConstantRange pcRange = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(rtxDenoisePush_t)
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vkrt.denoiseSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcRange
    };

    if (vkCreatePipelineLayout(vkrt.device, &pipelineLayoutInfo, NULL, &vkrt.denoisePipelineLayout) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create denoise pipeline layout\n");
        RTX_DestroyDenoisePipeline();
        return qfalse;
    }

    uint32_t codeSize = 0;
    uint32_t *shaderCode = R_LoadSPIRV("shaders/compute/rt_denoise.spv", &codeSize);
    if (!shaderCode) {
        ri.Printf(PRINT_WARNING, "RTX: Missing rt_denoise.spv\n");
        RTX_DestroyDenoisePipeline();
        return qfalse;
    }

    VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = codeSize,
        .pCode = shaderCode
    };

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(vkrt.device, &moduleInfo, NULL, &shaderModule) != VK_SUCCESS) {
        Z_Free(shaderCode);
        ri.Printf(PRINT_WARNING, "RTX: Failed to create denoise shader module\n");
        RTX_DestroyDenoisePipeline();
        return qfalse;
    }

    VkComputePipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = vkrt.denoisePipelineLayout,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shaderModule,
            .pName = "main"
        }
    };

    VkResult pipelineRes = vkCreateComputePipelines(vkrt.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkrt.denoisePipeline);
    vkDestroyShaderModule(vkrt.device, shaderModule, NULL);
    Z_Free(shaderCode);
    if (pipelineRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create denoise compute pipeline\n");
        RTX_DestroyDenoisePipeline();
        return qfalse;
    }

    VkDescriptorPoolSize poolSize = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5
    };

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize
    };

    if (vkCreateDescriptorPool(vkrt.device, &poolInfo, NULL, &vkrt.denoiseDescriptorPool) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create denoise descriptor pool\n");
        RTX_DestroyDenoisePipeline();
        return qfalse;
    }

    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vkrt.denoiseDescriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vkrt.denoiseSetLayout
    };

    if (vkAllocateDescriptorSets(vkrt.device, &allocInfo, &vkrt.denoiseDescriptorSet) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to allocate denoise descriptor set\n");
        RTX_DestroyDenoisePipeline();
        return qfalse;
    }

    return qtrue;
}

static void RTX_DestroyDenoisePipeline(void) {
    if (vkrt.denoiseDescriptorPool) {
        vkDestroyDescriptorPool(vkrt.device, vkrt.denoiseDescriptorPool, NULL);
        vkrt.denoiseDescriptorPool = VK_NULL_HANDLE;
        vkrt.denoiseDescriptorSet = VK_NULL_HANDLE;
    }
    if (vkrt.denoiseSetLayout) {
        vkDestroyDescriptorSetLayout(vkrt.device, vkrt.denoiseSetLayout, NULL);
        vkrt.denoiseSetLayout = VK_NULL_HANDLE;
    }
    if (vkrt.denoisePipeline) {
        vkDestroyPipeline(vkrt.device, vkrt.denoisePipeline, NULL);
        vkrt.denoisePipeline = VK_NULL_HANDLE;
    }
    if (vkrt.denoisePipelineLayout) {
        vkDestroyPipelineLayout(vkrt.device, vkrt.denoisePipelineLayout, NULL);
        vkrt.denoisePipelineLayout = VK_NULL_HANDLE;
    }
    vkrt.denoiseBoundColor = VK_NULL_HANDLE;
    vkrt.denoiseBoundPing = VK_NULL_HANDLE;
}

/*
================
RTX_RecordDenoise

Record the despeckle chain over the traced image. With filter=qtrue this is
four dilated à-trous passes ending in remodulation + tonemap; with
filter=qfalse a single pass keeps the image display-ready when filtering is
disabled. inputIsIllum says whether the temporal pass already demodulated
the color image into illumination. All images stay in GENERAL layout;
compute→compute barriers order the ping-pong.
================
*/
static void RTX_RecordDenoise(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                              qboolean filter, qboolean inputIsIllum) {
    if (vkrt.rtImageView == VK_NULL_HANDLE || vkrt.denoisePingImageView == VK_NULL_HANDLE ||
        vkrt.albedoImageView == VK_NULL_HANDLE || vkrt.normalImageView == VK_NULL_HANDLE ||
        vkrt.depthImageView == VK_NULL_HANDLE) {
        return;
    }

    if (!RTX_EnsureDenoisePipeline()) {
        return;
    }

    // Rebind descriptors only when the underlying views changed (resize)
    if (vkrt.denoiseBoundColor != vkrt.rtImageView ||
        vkrt.denoiseBoundPing != vkrt.denoisePingImageView) {

        vkQueueWaitIdle(vk.queue);

        VkDescriptorImageInfo imageInfos[5] = {
            { .imageView = vkrt.rtImageView,           .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.denoisePingImageView,  .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.albedoImageView,       .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.normalImageView,       .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.depthImageView,        .imageLayout = VK_IMAGE_LAYOUT_GENERAL }
        };

        VkWriteDescriptorSet writes[5];
        for (uint32_t i = 0; i < ARRAY_LEN(writes); i++) {
            writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = vkrt.denoiseDescriptorSet,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &imageInfos[i]
            };
        }

        vkUpdateDescriptorSets(vkrt.device, ARRAY_LEN(writes), writes, 0, NULL);

        vkrt.denoiseBoundColor = vkrt.rtImageView;
        vkrt.denoiseBoundPing = vkrt.denoisePingImageView;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkrt.denoisePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            vkrt.denoisePipelineLayout, 0, 1, &vkrt.denoiseDescriptorSet, 0, NULL);

    const uint32_t groupsX = (width + 7) / 8;
    const uint32_t groupsY = (height + 7) / 8;

    const VkMemoryBarrier computeBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    };

    rtxDenoisePush_t push;
    push.sigmaLum = 0.6f;
    push.sigmaDepth = 0.004f;
    push.exposure = (rt_exposure && rt_exposure->value > 0.0f) ? rt_exposure->value : 1.0f;

    // The temporal pass leaves demodulated illumination in the ping image;
    // otherwise raw radiance sits in the color image.
    if (!filter) {
        push.stepSize = 1;
        if (inputIsIllum) {
            // Single light spatial pass ping -> color with remodulation
            push.flags = RTX_DENOISE_FLAG_PING_TO_COLOR | RTX_DENOISE_FLAG_REMODULATE |
                         RTX_DENOISE_FLAG_FINALIZE;
        } else {
            push.flags = RTX_DENOISE_FLAG_PASSTHROUGH | RTX_DENOISE_FLAG_FINALIZE;
        }
        vkCmdPushConstants(cmd, vkrt.denoisePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);
        return;
    }

    uint32_t passFlags[4];
    uint32_t passSteps[4];
    int passCount;

    if (inputIsIllum) {
        // ping -> color -> ping -> color
        passFlags[0] = RTX_DENOISE_FLAG_PING_TO_COLOR;
        passFlags[1] = 0u;
        passFlags[2] = RTX_DENOISE_FLAG_PING_TO_COLOR | RTX_DENOISE_FLAG_REMODULATE |
                       RTX_DENOISE_FLAG_FINALIZE;
        passSteps[0] = 1;
        passSteps[1] = 2;
        passSteps[2] = 4;
        passCount = 3;
    } else {
        // color -> ping (demodulate) -> color -> ping -> color
        passFlags[0] = RTX_DENOISE_FLAG_DEMODULATE;
        passFlags[1] = RTX_DENOISE_FLAG_PING_TO_COLOR;
        passFlags[2] = 0u;
        passFlags[3] = RTX_DENOISE_FLAG_PING_TO_COLOR | RTX_DENOISE_FLAG_REMODULATE |
                       RTX_DENOISE_FLAG_FINALIZE;
        passSteps[0] = 1;
        passSteps[1] = 2;
        passSteps[2] = 4;
        passSteps[3] = 8;
        passCount = 4;
    }

    for (int pass = 0; pass < passCount; pass++) {
        if (pass > 0) {
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &computeBarrier, 0, NULL, 0, NULL);
        }

        push.stepSize = passSteps[pass];
        push.flags = passFlags[pass];
        vkCmdPushConstants(cmd, vkrt.denoisePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);
    }
}

/*
================
Temporal accumulation pipeline (see rt_temporal.comp)

Reprojects each pixel's first-hit world position into the previous frame
and blends demodulated illumination with the validated history. Leaves
illumination (not radiance) in the traced color image, so the à-trous
chain must run with inputIsIllum afterwards.
================
*/
static qboolean RTX_EnsureTemporalPipeline(void) {
    if (vkrt.temporalPipeline) {
        return qtrue;
    }

    if (!vkrt.device) {
        return qfalse;
    }

    VkDescriptorSetLayoutBinding bindings[10];
    for (uint32_t i = 0; i < ARRAY_LEN(bindings); i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        };
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_LEN(bindings),
        .pBindings = bindings
    };

    if (vkCreateDescriptorSetLayout(vkrt.device, &layoutInfo, NULL, &vkrt.temporalSetLayout) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create temporal descriptor set layout\n");
        return qfalse;
    }

    VkPushConstantRange pcRange = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(rtxTemporalPush_t)
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &vkrt.temporalSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcRange
    };

    if (vkCreatePipelineLayout(vkrt.device, &pipelineLayoutInfo, NULL, &vkrt.temporalPipelineLayout) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create temporal pipeline layout\n");
        RTX_DestroyTemporalPipeline();
        return qfalse;
    }

    uint32_t codeSize = 0;
    uint32_t *shaderCode = R_LoadSPIRV("shaders/compute/rt_temporal.spv", &codeSize);
    if (!shaderCode) {
        ri.Printf(PRINT_WARNING, "RTX: Missing rt_temporal.spv\n");
        RTX_DestroyTemporalPipeline();
        return qfalse;
    }

    VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = codeSize,
        .pCode = shaderCode
    };

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(vkrt.device, &moduleInfo, NULL, &shaderModule) != VK_SUCCESS) {
        Z_Free(shaderCode);
        ri.Printf(PRINT_WARNING, "RTX: Failed to create temporal shader module\n");
        RTX_DestroyTemporalPipeline();
        return qfalse;
    }

    VkComputePipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = vkrt.temporalPipelineLayout,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shaderModule,
            .pName = "main"
        }
    };

    VkResult pipelineRes = vkCreateComputePipelines(vkrt.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vkrt.temporalPipeline);
    vkDestroyShaderModule(vkrt.device, shaderModule, NULL);
    Z_Free(shaderCode);
    if (pipelineRes != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create temporal compute pipeline\n");
        RTX_DestroyTemporalPipeline();
        return qfalse;
    }

    VkDescriptorPoolSize poolSize = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10
    };

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize
    };

    if (vkCreateDescriptorPool(vkrt.device, &poolInfo, NULL, &vkrt.temporalDescriptorPool) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create temporal descriptor pool\n");
        RTX_DestroyTemporalPipeline();
        return qfalse;
    }

    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vkrt.temporalDescriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &vkrt.temporalSetLayout
    };

    if (vkAllocateDescriptorSets(vkrt.device, &allocInfo, &vkrt.temporalDescriptorSet) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to allocate temporal descriptor set\n");
        RTX_DestroyTemporalPipeline();
        return qfalse;
    }

    return qtrue;
}

static void RTX_DestroyTemporalPipeline(void) {
    if (vkrt.temporalDescriptorPool) {
        vkDestroyDescriptorPool(vkrt.device, vkrt.temporalDescriptorPool, NULL);
        vkrt.temporalDescriptorPool = VK_NULL_HANDLE;
        vkrt.temporalDescriptorSet = VK_NULL_HANDLE;
    }
    if (vkrt.temporalSetLayout) {
        vkDestroyDescriptorSetLayout(vkrt.device, vkrt.temporalSetLayout, NULL);
        vkrt.temporalSetLayout = VK_NULL_HANDLE;
    }
    if (vkrt.temporalPipeline) {
        vkDestroyPipeline(vkrt.device, vkrt.temporalPipeline, NULL);
        vkrt.temporalPipeline = VK_NULL_HANDLE;
    }
    if (vkrt.temporalPipelineLayout) {
        vkDestroyPipelineLayout(vkrt.device, vkrt.temporalPipelineLayout, NULL);
        vkrt.temporalPipelineLayout = VK_NULL_HANDLE;
    }
    vkrt.temporalBoundColor = VK_NULL_HANDLE;
}

/*
================
RTX_RecordTemporal

Returns qtrue when the pass was recorded (the traced color image then holds
demodulated illumination instead of radiance).
================
*/
static qboolean RTX_RecordTemporal(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (vkrt.rtImageView == VK_NULL_HANDLE || vkrt.albedoImageView == VK_NULL_HANDLE ||
        vkrt.normalImageView == VK_NULL_HANDLE || vkrt.depthImageView == VK_NULL_HANDLE ||
        vkrt.motionImageView == VK_NULL_HANDLE ||
        vkrt.denoisePingImageView == VK_NULL_HANDLE ||
        vkrt.historyIllumImageView[0] == VK_NULL_HANDLE ||
        vkrt.historyIllumImageView[1] == VK_NULL_HANDLE ||
        vkrt.historyGeomImageView[0] == VK_NULL_HANDLE ||
        vkrt.historyGeomImageView[1] == VK_NULL_HANDLE) {
        return qfalse;
    }

    if (!RTX_EnsureTemporalPipeline()) {
        return qfalse;
    }

    rtxTemporalPush_t push;
    qboolean havePrev = RTX_GetPrevViewProjection(push.prevViewProj);

    // Rebind descriptors only when the underlying views changed (resize)
    if (vkrt.temporalBoundColor != vkrt.rtImageView) {
        vkQueueWaitIdle(vk.queue);

        VkDescriptorImageInfo imageInfos[10] = {
            { .imageView = vkrt.rtImageView,              .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.albedoImageView,          .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.normalImageView,          .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.depthImageView,           .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.motionImageView,          .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.historyIllumImageView[0], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.historyGeomImageView[0],  .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.historyIllumImageView[1], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.historyGeomImageView[1],  .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
            { .imageView = vkrt.denoisePingImageView,     .imageLayout = VK_IMAGE_LAYOUT_GENERAL }
        };

        VkWriteDescriptorSet writes[10];
        for (uint32_t i = 0; i < ARRAY_LEN(writes); i++) {
            writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = vkrt.temporalDescriptorSet,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &imageInfos[i]
            };
        }

        vkUpdateDescriptorSets(vkrt.device, ARRAY_LEN(writes), writes, 0, NULL);
        vkrt.temporalBoundColor = vkrt.rtImageView;
    }

    float zFar = 4096.0f;
    RTX_GetLastCameraPlanes(NULL, &zFar);

    if (!havePrev) {
        Com_Memset(push.prevViewProj, 0, sizeof(push.prevViewProj));
    }
    push.zFar = zFar;
    push.alphaMin = 0.12f;
    push.parity = vkrt.temporalParity;
    // History needs both parities written once before it is trustworthy
    push.resetHistory = (!havePrev || vkrt.temporalFramesSinceReset < 2) ? 1u : 0u;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkrt.temporalPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            vkrt.temporalPipelineLayout, 0, 1, &vkrt.temporalDescriptorSet, 0, NULL);
    vkCmdPushConstants(cmd, vkrt.temporalPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    vkrt.temporalParity ^= 1u;
    if (vkrt.temporalFramesSinceReset < 1000) {
        vkrt.temporalFramesSinceReset++;
    }

    return qtrue;
}

static void RTX_DestroyDebugOverlayPipeline(void) {
    if (vkrt.debugOverlaySampler) {
        vkDestroySampler(vkrt.device, vkrt.debugOverlaySampler, NULL);
        vkrt.debugOverlaySampler = VK_NULL_HANDLE;
    }

    if (vkrt.debugOverlayDescriptorPool) {
        vkDestroyDescriptorPool(vkrt.device, vkrt.debugOverlayDescriptorPool, NULL);
        vkrt.debugOverlayDescriptorPool = VK_NULL_HANDLE;
    }

    if (vkrt.debugOverlaySetLayout) {
        vkDestroyDescriptorSetLayout(vkrt.device, vkrt.debugOverlaySetLayout, NULL);
        vkrt.debugOverlaySetLayout = VK_NULL_HANDLE;
    }

    if (vkrt.debugOverlayPipeline) {
        vkDestroyPipeline(vkrt.device, vkrt.debugOverlayPipeline, NULL);
        vkrt.debugOverlayPipeline = VK_NULL_HANDLE;
    }

    if (vkrt.debugOverlayPipelineLayout) {
        vkDestroyPipelineLayout(vkrt.device, vkrt.debugOverlayPipelineLayout, NULL);
        vkrt.debugOverlayPipelineLayout = VK_NULL_HANDLE;
    }
}

static qboolean RTX_UpdateDebugOverlayDescriptors(void) {
    if (!vkrt.debugOverlayDescriptorSet || !vkrt.debugOverlaySampler) {
        return qfalse;
    }

    if (!vkrt.rtImageView || !vk.color_image_view) {
        return qfalse;
    }

    VkImageView rtView = vkrt.rtImageView;
    VkImageView colorView = vk.color_image_view;
    VkImageView depthView = vk.depth_image_view_depth_only ? vk.depth_image_view_depth_only : colorView;

    VkDescriptorImageInfo depthInfo = {
        .sampler = vkrt.debugOverlaySampler,
        .imageView = depthView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL
    };

    VkDescriptorImageInfo normalInfo = {
        .sampler = vkrt.debugOverlaySampler,
        .imageView = rtView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL
    };

    VkDescriptorImageInfo motionInfo = normalInfo;
    VkDescriptorImageInfo rtSampleInfo = normalInfo;

    VkDescriptorImageInfo directInfo = normalInfo;
    VkDescriptorImageInfo indirectInfo = normalInfo;
    VkDescriptorImageInfo lightmapInfo = normalInfo;

    VkDescriptorImageInfo overlayImageInfo = {
        .sampler = VK_NULL_HANDLE,
        .imageView = rtView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL
    };

    VkWriteDescriptorSet writes[8] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkrt.debugOverlayDescriptorSet,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &depthInfo
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkrt.debugOverlayDescriptorSet,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &normalInfo
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkrt.debugOverlayDescriptorSet,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &motionInfo
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkrt.debugOverlayDescriptorSet,
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &rtSampleInfo
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkrt.debugOverlayDescriptorSet,
            .dstBinding = 4,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &overlayImageInfo
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkrt.debugOverlayDescriptorSet,
            .dstBinding = 5,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &directInfo
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkrt.debugOverlayDescriptorSet,
            .dstBinding = 6,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &indirectInfo
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkrt.debugOverlayDescriptorSet,
            .dstBinding = 7,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &lightmapInfo
        }
    };

    vkUpdateDescriptorSets(vkrt.device, ARRAY_LEN(writes), writes, 0, NULL);
    return qtrue;
}

void RTX_ApplyDebugOverlayCompute(VkCommandBuffer cmd, VkImage colorImage) {
    if (!cmd || colorImage == VK_NULL_HANDLE) {
        return;
    }

    if (!rtx.available || !RTX_IsEnabled()) {
        return;
    }

    if (!r_rtx_debug || r_rtx_debug->integer <= 0) {
        return;
    }

    if (!vkrt.rtImage || !rtOutputInitialized) {
        return;
    }

    if (rtOutputWidth == 0 || rtOutputHeight == 0) {
        return;
    }

    if (!RTX_FramebufferCopySupported(
            vkrt.rtImageFormat,
            vk.color_format,
            "debug overlay copy",
            qfalse)) {
        return;
    }

    (void)RTX_EnsureDebugOverlayPipeline();
    (void)RTX_UpdateDebugOverlayDescriptors();

    VkImageCopy copyRegion = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { rtOutputWidth, rtOutputHeight, 1 }
    };

    VkImageMemoryBarrier prepareColor = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = vk_image_get_layout_or( colorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = colorImage,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &prepareColor);

    vk_image_set_layout( colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

    if ( vk.color_format == VK_FORMAT_R32G32B32A32_SFLOAT ) {
        vkCmdCopyImage(cmd,
                       vkrt.rtImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copyRegion);
    } else {
        VkImageBlit blitRegion = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = {
                { 0, 0, 0 },
                { (int32_t)rtOutputWidth, (int32_t)rtOutputHeight, 1 }
            },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = {
                { 0, 0, 0 },
                { (int32_t)rtOutputWidth, (int32_t)rtOutputHeight, 1 }
            }
        };

        vkCmdBlitImage(cmd,
                       vkrt.rtImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blitRegion,
                       VK_FILTER_NEAREST);
    }

    VkImageMemoryBarrier restoreColor = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = vk_image_get_layout_or( colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ),
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = colorImage,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &restoreColor);

    vk_image_set_layout( colorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
}
 

/*
================
RTX_UpdateBLASGPU

Update dynamic BLAS using refit (update mode)
================
*/

/*
================
RTX_UpdateBLASGPU

Update dynamic BLAS using refit (update mode)
================
*/
qboolean RTX_UpdateBLASGPU(rtxBLAS_t *blas) {
    if (!blas || !blas->gpuData || !blas->isDynamic) {
        return qfalse;
    }

    rtxBLASGPU_t *gpu = (rtxBLASGPU_t *)blas->gpuData;
    VkDeviceSize vertexSize = sizeof(vec3_t) * (VkDeviceSize)blas->numVertices;
    VkDeviceSize shaderVertexSize = sizeof(rtxShaderVertex_t) * (VkDeviceSize)blas->numVertices;

    // 1. Update position-only vertex buffer for AS construction
    void *mapped = NULL;
    VkResult res = vkMapMemory(vkrt.device, gpu->vertexMemory, 0, vertexSize, 0, &mapped);
    if (res == VK_SUCCESS) {
        memcpy(mapped, blas->vertices, vertexSize);
        vkUnmapMemory(vkrt.device, gpu->vertexMemory);
    } else {
        ri.Printf(PRINT_WARNING, "RTX_UpdateBLASGPU: vkMapMemory failed for vertex buffer\n");
        return qfalse;
    }

    // 2. Update interleaved shader vertex buffer
    res = vkMapMemory(vkrt.device, gpu->shaderVertexMemory, 0, shaderVertexSize, 0, &mapped);
    if (res == VK_SUCCESS) {
        rtxShaderVertex_t *packed = (rtxShaderVertex_t *)mapped;
        for (int i = 0; i < blas->numVertices; i++) {
            VectorCopy(blas->vertices[i], packed[i].position);
            if (blas->normals) VectorCopy(blas->normals[i], packed[i].normal);
            // Other attributes like texCoords/colors are assumed static for refit
        }
        vkUnmapMemory(vkrt.device, gpu->shaderVertexMemory);
    } else {
        ri.Printf(PRINT_WARNING, "RTX_UpdateBLASGPU: vkMapMemory failed for shader vertex buffer\n");
        return qfalse;
    }

    // 3. Perform AS update (refit)
    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry = {
            .triangles = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                .vertexData = { .deviceAddress = RTX_GetBufferDeviceAddressVK(gpu->vertexBuffer) },
                .vertexStride = sizeof(vec3_t),
                .maxVertex = blas->numVertices - 1,
                .indexType = VK_INDEX_TYPE_UINT32,
                .indexData = { .deviceAddress = RTX_GetBufferDeviceAddressVK(gpu->indexBuffer) },
            }
        },
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR
    };

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,
        .srcAccelerationStructure = gpu->as,
        .dstAccelerationStructure = gpu->as,
        .geometryCount = 1,
        .pGeometries = &geometry
    };

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    uint32_t primitiveCount = blas->numTriangles;
    qvkGetAccelerationStructureBuildSizesKHR(vkrt.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);

    VkDeviceMemory scratchMemory;
    VkBuffer scratchBuffer = RTX_AllocateScratchBuffer(sizeInfo.updateScratchSize, &scratchMemory);
    if (!scratchBuffer) return qfalse;

    if (RTX_BeginImmediateCommands("RefitBLAS") != VK_SUCCESS) return qfalse;

    buildInfo.scratchData.deviceAddress = RTX_GetBufferDeviceAddressVK(scratchBuffer);
    VkAccelerationStructureBuildRangeInfoKHR range = { .primitiveCount = blas->numTriangles };
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
    qvkCmdBuildAccelerationStructuresKHR(vkrt.commandBuffer, 1, &buildInfo, &ranges);

    if (vkEndCommandBuffer(vkrt.commandBuffer) != VK_SUCCESS) return qfalse;
    if (RTX_SubmitImmediateCommands("RefitBLAS") != VK_SUCCESS) return qfalse;

    return qtrue;
}
