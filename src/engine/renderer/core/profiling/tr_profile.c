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
// tr_profile.c - GPU Profiling System implementation

#include "tr_profile.h"
#include "tr_debug.h"
#include "../../vulkan/vk.h"
#include <time.h>
#include <float.h>

#ifdef _WIN32
#include <windows.h>
#endif

// VK_CHECK macro for error handling
#ifndef VK_CHECK
#define VK_CHECK( function_call ) { \
    VkResult res = function_call; \
    if ( res < 0 ) { \
        ri.Error( ERR_FATAL, "Vulkan: %s returned error %d", #function_call, res ); \
    } \
}
#endif

// Forward declarations
static void R_DestroyQueryPools( void );
static qboolean R_CreateQueryPools( void );
static void R_UpdateFrameStatistics( frameProfile_t *frame );
static void R_PrintTimerHierarchy( profileTimer_t *timer, uint32_t depth );

// Global profiling state
profileState_t profileState;

// CVars
cvar_t *r_profile;
cvar_t *r_profileGPU;
cvar_t *r_profileCPU;
cvar_t *r_profileOverlay;
cvar_t *r_profileLog;
cvar_t *r_profileReport;

// Timer type names
static const char *timerTypeNames[] = {
    "Frame",
    "Shadow",
    "GBuffer",
    "Lighting",
    "PostProcess",
    "UI",
    "Present",
    "Compute",
    "Transfer",
    "Custom"
};

// Counter type names
static const char *counterTypeNames[] = {
    "DrawCalls",
    "Triangles",
    "Vertices",
    "Pipelines",
    "Descriptors",
    "Buffers",
    "Textures",
    "Shaders",
    "Custom"
};

/*
================
R_InitProfiling

Initialize profiling system
================
*/
qboolean R_InitProfiling( void ) {
    Com_Memset( &profileState, 0, sizeof( profileState ) );
    
    // Register CVars
    r_profile = ri.Cvar_Get( "r_profile", "0", CVAR_ARCHIVE );
    r_profileGPU = ri.Cvar_Get( "r_profileGPU", "1", CVAR_ARCHIVE );
    r_profileCPU = ri.Cvar_Get( "r_profileCPU", "1", CVAR_ARCHIVE );
    r_profileOverlay = ri.Cvar_Get( "r_profileOverlay", "0", CVAR_CHEAT );
    r_profileLog = ri.Cvar_Get( "r_profileLog", "0", CVAR_ARCHIVE );
    r_profileReport = ri.Cvar_Get( "r_profileReport", "0", CVAR_CHEAT );
    
    // Create GPU query pools
    if ( !R_CreateQueryPools() ) {
        ri.Printf( PRINT_WARNING, "Failed to create GPU query pools\n" );
        return qfalse;
    }
    
    // Get timestamp period for GPU timing
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties( vk.physical_device, &props );
    profileState.timestampPeriod = props.limits.timestampPeriod;
    
    // Create root timer
    profileState.rootTimer = R_CreateProfileTimer( "Frame", PROFILE_TIMER_FRAME );
    profileState.currentTimer = profileState.rootTimer;
    
    // Create default counters
    R_CreateProfileCounter( "DrawCalls", PROFILE_COUNTER_DRAWCALLS );
    R_CreateProfileCounter( "Triangles", PROFILE_COUNTER_TRIANGLES );
    R_CreateProfileCounter( "Vertices", PROFILE_COUNTER_VERTICES );
    
    // Register main thread
    profileState.mainThreadId = 1; // Main thread ID (no threading API available)
    R_RegisterProfileThread( "Main" );
    
    // Set default configuration
    profileState.enabled = r_profile->integer ? qtrue : qfalse;
    profileState.gpuTimingEnabled = r_profileGPU->integer ? qtrue : qfalse;
    profileState.cpuTimingEnabled = r_profileCPU->integer ? qtrue : qfalse;
    profileState.showOverlay = r_profileOverlay->integer ? qtrue : qfalse;
    profileState.logToFile = r_profileLog->integer ? qtrue : qfalse;
    profileState.autoReport = r_profileReport->integer ? qtrue : qfalse;
    profileState.reportInterval = 60;  // Report every 60 frames
    profileState.targetFrameTime = 16.67f;  // 60 FPS target
    
    profileState.initialized = qtrue;
    
    ri.Printf( PRINT_ALL, "Profiling system initialized\n" );
    return qtrue;
}

