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

#ifndef TR_PERFORMANCE_H
#define TR_PERFORMANCE_H

#include "../core/tr_local.h"

/*
================================================================================
Phase 10: Performance Monitoring System

Comprehensive performance tracking and optimization.
================================================================================
*/

// Performance counter types
typedef enum {
    PERF_FRAME_TIME,
    PERF_CPU_TIME,
    PERF_GPU_TIME,
    PERF_LIGHT_CULL,
    PERF_SHADOW_GEN,
    PERF_INTERACTION_BUILD,
    PERF_DRAW_CALLS,
    PERF_STATE_CHANGES,
    PERF_VERTICES,
    PERF_TRIANGLES,
    PERF_MEMORY_ALLOCS,
    PERF_CACHE_HITS,
    PERF_CACHE_MISSES,
    MAX_PERF_COUNTERS
} perfCounter_t;

// Performance sample
typedef struct perfSample_s {
    float       value;
    int         frameNum;
} perfSample_t;

// Performance metric
typedef struct perfMetric_s {
    char        name[64];
    perfCounter_t type;
    
    // Current frame
    float       current;
    int         count;
    
    // Statistics
    float       min;
    float       max;
    float       average;
    float       total;
    
    // History
    perfSample_t samples[128];
    int         sampleIndex;
    int         numSamples;
} perfMetric_t;

// Performance state
typedef struct perfState_s {
    qboolean    enabled;
    int         frameCount;
    
    // Metrics
    perfMetric_t metrics[MAX_PERF_COUNTERS];
    
    // Timers
    int         timerStack[16];
    int         timerDepth;
    int         frameStartTime;
    int         frameEndTime;
    
    // Memory tracking
    size_t      frameMemoryUsed;
    size_t      peakMemoryUsed;
    int         allocCount;
    
    // GPU queries (if supported)
    qboolean    gpuTimingAvailable;
    void        *gpuQueries[16];
    int         currentGPUQuery;
} perfState_t;

// Global performance state
extern perfState_t perf;

// Function declarations

// System management
void R_InitPerformance(void);
void R_ShutdownPerformance(void);
void R_BeginFrame(void);
void R_EndFrame(void);

// Timer management
void R_PushTimer(perfCounter_t counter);
void R_PopTimer(void);
int R_GetTimeMicroseconds(void);

// Counter updates
void R_IncCounter(perfCounter_t counter);
void R_AddCounter(perfCounter_t counter, float value);
void R_SetCounter(perfCounter_t counter, float value);

// GPU timing
void R_BeginGPUTimer(const char *name);
void R_EndGPUTimer(void);

// Reporting
void R_PerformanceReport(void);
void R_PerformanceGraph(void);
void R_PerformanceCSV(const char *filename);

// Optimization hints
void R_AnalyzeBottlenecks(void);
void R_SuggestOptimizations(void);

// Macros for easy timing
#define PERF_BEGIN(counter) if (r_speeds->integer) R_PushTimer(counter)
#define PERF_END() if (r_speeds->integer) R_PopTimer()
#define PERF_INC(counter) if (r_speeds->integer) R_IncCounter(counter)
#define PERF_ADD(counter, val) if (r_speeds->integer) R_AddCounter(counter, val)

#endif // TR_PERFORMANCE_H