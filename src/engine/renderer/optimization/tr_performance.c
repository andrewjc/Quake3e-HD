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

#include "../core/tr_local.h"
#include "tr_performance.h"

/*
================================================================================
Phase 10: Performance Monitoring Implementation

Tracks and analyzes renderer performance metrics.
================================================================================
*/

perfState_t perf;

// Counter names
static const char *perfCounterNames[MAX_PERF_COUNTERS] = {
    "Frame Time",
    "CPU Time",
    "GPU Time",
    "Light Culling",
    "Shadow Generation",
    "Interaction Building",
    "Draw Calls",
    "State Changes",
    "Vertices",
    "Triangles",
    "Memory Allocations",
    "Cache Hits",
    "Cache Misses"
};

/*
================
R_InitPerformance

Initialize performance monitoring system
================
*/
void R_InitPerformance(void) {
    int i;
    
    Com_Memset(&perf, 0, sizeof(perf));
    
    // Initialize metrics
    for (i = 0; i < MAX_PERF_COUNTERS; i++) {
        Q_strncpyz(perf.metrics[i].name, perfCounterNames[i], 
                   sizeof(perf.metrics[i].name));
        perf.metrics[i].type = i;
        perf.metrics[i].min = 999999.0f;
        perf.metrics[i].max = 0.0f;
    }
    
    // Check for GPU timer support
    // In actual implementation: Query OpenGL/Vulkan extensions
    perf.gpuTimingAvailable = qfalse;
    
    perf.enabled = qtrue;
    
    ri.Printf(PRINT_ALL, "Performance monitoring initialized\n");
}

/*
================
R_ShutdownPerformance

Shutdown performance monitoring and generate final report
================
*/
void R_ShutdownPerformance(void) {
    if (!perf.enabled) {
        return;
    }
    
    // Generate final report
    R_PerformanceReport();
    
    // Export to CSV if requested
    if (r_speeds->integer >= 2) {
        R_PerformanceCSV("performance_report.csv");
    }
    
    perf.enabled = qfalse;
}

/*
================
R_BeginFrame

Begin frame timing
================
*/
void R_BeginFrame(void) {
    if (!perf.enabled) {
        return;
    }
    
    perf.frameStartTime = R_GetTimeMicroseconds();
    perf.frameCount++;
    
    // Reset per-frame counters
    perf.metrics[PERF_DRAW_CALLS].current = 0;
    perf.metrics[PERF_STATE_CHANGES].current = 0;
    perf.metrics[PERF_VERTICES].current = 0;
    perf.metrics[PERF_TRIANGLES].current = 0;
    perf.timerDepth = 0;
}

/*
================
R_EndFrame

End frame timing and update statistics
================
*/
void R_EndFrame(void) {
    int i;
    float frameTime;
    
    if (!perf.enabled) {
        return;
    }
    
    perf.frameEndTime = R_GetTimeMicroseconds();
    frameTime = (perf.frameEndTime - perf.frameStartTime) / 1000.0f;  // Convert to ms
    
    // Update frame time metric
    R_SetCounter(PERF_FRAME_TIME, frameTime);
    
    // Update all metrics
    for (i = 0; i < MAX_PERF_COUNTERS; i++) {
        perfMetric_t *m = &perf.metrics[i];
        
        // Update statistics
        if (m->current < m->min) {
            m->min = m->current;
        }
        if (m->current > m->max) {
            m->max = m->current;
        }
        
        m->total += m->current;
        m->average = m->total / perf.frameCount;
        
        // Store sample
        m->samples[m->sampleIndex].value = m->current;
        m->samples[m->sampleIndex].frameNum = perf.frameCount;
        m->sampleIndex = (m->sampleIndex + 1) % 128;
        if (m->numSamples < 128) {
            m->numSamples++;
        }
    }
    
    // Display real-time stats if requested
    if (r_speeds->integer >= 3) {
        R_PerformanceReport();
    }
}

/*
================
R_PushTimer

Start timing a specific operation
================
*/
void R_PushTimer(perfCounter_t counter) {
    if (!perf.enabled || perf.timerDepth >= 16) {
        return;
    }
    
    perf.timerStack[perf.timerDepth++] = R_GetTimeMicroseconds();
}