/*
================
R_ShutdownProfiling

Shutdown profiling system
================
*/
void R_ShutdownProfiling( void ) {
    if ( !profileState.initialized ) {
        return;
    }
    
    // Generate final report
    if ( profileState.autoReport ) {
        R_GenerateProfileReport();
    }
    
    // Close log file if open
    // NOTE: File writing is not supported in renderer - logging disabled
    profileState.logFile = 0;
    
    // Destroy query pools
    R_DestroyQueryPools();
    
    Com_Memset( &profileState, 0, sizeof( profileState ) );
}

/*
================
R_CreateQueryPools

Create GPU query pools
================
*/
static qboolean R_CreateQueryPools( void ) {
    profileQueryPool_t *pool = &profileState.queryPool;
    
    // Create timestamp query pool
    pool->timestampCount = 1024;
    
    VkQueryPoolCreateInfo timestampInfo = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = pool->timestampCount
    };
    
    VK_CHECK( vkCreateQueryPool( vk.device, &timestampInfo, NULL, &pool->timestampPool ) );
    
    // Create pipeline statistics query pool (only if feature is available)
    if ( vk.pipelineStatisticsQuery ) {
        pool->statisticsCount = 256;
        pool->statisticsFlags = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                               VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                               VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                               VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        
        VkQueryPoolCreateInfo statisticsInfo = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
            .queryCount = pool->statisticsCount,
            .pipelineStatistics = pool->statisticsFlags
        };
        
        VK_CHECK( vkCreateQueryPool( vk.device, &statisticsInfo, NULL, &pool->statisticsPool ) );
    } else {
        pool->statisticsPool = VK_NULL_HANDLE;
        pool->statisticsCount = 0;
        pool->statisticsFlags = 0;
    }
    
    // Create occlusion query pool
    pool->occlusionCount = 256;
    
    VkQueryPoolCreateInfo occlusionInfo = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_OCCLUSION,
        .queryCount = pool->occlusionCount
    };
    
    VK_CHECK( vkCreateQueryPool( vk.device, &occlusionInfo, NULL, &pool->occlusionPool ) );
    
    // Allocate results buffer
    pool->timestampResults = ri.Malloc( sizeof( uint64_t ) * pool->timestampCount );
    
    // Note: Query pools will be reset when first used in a recording command buffer
    // We cannot reset them here as we may not have a recording command buffer yet
    
    return qtrue;
}

/*
================
R_DestroyQueryPools

Destroy GPU query pools
================
*/
static void R_DestroyQueryPools( void ) {
    profileQueryPool_t *pool = &profileState.queryPool;
    
    if ( pool->timestampPool ) {
        vkDestroyQueryPool( vk.device, pool->timestampPool, NULL );
    }
    
    if ( pool->statisticsPool ) {
        vkDestroyQueryPool( vk.device, pool->statisticsPool, NULL );
    }
    
    if ( pool->occlusionPool ) {
        vkDestroyQueryPool( vk.device, pool->occlusionPool, NULL );
    }
    
    if ( pool->timestampResults ) {
        ri.Free( pool->timestampResults );
    }
}

/*
================
R_BeginFrameProfiling

Begin frame profiling
================
*/
void R_BeginFrameProfiling( void ) {
    if ( !profileState.initialized || !profileState.enabled ) {
        return;
    }
    
    // Reset query pools on first use (when we have a recording command buffer)
    if ( !profileState.queryPoolsReset && vk.cmd && vk.cmd->command_buffer ) {
        VkCommandBuffer cmd = vk.cmd->command_buffer;
        profileQueryPool_t *pool = &profileState.queryPool;
        
        vkCmdResetQueryPool( cmd, pool->timestampPool, 0, pool->timestampCount );
        if ( pool->statisticsPool != VK_NULL_HANDLE ) {
            vkCmdResetQueryPool( cmd, pool->statisticsPool, 0, pool->statisticsCount );
        }
        vkCmdResetQueryPool( cmd, pool->occlusionPool, 0, pool->occlusionCount );
        
        profileState.queryPoolsReset = qtrue;
    }
    
    // Start frame timer
    R_StartProfileTimer( profileState.rootTimer );
    
    // Reset frame counters
    R_ResetFrameStats();
    
    // Record frame start time
    frameProfile_t *frame = &profileState.frameHistory[profileState.currentFrame];
    frame->frameNumber = profileState.frameCount;
    frame->cpuStartTime = R_GetCPUTimestamp();
    
    // Reset query pool indices
    profileState.queryPool.currentTimestamp = 0;
    profileState.queryPool.currentStatistics = 0;
}

