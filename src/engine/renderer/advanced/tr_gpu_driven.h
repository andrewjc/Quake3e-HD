/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

This file is part of Quake3e-HD.

Quake3e-HD is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Quake3e-HD is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake3e-HD; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
===========================================================================
*/
// tr_gpu_driven.h - GPU-driven rendering system

#ifndef __TR_GPU_DRIVEN_H
#define __TR_GPU_DRIVEN_H

#include "../core/tr_local.h"
#include "vulkan/vulkan.h"

// Maximum number of draws in a single indirect buffer
#define MAX_INDIRECT_DRAWS      65536
#define MAX_MESHLETS           262144
#define MAX_INSTANCES          131072
#define MAX_BINDLESS_TEXTURES   16384
#define MAX_BINDLESS_BUFFERS     8192

// GPU buffer sizes
#define INDIRECT_BUFFER_SIZE    (MAX_INDIRECT_DRAWS * sizeof(VkDrawIndexedIndirectCommand))
#define MESHLET_BUFFER_SIZE     (MAX_MESHLETS * sizeof(gpuMeshlet_t))
#define INSTANCE_BUFFER_SIZE    (MAX_INSTANCES * sizeof(gpuInstance_t))
#define VISIBILITY_BUFFER_SIZE  (MAX_INDIRECT_DRAWS * sizeof(uint32_t))

// Draw command structure for GPU
typedef struct gpuDrawCommand_s {
    VkDrawIndexedIndirectCommand   cmd;
    uint32_t                        instanceOffset;
    uint32_t                        materialID;
    uint32_t                        meshID;
    uint32_t                        visibilityID;
} gpuDrawCommand_t;

// Instance data on GPU
typedef struct gpuInstance_s {
    mat4_t      modelMatrix;
    mat4_t      normalMatrix;
    vec4_t      color;
    uint32_t    materialID;
    uint32_t    flags;
    uint32_t    lightMask;
    uint32_t    pad;
} gpuInstance_t;

// Meshlet structure for mesh shading
typedef struct gpuMeshlet_s {
    uint32_t    vertexOffset;
    uint32_t    vertexCount;
    uint32_t    primitiveOffset;
    uint32_t    primitiveCount;
    vec3_t      center;
    float       radius;
    vec3_t      coneAxis;
    float       coneCutoff;
    vec3_t      boundingMin;
    vec3_t      boundingMax;
} gpuMeshlet_t;

// GPU culling data
typedef struct gpuCullData_s {
    mat4_t      viewProjMatrix;
    vec4_t      frustumPlanes[6];
    vec3_t      viewOrigin;
    float       lodBias;
    uint32_t    drawCount;
    uint32_t    meshletCount;
    uint32_t    hiZWidth;
    uint32_t    hiZHeight;
} gpuCullData_t;

// Multi-draw indirect system
typedef struct multiDrawIndirect_s {
    VkBuffer                drawBuffer;
    VkDeviceMemory         drawMemory;
    VkBuffer                countBuffer;
    VkDeviceMemory         countMemory;
    VkBuffer                instanceBuffer;
    VkDeviceMemory         instanceMemory;
    VkBuffer                visibilityBuffer;
    VkDeviceMemory         visibilityMemory;
    
    gpuDrawCommand_t       *drawCommands;
    gpuInstance_t          *instances;
    uint32_t               *visibilityData;
    
    uint32_t                numDraws;
    uint32_t                numInstances;
    uint32_t                maxDraws;
    uint32_t                maxInstances;
    
    VkCommandBuffer         cullCommandBuffer;
    VkPipeline              cullPipeline;
    VkPipelineLayout        cullPipelineLayout;
    VkDescriptorSet         cullDescriptorSet;
} multiDrawIndirect_t;

// Mesh shading pipeline state
typedef struct meshShadingPipeline_s {
    VkPipeline              taskShaderPipeline;
    VkPipeline              meshShaderPipeline;
    VkPipelineLayout        pipelineLayout;
    VkDescriptorSetLayout   descriptorSetLayout;
    VkDescriptorSet         descriptorSet;
    
    VkBuffer                meshletBuffer;
    VkDeviceMemory         meshletMemory;
    gpuMeshlet_t           *meshlets;
    uint32_t                numMeshlets;
    
    qboolean                supported;
    uint32_t                maxMeshOutputVertices;
    uint32_t                maxMeshOutputPrimitives;
    uint32_t                maxMeshWorkGroupSize[3];
} meshShadingPipeline_t;