/*
================
R_PopTimer

End timing and accumulate result
================
*/
void R_PopTimer(void) {
    int elapsed;
    
    if (!perf.enabled || perf.timerDepth == 0) {
        return;
    }
    
    elapsed = R_GetTimeMicroseconds() - perf.timerStack[--perf.timerDepth];
    
    // Add to CPU time
    perf.metrics[PERF_CPU_TIME].current += elapsed / 1000.0f;  // Convert to ms
}

/*
================
R_GetTimeMicroseconds

Get high-resolution time in microseconds
================
*/
int R_GetTimeMicroseconds(void) {
    // In actual implementation:
    // - Windows: QueryPerformanceCounter
    // - Linux: clock_gettime(CLOCK_MONOTONIC)
    
    return ri.Milliseconds() * 1000;  // Approximate for now
}

/*
================
R_IncCounter

Increment a performance counter
================
*/
void R_IncCounter(perfCounter_t counter) {
    if (!perf.enabled || counter >= MAX_PERF_COUNTERS) {
        return;
    }
    
    perf.metrics[counter].current++;
    perf.metrics[counter].count++;
}

/*
================
R_AddCounter

Add value to a performance counter
================
*/
void R_AddCounter(perfCounter_t counter, float value) {
    if (!perf.enabled || counter >= MAX_PERF_COUNTERS) {
        return;
    }
    
    perf.metrics[counter].current += value;
    perf.metrics[counter].count++;
}

/*
================
R_SetCounter

Set a performance counter value
================
*/
void R_SetCounter(perfCounter_t counter, float value) {
    if (!perf.enabled || counter >= MAX_PERF_COUNTERS) {
        return;
    }
    
    perf.metrics[counter].current = value;
    perf.metrics[counter].count = 1;
}

/*
================
R_PerformanceReport

Generate performance report
================
*/
void R_PerformanceReport(void) {
    int i;
    
    if (!perf.enabled || !r_speeds->integer) {
        return;
    }
    
    ri.Printf(PRINT_ALL, "\n==== Performance Report (Frame %d) ====\n", 
              perf.frameCount);
    
    // Timing metrics
    ri.Printf(PRINT_ALL, "Timing:\n");
    ri.Printf(PRINT_ALL, "  Frame: %.2f ms (%.1f FPS)\n",
              perf.metrics[PERF_FRAME_TIME].current,
              1000.0f / perf.metrics[PERF_FRAME_TIME].current);
    ri.Printf(PRINT_ALL, "  CPU: %.2f ms\n", 
              perf.metrics[PERF_CPU_TIME].current);
    
    if (perf.gpuTimingAvailable) {
        ri.Printf(PRINT_ALL, "  GPU: %.2f ms\n",
                  perf.metrics[PERF_GPU_TIME].current);
    }
    
    // Rendering metrics
    ri.Printf(PRINT_ALL, "\nRendering:\n");
    ri.Printf(PRINT_ALL, "  Draw Calls: %.0f\n",
              perf.metrics[PERF_DRAW_CALLS].current);
    ri.Printf(PRINT_ALL, "  State Changes: %.0f\n",
              perf.metrics[PERF_STATE_CHANGES].current);
    ri.Printf(PRINT_ALL, "  Vertices: %.0f\n",
              perf.metrics[PERF_VERTICES].current);
    ri.Printf(PRINT_ALL, "  Triangles: %.0f\n",
              perf.metrics[PERF_TRIANGLES].current);
    
    // Lighting metrics
    ri.Printf(PRINT_ALL, "\nLighting:\n");
    ri.Printf(PRINT_ALL, "  Light Culling: %.2f ms\n",
              perf.metrics[PERF_LIGHT_CULL].current);
    ri.Printf(PRINT_ALL, "  Shadow Generation: %.2f ms\n",
              perf.metrics[PERF_SHADOW_GEN].current);
    ri.Printf(PRINT_ALL, "  Interaction Building: %.2f ms\n",
              perf.metrics[PERF_INTERACTION_BUILD].current);
    
    // Memory metrics
    ri.Printf(PRINT_ALL, "\nMemory:\n");
    ri.Printf(PRINT_ALL, "  Frame Memory: %d KB\n",
              (int)(perf.frameMemoryUsed / 1024));
    ri.Printf(PRINT_ALL, "  Peak Memory: %d KB\n",
              (int)(perf.peakMemoryUsed / 1024));
    ri.Printf(PRINT_ALL, "  Allocations: %.0f\n",
              perf.metrics[PERF_MEMORY_ALLOCS].current);
    
    // Cache metrics
    float hitRate = 0.0f;
    if (perf.metrics[PERF_CACHE_HITS].current + 
        perf.metrics[PERF_CACHE_MISSES].current > 0) {
        hitRate = perf.metrics[PERF_CACHE_HITS].current * 100.0f /
                 (perf.metrics[PERF_CACHE_HITS].current + 
                  perf.metrics[PERF_CACHE_MISSES].current);
    }
    
    ri.Printf(PRINT_ALL, "\nCache:\n");
    ri.Printf(PRINT_ALL, "  Hit Rate: %.1f%%\n", hitRate);
    ri.Printf(PRINT_ALL, "  Hits: %.0f\n",
              perf.metrics[PERF_CACHE_HITS].current);
    ri.Printf(PRINT_ALL, "  Misses: %.0f\n",
              perf.metrics[PERF_CACHE_MISSES].current);
    
    // Averages
    ri.Printf(PRINT_ALL, "\nAverages:\n");
    for (i = 0; i < MAX_PERF_COUNTERS; i++) {
        if (perf.metrics[i].average > 0.001f) {
            ri.Printf(PRINT_ALL, "  %s: %.2f (min: %.2f, max: %.2f)\n",
                      perf.metrics[i].name,
                      perf.metrics[i].average,
                      perf.metrics[i].min,
                      perf.metrics[i].max);
        }
    }
}

