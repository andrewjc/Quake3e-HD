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
// tr_query.c - GPU Query Management System Implementation

#include "tr_query.h"
#include "../core/tr_local.h"

queryManager_t queryManager;

static gpuQuery_t* R_AllocateQuery( queryType_t type );
static void R_FreeQuery( gpuQuery_t *query );

qboolean R_InitQueryManager( VkDevice device, VkPhysicalDevice physicalDevice ) {
    ri.Printf( PRINT_ALL, "Initializing GPU query manager...\n" );
    
    Com_Memset( &queryManager, 0, sizeof( queryManager ) );
    
    // Get timestamp period
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties( physicalDevice, &props );
    queryManager.timestampPeriod = props.limits.timestampPeriod;
    
    // Set pipeline statistics flags
    queryManager.pipelineStatFlags = 
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
        VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
    
    queryManager.timestampsEnabled = qtrue;
    queryManager.initialized = qtrue;
    
    ri.Printf( PRINT_ALL, "GPU query manager initialized (timestamp period: %.3f ns)\n", 
              queryManager.timestampPeriod );
    return qtrue;
}

void R_ShutdownQueryManager( void ) {
    if ( !queryManager.initialized ) {
        return;
    }
    
    // Destroy all query pools
    for ( uint32_t i = 0; i < queryManager.poolCount; i++ ) {
        R_DestroyQueryPool( &queryManager.pools[i] );
    }
    
    Com_Memset( &queryManager, 0, sizeof( queryManager ) );
    ri.Printf( PRINT_ALL, "GPU query manager shutdown\n" );
}

queryPool_t* R_CreateQueryPool( queryType_t type, uint32_t count ) {
    if ( !queryManager.initialized || queryManager.poolCount >= QUERY_MAX_POOLS ) {
        return NULL;
    }
    
    queryPool_t *pool = &queryManager.pools[queryManager.poolCount++];
    Com_Memset( pool, 0, sizeof( queryPool_t ) );
    
    pool->type = type;
    pool->count = count;
    
    // Allocate query storage
    pool->queries = ri.Malloc( sizeof( gpuQuery_t ) * count );
    Com_Memset( pool->queries, 0, sizeof( gpuQuery_t ) * count );
    
    // Initialize queries
    for ( uint32_t i = 0; i < count; i++ ) {
        pool->queries[i].type = type;
        pool->queries[i].poolIndex = queryManager.poolCount - 1;
        pool->queries[i].queryIndex = i;
        pool->queries[i].pool = pool->pool;
    }
    
    VkQueryPoolCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .queryCount = count
    };
    
    switch ( type ) {
        case QUERY_TYPE_TIMESTAMP:
            createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            break;
        case QUERY_TYPE_OCCLUSION:
            createInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
            break;
        case QUERY_TYPE_PIPELINE_STATS:
            createInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            createInfo.pipelineStatistics = queryManager.pipelineStatFlags;
            break;
        default:
            ri.Free( pool->queries );
            queryManager.poolCount--;
            return NULL;
    }
    
    VkResult result = vkCreateQueryPool( vk.device, &createInfo, NULL, &pool->pool );
    
    if ( result != VK_SUCCESS ) {
        ri.Printf( PRINT_WARNING, "Failed to create query pool: %d\n", result );
        ri.Free( pool->queries );
        queryManager.poolCount--;
        return NULL;
    }
    
    return pool;
}

void R_DestroyQueryPool( queryPool_t *pool ) {
    if ( !pool || !pool->pool ) {
        return;
    }
    
    vkDestroyQueryPool( vk.device, pool->pool, NULL );
    
    if ( pool->queries ) {
        ri.Free( pool->queries );
    }
    
    Com_Memset( pool, 0, sizeof( queryPool_t ) );
}

void R_ResetQueryPool( VkCommandBuffer cmd, queryPool_t *pool ) {
    if ( !pool || !pool->pool ) {
        return;
    }
    
    vkCmdResetQueryPool( cmd, pool->pool, 0, pool->count );
    pool->used = 0;
    pool->nextQuery = 0;
}

gpuQuery_t* R_BeginQuery( VkCommandBuffer cmd, queryType_t type ) {
    if ( !queryManager.initialized ) {
        return NULL;
    }
    
    gpuQuery_t *query = R_AllocateQuery( type );
    if ( !query ) {
        return NULL;
    }
    
    // Find appropriate pool
    queryPool_t *pool = NULL;
    for ( uint32_t i = 0; i < queryManager.poolCount; i++ ) {
        if ( queryManager.pools[i].type == type && 
             queryManager.pools[i].used < queryManager.pools[i].count ) {
            pool = &queryManager.pools[i];
            break;
        }
    }
    
    if ( !pool ) {
        // Create new pool if needed
        pool = R_CreateQueryPool( type, QUERY_MAX_PER_POOL );
        if ( !pool ) {
            return NULL;
        }
    }
    
    query->pool = pool->pool;
    query->queryIndex = pool->nextQuery++;
    pool->used++;
    
    if ( type == QUERY_TYPE_OCCLUSION ) {
        vkCmdBeginQuery( cmd, pool->pool, query->queryIndex, 0 );
    } else if ( type == QUERY_TYPE_PIPELINE_STATS ) {
        vkCmdBeginQuery( cmd, pool->pool, query->queryIndex, 0 );
    }
    
    query->active = qtrue;
    query->frameIssued = tr.frameCount;
    
    return query;
}