/*
================
R_EndFrameProfiling

End frame profiling
================
*/
void R_EndFrameProfiling( void ) {
    if ( !profileState.initialized || !profileState.enabled ) {
        return;
    }
    
    // End frame timer
    R_EndProfileTimer( profileState.rootTimer );
    
    // Record frame end time
    frameProfile_t *frame = &profileState.frameHistory[profileState.currentFrame];
    frame->cpuEndTime = R_GetCPUTimestamp();
    frame->cpuFrameTime = R_TimestampToMilliseconds( frame->cpuStartTime, frame->cpuEndTime );
    
    // Collect GPU timings
    R_CollectGPUTimings();
    
    // Update statistics
    R_UpdateFrameStatistics( frame );
    
    // Generate report if needed
    if ( profileState.autoReport && 
         profileState.frameCount - profileState.lastReportFrame >= profileState.reportInterval ) {
        R_GenerateProfileReport();
        profileState.lastReportFrame = profileState.frameCount;
    }
    
    // Draw overlay if enabled
    if ( profileState.showOverlay ) {
        R_DrawProfileOverlay();
    }
    
    // Advance frame
    profileState.currentFrame = ( profileState.currentFrame + 1 ) % PROFILE_HISTORY_FRAMES;
    profileState.frameCount++;
}

/*
================
R_CreateProfileTimer

Create a profile timer
================
*/
profileTimer_t* R_CreateProfileTimer( const char *name, profileTimerType_t type ) {
    if ( profileState.numTimers >= PROFILE_MAX_TIMERS ) {
        ri.Printf( PRINT_WARNING, "R_CreateProfileTimer: MAX_TIMERS reached\n" );
        return NULL;
    }
    
    profileTimer_t *timer = &profileState.timers[profileState.numTimers++];
    Q_strncpyz( timer->name, name, sizeof( timer->name ) );
    timer->type = type;
    timer->enabled = qtrue;
    timer->minTime = FLT_MAX;
    
    return timer;
}

/*
================
R_StartProfileTimer

Start a profile timer
================
*/
void R_StartProfileTimer( profileTimer_t *timer ) {
    if ( !timer || !timer->enabled || timer->active ) {
        return;
    }
    
    timer->active = qtrue;
    
    // CPU timing
    if ( profileState.cpuTimingEnabled ) {
        timer->cpuTime = R_GetCPUTimestamp();
    }
    
    // GPU timing
    if ( profileState.gpuTimingEnabled && profileState.queryPool.currentTimestamp < profileState.queryPool.timestampCount - 1 ) {
        timer->queryIndex[0] = profileState.queryPool.currentTimestamp++;
        
        VkCommandBuffer cmd = vk.cmd->command_buffer;
        vkCmdWriteTimestamp( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           profileState.queryPool.timestampPool,
                           timer->queryIndex[0] );
    }
}

/*
================
R_EndProfileTimer

End a profile timer
================
*/
void R_EndProfileTimer( profileTimer_t *timer ) {
    if ( !timer || !timer->enabled || !timer->active ) {
        return;
    }
    
    timer->active = qfalse;
    
    // CPU timing
    if ( profileState.cpuTimingEnabled ) {
        uint64_t endTime = R_GetCPUTimestamp();
        float deltaTime = R_TimestampToMilliseconds( timer->cpuTime, endTime );
        timer->cpuTime = endTime - timer->cpuTime;
        
        // Update statistics
        timer->samples++;
        timer->avgTime = ( timer->avgTime * ( timer->samples - 1 ) + deltaTime ) / timer->samples;
        timer->minTime = MIN( timer->minTime, deltaTime );
        timer->maxTime = MAX( timer->maxTime, deltaTime );
    }
    
    // GPU timing
    if ( profileState.gpuTimingEnabled && profileState.queryPool.currentTimestamp < profileState.queryPool.timestampCount ) {
        timer->queryIndex[1] = profileState.queryPool.currentTimestamp++;
        
        VkCommandBuffer cmd = vk.cmd->command_buffer;
        vkCmdWriteTimestamp( cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           profileState.queryPool.timestampPool,
                           timer->queryIndex[1] );
    }
}