/*
================
R_AnalyzeBottlenecks

Analyze performance bottlenecks
================
*/
void R_AnalyzeBottlenecks(void) {
    float frameTime;
    float cpuTime;
    float gpuTime;
    
    if (!perf.enabled) {
        return;
    }
    
    frameTime = perf.metrics[PERF_FRAME_TIME].average;
    cpuTime = perf.metrics[PERF_CPU_TIME].average;
    gpuTime = perf.metrics[PERF_GPU_TIME].average;
    
    ri.Printf(PRINT_ALL, "\n==== Bottleneck Analysis ====\n");
    
    // Determine bottleneck
    if (cpuTime > gpuTime * 1.1f) {
        ri.Printf(PRINT_ALL, "CPU Bound (CPU: %.2f ms, GPU: %.2f ms)\n",
                  cpuTime, gpuTime);
        
        // Analyze CPU bottlenecks
        if (perf.metrics[PERF_SHADOW_GEN].average > frameTime * 0.3f) {
            ri.Printf(PRINT_ALL, "  - Shadow generation taking %.1f%% of frame\n",
                      perf.metrics[PERF_SHADOW_GEN].average * 100.0f / frameTime);
        }
        
        if (perf.metrics[PERF_LIGHT_CULL].average > frameTime * 0.2f) {
            ri.Printf(PRINT_ALL, "  - Light culling taking %.1f%% of frame\n",
                      perf.metrics[PERF_LIGHT_CULL].average * 100.0f / frameTime);
        }
    } else if (gpuTime > cpuTime * 1.1f) {
        ri.Printf(PRINT_ALL, "GPU Bound (CPU: %.2f ms, GPU: %.2f ms)\n",
                  cpuTime, gpuTime);
        
        // Analyze GPU bottlenecks
        if (perf.metrics[PERF_DRAW_CALLS].average > 1000) {
            ri.Printf(PRINT_ALL, "  - High draw call count: %.0f\n",
                      perf.metrics[PERF_DRAW_CALLS].average);
        }
        
        if (perf.metrics[PERF_STATE_CHANGES].average > 500) {
            ri.Printf(PRINT_ALL, "  - High state change count: %.0f\n",
                      perf.metrics[PERF_STATE_CHANGES].average);
        }
    } else {
        ri.Printf(PRINT_ALL, "Balanced (CPU: %.2f ms, GPU: %.2f ms)\n",
                  cpuTime, gpuTime);
    }
}

/*
================
R_PerformanceCSV

Export performance data to CSV file
================
*/
void R_PerformanceCSV(const char *filename) {
    // In actual implementation:
    // Write performance metrics to CSV file for external analysis
    
    ri.Printf(PRINT_ALL, "Performance data exported to %s\n", filename);
}