// Variable Rate Shading state (Vulkan-specific)
typedef struct vrsVulkanState_s {
    VkExtent2D              tileSize;
    VkImage                 rateImage;
    VkImageView             rateImageView;
    VkDeviceMemory         rateImageMemory;
    
    VkShadingRatePaletteNV  palette;
    uint32_t                shadingRateCount;
    
    qboolean                supported;
    qboolean                perPrimitive;
    qboolean                perViewport;
    
    // Shading rate modes
    uint32_t                defaultRate;
    uint32_t                distanceRate;
    uint32_t                motionRate;
} vrsVulkanState_t;

// Bindless resource system
typedef struct bindlessResources_s {
    VkDescriptorPool        descriptorPool;
    VkDescriptorSetLayout   descriptorSetLayout;
    VkDescriptorSet         descriptorSet;
    
    // Texture arrays
    VkSampler              *samplers;
    VkImageView            *textureViews;
    uint32_t                numTextures;
    uint32_t                maxTextures;
    
    // Buffer arrays
    VkBuffer               *buffers;
    VkDeviceSize           *bufferOffsets;
    uint32_t                numBuffers;
    uint32_t                maxBuffers;
    
    // Resource indices
    uint32_t               *textureIndices;
    uint32_t               *bufferIndices;
    
    qboolean                supported;
} bindlessResources_t;

// GPU-driven rendering context
typedef struct gpuDrivenContext_s {
    multiDrawIndirect_t     multiDraw;
    meshShadingPipeline_t   meshShading;
    vrsVulkanState_t        vrs;
    bindlessResources_t     bindless;
    
    // Hierarchical-Z buffer for GPU occlusion culling
    VkImage                 hiZBuffer;
    VkImageView             hiZBufferView;
    VkDeviceMemory         hiZBufferMemory;
    uint32_t                hiZMipLevels;
    
    // GPU timestamp queries
    VkQueryPool             timestampPool;
    uint64_t               *timestamps;
    uint32_t                timestampCount;
    
    // Statistics
    uint32_t                totalDrawCalls;
    uint32_t                culledDrawCalls;
    uint32_t                totalMeshlets;
    uint32_t                culledMeshlets;
} gpuDrivenContext_t;

// Global GPU-driven rendering state
extern gpuDrivenContext_t gpuContext;

// Initialization and shutdown
void R_InitGPUDriven( void );
void R_ShutdownGPUDriven( void );

// Multi-draw indirect
void R_BeginMultiDrawIndirect( void );
void R_AddDrawIndirect( const drawSurf_t *drawSurf );
void R_EndMultiDrawIndirect( void );
void R_ExecuteMultiDrawIndirect( VkCommandBuffer commandBuffer );

// GPU culling
void R_SetupGPUCulling( const viewParms_t *viewParms );
void R_ExecuteGPUCulling( VkCommandBuffer commandBuffer );
void R_UpdateHiZBuffer( VkCommandBuffer commandBuffer );

// Mesh shading
qboolean R_InitMeshShading( void );
void R_ShutdownMeshShading( void );
void R_BuildMeshlets( srfTriangles_t *surface );
void R_DrawMeshShading( VkCommandBuffer commandBuffer, const drawSurf_t *drawSurf );

// Variable Rate Shading
qboolean R_InitVRS( void );
void R_ShutdownVRS( void );
void R_SetShadingRate( VkCommandBuffer commandBuffer, uint32_t rate );
void R_UpdateShadingRateImage( VkCommandBuffer commandBuffer, const viewParms_t *viewParms );

// Bindless resources
qboolean R_InitBindlessResources( void );
void R_ShutdownBindlessResources( void );
uint32_t R_RegisterBindlessTexture( VkImageView imageView, VkSampler sampler );
uint32_t R_RegisterBindlessBuffer( VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range );
void R_UpdateBindlessDescriptors( void );

// Helper functions
void R_CreateGPUBuffer( VkBuffer *buffer, VkDeviceMemory *memory, VkDeviceSize size, 
                       VkBufferUsageFlags usage, VkMemoryPropertyFlags properties );
void R_CreateGPUImage( VkImage *image, VkDeviceMemory *memory, VkImageView *view,
                      uint32_t width, uint32_t height, VkFormat format,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect );
void R_TransitionImageLayout( VkCommandBuffer commandBuffer, VkImage image,
                             VkImageLayout oldLayout, VkImageLayout newLayout,
                             VkImageAspectFlags aspect );

// Compute shader compilation
VkShaderModule R_CreateComputeShader( const uint32_t *spirvCode, size_t codeSize );
VkPipeline R_CreateComputePipeline( VkShaderModule shader, VkPipelineLayout layout );

// Debug and statistics
void R_DrawGPUStats( void );
void R_DumpGPUTimestamps( void );

#endif // __TR_GPU_DRIVEN_H