void R_EndQuery( VkCommandBuffer cmd, gpuQuery_t *query ) {
    if ( !query || !query->active ) {
        return;
    }
    
    if ( query->type == QUERY_TYPE_OCCLUSION || 
         query->type == QUERY_TYPE_PIPELINE_STATS ) {
        vkCmdEndQuery( cmd, query->pool, query->queryIndex );
    }
    
    query->active = qfalse;
}

gpuQuery_t* R_WriteTimestamp( VkCommandBuffer cmd, VkPipelineStageFlagBits stage ) {
    if ( !queryManager.initialized || !queryManager.timestampsEnabled ) {
        return NULL;
    }
    
    gpuQuery_t *query = R_BeginQuery( cmd, QUERY_TYPE_TIMESTAMP );
    if ( !query ) {
        return NULL;
    }
    
    vkCmdWriteTimestamp( cmd, stage, query->pool, query->queryIndex );
    
    query->active = qfalse;
    query->available = qfalse;
    
    return query;
}

qboolean R_GetQueryResult( gpuQuery_t *query, uint64_t *result ) {
    if ( !query || !result ) {
        return qfalse;
    }
    
    if ( query->available ) {
        *result = query->result;
        return qtrue;
    }
    
    VkResult res = vkGetQueryPoolResults( vk.device, query->pool, query->queryIndex, 1,
                                          sizeof( uint64_t ), &query->result,
                                          sizeof( uint64_t ), 
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
    
    if ( res == VK_SUCCESS ) {
        query->available = qtrue;
        *result = query->result;
        return qtrue;
    }
    
    return qfalse;
}

void R_CollectQueryResults( void ) {
    if ( !queryManager.initialized ) {
        return;
    }
    
    for ( uint32_t i = 0; i < queryManager.activeCount; i++ ) {
        gpuQuery_t *query = &queryManager.activeQueries[i];
        
        if ( !query->available ) {
            uint64_t result;
            if ( R_GetQueryResult( query, &result ) ) {
                // Query result is now available
            }
        }
    }
}

gpuQuery_t* R_BeginOcclusionQuery( VkCommandBuffer cmd, qboolean precise ) {
    gpuQuery_t *query = R_BeginQuery( cmd, QUERY_TYPE_OCCLUSION );
    
    if ( query && precise ) {
        // Set precise flag if supported
        vkCmdBeginQuery( cmd, query->pool, query->queryIndex, 
                        VK_QUERY_CONTROL_PRECISE_BIT );
    }
    
    return query;
}

uint32_t R_GetOcclusionResult( gpuQuery_t *query ) {
    if ( !query || query->type != QUERY_TYPE_OCCLUSION ) {
        return 0;
    }
    
    uint64_t result = 0;
    if ( R_GetQueryResult( query, &result ) ) {
        return (uint32_t)result;
    }
    
    return 0;
}

void R_InsertTimestamp( VkCommandBuffer cmd, const char *label ) {
    if ( !queryManager.timestampsEnabled ) {
        return;
    }
    
    gpuQuery_t *query = R_WriteTimestamp( cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT );
    
    if ( query && label ) {
        // Store label for debugging
        query->userData = (void*)label;
    }
}

float R_GetTimestampDelta( gpuQuery_t *start, gpuQuery_t *end ) {
    if ( !start || !end || !start->available || !end->available ) {
        return 0;
    }
    
    uint64_t delta = end->result - start->result;
    
    // Convert to milliseconds
    return ( delta * queryManager.timestampPeriod ) / 1000000.0f;
}

static gpuQuery_t* R_AllocateQuery( queryType_t type ) {
    if ( queryManager.activeCount >= QUERY_MAX_ACTIVE ) {
        return NULL;
    }
    
    gpuQuery_t *query = &queryManager.activeQueries[queryManager.activeCount++];
    Com_Memset( query, 0, sizeof( gpuQuery_t ) );
    query->type = type;
    
    return query;
}

static void R_FreeQuery( gpuQuery_t *query ) {
    if ( !query ) {
        return;
    }
    
    Com_Memset( query, 0, sizeof( gpuQuery_t ) );
}