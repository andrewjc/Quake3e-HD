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
// tr_profile.h - GPU Profiling System

#ifndef __TR_PROFILE_H
#define __TR_PROFILE_H

#include "../tr_local.h"
#include "vulkan/vulkan.h"

// Profiling configuration
#define PROFILE_MAX_TIMERS          256
#define PROFILE_MAX_COUNTERS        128
#define PROFILE_MAX_SAMPLES         1024
#define PROFILE_HISTORY_FRAMES      120
#define PROFILE_MAX_THREADS         8
#define PROFILE_MAX_NAME_LENGTH     64

// Profile timer types
typedef enum {
    PROFILE_TIMER_FRAME = 0,
    PROFILE_TIMER_SHADOW,
    PROFILE_TIMER_GBUFFER,
    PROFILE_TIMER_LIGHTING,
    PROFILE_TIMER_POSTPROCESS,
    PROFILE_TIMER_UI,
    PROFILE_TIMER_PRESENT,
    PROFILE_TIMER_COMPUTE,
    PROFILE_TIMER_TRANSFER,
    PROFILE_TIMER_CUSTOM,
} profileTimerType_t;

// Profile counter types
typedef enum {
    PROFILE_COUNTER_DRAWCALLS = 0,
    PROFILE_COUNTER_TRIANGLES,
    PROFILE_COUNTER_VERTICES,
    PROFILE_COUNTER_PIPELINES,
    PROFILE_COUNTER_DESCRIPTORS,
    PROFILE_COUNTER_BUFFERS,
    PROFILE_COUNTER_TEXTURES,
    PROFILE_COUNTER_SHADERS,
    PROFILE_COUNTER_CUSTOM,
} profileCounterType_t;

// GPU timer structure
typedef struct profileTimer_s {
    char                    name[PROFILE_MAX_NAME_LENGTH];
    profileTimerType_t      type;
    uint32_t                queryIndex[2];     // Start and end queries
    uint64_t                gpuTime;           // GPU time in nanoseconds
    uint64_t                cpuTime;           // CPU time in nanoseconds
    float                   avgTime;           // Moving average
    float                   minTime;           // Minimum time
    float                   maxTime;           // Maximum time
    uint32_t                samples;           // Number of samples
    uint32_t                depth;             // Hierarchy depth
    struct profileTimer_s  *parent;            // Parent timer
    struct profileTimer_s  *children;          // Child timers
    struct profileTimer_s  *next;              // Next sibling
    qboolean                active;            // Currently timing
    qboolean                enabled;           // Timer enabled
} profileTimer_t;

// Performance counter structure
typedef struct profileCounter_s {
    char                    name[PROFILE_MAX_NAME_LENGTH];
    profileCounterType_t    type;
    uint64_t                value;             // Current value
    uint64_t                avgValue;          // Moving average
    uint64_t                minValue;          // Minimum value
    uint64_t                maxValue;          // Maximum value
    uint64_t                totalValue;        // Total accumulated
    uint32_t                samples;           // Number of samples
    qboolean                enabled;           // Counter enabled
} profileCounter_t;

// Frame timing data
typedef struct frameProfile_s {
    uint64_t                frameNumber;
    uint64_t                cpuStartTime;
    uint64_t                cpuEndTime;
    uint64_t                gpuStartTime;
    uint64_t                gpuEndTime;
    float                   cpuFrameTime;      // Total CPU frame time (ms)
    float                   gpuFrameTime;      // Total GPU frame time (ms)
    float                   presentTime;       // Present/swap time (ms)
    uint32_t                drawCalls;
    uint32_t                triangles;
    uint32_t                vertices;
} frameProfile_t;

// Thread profiling data
typedef struct threadProfile_s {
    uint32_t                threadId;
    char                    threadName[PROFILE_MAX_NAME_LENGTH];
    uint64_t                totalTime;
    uint64_t                idleTime;
    float                   utilization;
    profileTimer_t         *timers;
    uint32_t                numTimers;
} threadProfile_t;

// GPU query pool
typedef struct profileQueryPool_s {
    VkQueryPool             timestampPool;
    VkQueryPool             statisticsPool;
    VkQueryPool             occlusionPool;
    uint32_t                timestampCount;
    uint32_t                statisticsCount;
    uint32_t                occlusionCount;
    uint32_t                currentTimestamp;
    uint32_t                currentStatistics;
    uint64_t               *timestampResults;
    VkQueryPipelineStatisticFlags statisticsFlags;
} profileQueryPool_t;