/*
================
R_CreateProfileCounter

Create a profile counter
================
*/
profileCounter_t* R_CreateProfileCounter( const char *name, profileCounterType_t type ) {
    if ( profileState.numCounters >= PROFILE_MAX_COUNTERS ) {
        ri.Printf( PRINT_WARNING, "R_CreateProfileCounter: MAX_COUNTERS reached\n" );
        return NULL;
    }
    
    profileCounter_t *counter = &profileState.counters[profileState.numCounters++];
    Q_strncpyz( counter->name, name, sizeof( counter->name ) );
    counter->type = type;
    counter->enabled = qtrue;
    counter->minValue = UINT64_MAX;
    
    return counter;
}

/*
================
R_UpdateProfileCounter

Update a profile counter
================
*/
void R_UpdateProfileCounter( profileCounter_t *counter, uint64_t value ) {
    if ( !counter || !counter->enabled ) {
        return;
    }
    
    counter->value = value;
    counter->totalValue += value;
    counter->samples++;
    
    // Update statistics
    counter->avgValue = counter->totalValue / counter->samples;
    counter->minValue = MIN( counter->minValue, value );
    counter->maxValue = MAX( counter->maxValue, value );
}

/*
================
R_GetCPUTimestamp

Get high-resolution CPU timestamp
================
*/
uint64_t R_GetCPUTimestamp( void ) {
#ifdef _WIN32
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter( &timestamp );
    return timestamp.QuadPart;
#else
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/*
================
R_TimestampToMilliseconds

Convert timestamp difference to milliseconds
================
*/
float R_TimestampToMilliseconds( uint64_t start, uint64_t end ) {
#ifdef _WIN32
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency( &frequency );
    return ( end - start ) * 1000.0f / frequency.QuadPart;
#else
    return ( end - start ) / 1000000.0f;
#endif
}

/*
================
R_CollectGPUTimings

Collect GPU timing results
================
*/
void R_CollectGPUTimings( void ) {
    if ( !profileState.gpuTimingEnabled || profileState.queryPool.currentTimestamp == 0 ) {
        return;
    }
    
    // Get timestamp results
    VkResult result = vkGetQueryPoolResults( vk.device,
                                            profileState.queryPool.timestampPool,
                                            0, profileState.queryPool.currentTimestamp,
                                            sizeof( uint64_t ) * profileState.queryPool.currentTimestamp,
                                            profileState.queryPool.timestampResults,
                                            sizeof( uint64_t ),
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT );
    
    if ( result != VK_SUCCESS ) {
        return;
    }
    
    // Process timer results
    for ( uint32_t i = 0; i < profileState.numTimers; i++ ) {
        profileTimer_t *timer = &profileState.timers[i];
        
        if ( timer->queryIndex[0] < profileState.queryPool.currentTimestamp &&
             timer->queryIndex[1] < profileState.queryPool.currentTimestamp ) {
            uint64_t startTime = profileState.queryPool.timestampResults[timer->queryIndex[0]];
            uint64_t endTime = profileState.queryPool.timestampResults[timer->queryIndex[1]];
            
            // Convert to milliseconds
            float deltaTime = ( endTime - startTime ) * profileState.timestampPeriod / 1000000.0f;
            timer->gpuTime = endTime - startTime;
            
            // Update GPU statistics
            if ( deltaTime > 0 ) {
                timer->samples++;
                timer->avgTime = ( timer->avgTime * ( timer->samples - 1 ) + deltaTime ) / timer->samples;
                timer->minTime = MIN( timer->minTime, deltaTime );
                timer->maxTime = MAX( timer->maxTime, deltaTime );
            }
        }
    }
}

/*
================
R_UpdateFrameStatistics

Update frame statistics
================
*/
static void R_UpdateFrameStatistics( frameProfile_t *frame ) {
    // Calculate frame time statistics
    profileState.totalFrames++;
    
    float frameTime = frame->cpuFrameTime;
    profileState.avgFrameTime = ( profileState.avgFrameTime * ( profileState.totalFrames - 1 ) + frameTime ) / profileState.totalFrames;
    profileState.minFrameTime = MIN( profileState.minFrameTime, frameTime );
    profileState.maxFrameTime = MAX( profileState.maxFrameTime, frameTime );
    
    // Check for dropped frames
    if ( frameTime > profileState.targetFrameTime * 1.5f ) {
        profileState.droppedFrames++;
    }
    
    // Update thread utilization
    R_UpdateThreadUtilization();
}

/*
================
R_GenerateProfileReport

Generate profiling report
================
*/
void R_GenerateProfileReport( void ) {
    if ( !profileState.initialized ) {
        return;
    }
    
    ri.Printf( PRINT_ALL, "=== Performance Profile Report ===\n" );
    ri.Printf( PRINT_ALL, "Frame: %d, Avg: %.2f ms, Min: %.2f ms, Max: %.2f ms\n",
              profileState.frameCount,
              profileState.avgFrameTime,
              profileState.minFrameTime,
              profileState.maxFrameTime );
    ri.Printf( PRINT_ALL, "Dropped Frames: %d (%.1f%%)\n",
              profileState.droppedFrames,
              ( profileState.droppedFrames * 100.0f ) / profileState.totalFrames );
    
    // Print timer hierarchy
    ri.Printf( PRINT_ALL, "\n--- Timer Breakdown ---\n" );
    R_PrintTimerHierarchy( profileState.rootTimer, 0 );
    
    // Print counters
    ri.Printf( PRINT_ALL, "\n--- Performance Counters ---\n" );
    for ( uint32_t i = 0; i < profileState.numCounters; i++ ) {
        profileCounter_t *counter = &profileState.counters[i];
        ri.Printf( PRINT_ALL, "  %s: %llu (avg: %llu, min: %llu, max: %llu)\n",
                  counter->name,
                  counter->value,
                  counter->avgValue,
                  counter->minValue,
                  counter->maxValue );
    }
    
    // Write to file if logging enabled
    if ( profileState.logToFile ) {
        R_WriteProfileReport( "profile_report.txt" );
    }
}

/*
================
R_PrintTimerHierarchy

Print timer hierarchy recursively
================
*/
static void R_PrintTimerHierarchy( profileTimer_t *timer, uint32_t depth ) {
    if ( !timer ) {
        return;
    }
    
    // Indent based on depth
    char indent[64];
    for ( uint32_t i = 0; i < depth * 2 && i < 63; i++ ) {
        indent[i] = ' ';
    }
    indent[depth * 2] = '\0';
    
    // Print timer info
    ri.Printf( PRINT_ALL, "%s%s: CPU: %.2f ms, GPU: %.2f ms (avg: %.2f, min: %.2f, max: %.2f)\n",
              indent,
              timer->name,
              timer->cpuTime * profileState.timestampPeriod / 1000000.0f,
              timer->gpuTime * profileState.timestampPeriod / 1000000.0f,
              timer->avgTime,
              timer->minTime,
              timer->maxTime );
    
    // Print children
    profileTimer_t *child = timer->children;
    while ( child ) {
        R_PrintTimerHierarchy( child, depth + 1 );
        child = child->next;
    }
}

/*
================
R_DrawProfileOverlay

Draw profiling overlay on screen
================
*/
void R_DrawProfileOverlay( void ) {
    if ( !profileState.showOverlay ) {
        return;
    }
    
    // Draw frame time graph
    R_DrawFrameTimeGraph( 10, 10, 400, 200 );
    
    // Draw timer bars
    R_DrawTimerBars( 10, 220 );
    
    // Draw counter values
    R_DrawCounterValues( 10, 400 );
}

/*
================
R_SetProfilingEnabled

Enable/disable profiling
================
*/
void R_SetProfilingEnabled( qboolean enable ) {
    profileState.enabled = enable;
    
    if ( enable && !profileState.initialized ) {
        R_InitProfiling();
    }
}