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
// tr_query.h - GPU Query Management System

#ifndef __TR_QUERY_H
#define __TR_QUERY_H

#include "../core/tr_local.h"
#include "vulkan/vulkan.h"

#define QUERY_MAX_POOLS         16
#define QUERY_MAX_PER_POOL     256
#define QUERY_MAX_ACTIVE       128

typedef enum {
    QUERY_TYPE_TIMESTAMP = 0,
    QUERY_TYPE_OCCLUSION,
    QUERY_TYPE_PIPELINE_STATS,
    QUERY_TYPE_PERFORMANCE
} queryType_t;

typedef struct gpuQuery_s {
    queryType_t            type;
    uint32_t               poolIndex;
    uint32_t               queryIndex;
    VkQueryPool            pool;
    uint64_t               result;
    qboolean               available;
    qboolean               active;
    uint64_t               frameIssued;
    void                  *userData;
} gpuQuery_t;

typedef struct queryPool_s {
    VkQueryPool            pool;
    queryType_t            type;
    uint32_t               count;
    uint32_t               used;
    uint32_t               nextQuery;
    gpuQuery_t            *queries;
} queryPool_t;

typedef struct queryManager_s {
    qboolean               initialized;
    queryPool_t            pools[QUERY_MAX_POOLS];
    uint32_t               poolCount;
    gpuQuery_t             activeQueries[QUERY_MAX_ACTIVE];
    uint32_t               activeCount;
    VkQueryPipelineStatisticFlags pipelineStatFlags;
    qboolean               timestampsEnabled;
    float                  timestampPeriod;
} queryManager_t;

extern queryManager_t queryManager;

// Initialization
qboolean R_InitQueryManager( VkDevice device, VkPhysicalDevice physicalDevice );
void R_ShutdownQueryManager( void );

// Pool management
queryPool_t* R_CreateQueryPool( queryType_t type, uint32_t count );
void R_DestroyQueryPool( queryPool_t *pool );
void R_ResetQueryPool( VkCommandBuffer cmd, queryPool_t *pool );

// Query operations
gpuQuery_t* R_BeginQuery( VkCommandBuffer cmd, queryType_t type );
void R_EndQuery( VkCommandBuffer cmd, gpuQuery_t *query );
gpuQuery_t* R_WriteTimestamp( VkCommandBuffer cmd, VkPipelineStageFlagBits stage );

// Result retrieval
qboolean R_GetQueryResult( gpuQuery_t *query, uint64_t *result );
void R_CollectQueryResults( void );
void R_WaitForQuery( gpuQuery_t *query );

// Occlusion queries
gpuQuery_t* R_BeginOcclusionQuery( VkCommandBuffer cmd, qboolean precise );
uint32_t R_GetOcclusionResult( gpuQuery_t *query );

// Pipeline statistics
void R_BeginPipelineStats( VkCommandBuffer cmd );
void R_EndPipelineStats( VkCommandBuffer cmd );
void R_GetPipelineStats( VkQueryPipelineStatisticFlags *stats );

// Timestamp queries
void R_InsertTimestamp( VkCommandBuffer cmd, const char *label );
float R_GetTimestampDelta( gpuQuery_t *start, gpuQuery_t *end );

// Performance queries (if supported)
void R_BeginPerformanceQuery( VkCommandBuffer cmd );
void R_EndPerformanceQuery( VkCommandBuffer cmd );

#endif // __TR_QUERY_H