// Profiling state
typedef struct profileState_s {
    qboolean                initialized;
    qboolean                enabled;
    qboolean                gpuTimingEnabled;
    qboolean                cpuTimingEnabled;
    qboolean                autoReport;
    
    // Timers
    profileTimer_t          timers[PROFILE_MAX_TIMERS];
    uint32_t                numTimers;
    profileTimer_t         *currentTimer;
    profileTimer_t         *rootTimer;
    
    // Counters
    profileCounter_t        counters[PROFILE_MAX_COUNTERS];
    uint32_t                numCounters;
    
    // Frame history
    frameProfile_t          frameHistory[PROFILE_HISTORY_FRAMES];
    uint32_t                currentFrame;
    uint32_t                frameCount;
    
    // Thread profiling
    threadProfile_t         threads[PROFILE_MAX_THREADS];
    uint32_t                numThreads;
    uint32_t                mainThreadId;
    
    // GPU queries
    profileQueryPool_t      queryPool;
    float                   timestampPeriod;
    qboolean                queryPoolsReset;  // Track if pools have been reset
    
    // Statistics
    float                   avgFrameTime;
    float                   minFrameTime;
    float                   maxFrameTime;
    float                   targetFrameTime;
    uint32_t                droppedFrames;
    uint32_t                totalFrames;
    
    // Reporting
    qboolean                showOverlay;
    qboolean                logToFile;
    fileHandle_t            logFile;
    uint32_t                reportInterval;
    uint32_t                lastReportFrame;
} profileState_t;

// Global profiling state
extern profileState_t profileState;

// Initialization
qboolean R_InitProfiling( void );
void R_ShutdownProfiling( void );

// Frame management
void R_BeginFrameProfiling( void );
void R_EndFrameProfiling( void );
void R_ResetFrameStats( void );

// Timer management
profileTimer_t* R_CreateProfileTimer( const char *name, profileTimerType_t type );
void R_DestroyProfileTimer( profileTimer_t *timer );
void R_StartProfileTimer( profileTimer_t *timer );
void R_EndProfileTimer( profileTimer_t *timer );
void R_PushProfileTimer( const char *name );
void R_PopProfileTimer( void );

// Counter management
profileCounter_t* R_CreateProfileCounter( const char *name, profileCounterType_t type );
void R_DestroyProfileCounter( profileCounter_t *counter );
void R_UpdateProfileCounter( profileCounter_t *counter, uint64_t value );
void R_IncrementProfileCounter( profileCounter_t *counter, uint64_t delta );
void R_ResetProfileCounter( profileCounter_t *counter );

// GPU timing
void R_BeginGPUTimer( VkCommandBuffer cmd, const char *name );
void R_EndGPUTimer( VkCommandBuffer cmd );
void R_InsertGPUTimestamp( VkCommandBuffer cmd, const char *label );
void R_CollectGPUTimings( void );

// CPU timing
void R_BeginCPUTimer( const char *name );
void R_EndCPUTimer( void );
uint64_t R_GetCPUTimestamp( void );
float R_TimestampToMilliseconds( uint64_t start, uint64_t end );

// Thread profiling
void R_RegisterProfileThread( const char *name );
void R_UnregisterProfileThread( void );
void R_UpdateThreadUtilization( void );

// Statistics queries
void R_BeginStatisticsQuery( VkCommandBuffer cmd );
void R_EndStatisticsQuery( VkCommandBuffer cmd );
void R_CollectStatistics( void );

// Reporting
void R_GenerateProfileReport( void );
void R_PrintProfileReport( void );
void R_WriteProfileReport( const char *filename );
void R_DrawProfileOverlay( void );

// Analysis
float R_GetAverageFrameTime( void );
float R_GetWorstFrameTime( void );
float R_GetFrameTimeVariance( void );
uint32_t R_GetDroppedFrames( void );
void R_AnalyzePerformanceBottlenecks( void );

// Helpers
const char* R_GetTimerName( profileTimerType_t type );
const char* R_GetCounterName( profileCounterType_t type );
void R_SortTimersByTime( profileTimer_t **timers, uint32_t count );
void R_CalculateTimerPercentages( void );

// Macros for easy profiling
#define PROFILE_BEGIN(name) R_BeginCPUTimer(name)
#define PROFILE_END() R_EndCPUTimer()
#define PROFILE_GPU_BEGIN(cmd, name) R_BeginGPUTimer(cmd, name)
#define PROFILE_GPU_END(cmd) R_EndGPUTimer(cmd)
#define PROFILE_COUNTER(name, value) R_UpdateProfileCounter(R_CreateProfileCounter(name, PROFILE_COUNTER_CUSTOM), value)

// Configuration
void R_SetProfilingEnabled( qboolean enable );
void R_SetGPUTimingEnabled( qboolean enable );
void R_SetCPUTimingEnabled( qboolean enable );
void R_SetProfileOverlay( qboolean show );
void R_SetProfileLogging( qboolean enable );

#endif // __TR_PROFILE_H