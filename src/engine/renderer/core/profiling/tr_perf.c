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
// tr_perf.c - Performance Monitoring System Implementation

#include "tr_perf.h"
#include "../tr_local.h"
#include "../tr_common_utils.h"
#include <math.h>
#include <float.h>

// Global performance state
perfState_t perfState;

// Monitor type names
static const char *monitorTypeNames[] = {
    "FPS",
    "Frame Time",
    "CPU Usage",
    "GPU Usage",
    "Memory",
    "Bandwidth",
    "Latency",
    "Stalls",
    "Cache",
    "Thermal",
    "Power",
    "Custom"
};

// Event type names
static const char *eventTypeNames[] = {
    "Frame Drop",
    "Stutter",
    "Hitch",
    "Spike",
    "Throttle",
    "Memory Pressure",
    "GPU Stall",
    "CPU Stall",
    "Pipeline Flush",
    "Cache Thrash",
    "Thermal Limit",
    "Power Limit",
    "Custom"
};

// Severity names
static const char *severityNames[] = {
    "Info",
    "Warning",
    "Error",
    "Critical"
};

// Static functions
static void R_UpdateFPSMonitor( perfMonitor_t *monitor );
static void R_UpdateFrameTimeMonitor( perfMonitor_t *monitor );
static void R_UpdateCPUMonitor( perfMonitor_t *monitor );
static void R_UpdateGPUMonitor( perfMonitor_t *monitor );
static void R_AnalyzeFrameTiming( void );
static void R_PredictPerformance( void );
static void R_AnalyzePerfBottlenecks( void );
static perfEvent_t* R_AllocPerfEvent( void );
static void R_FreePerfEvent( perfEvent_t *event );

/*
===============
R_InitPerformanceMonitoring

Initialize the performance monitoring system
===============
*/
qboolean R_InitPerformanceMonitoring( void ) {
    ri.Printf( PRINT_ALL, "Initializing performance monitoring system...\n" );
    
    Com_Memset( &perfState, 0, sizeof( perfState ) );
    
    // Allocate frame history
    perfState.frameHistorySize = PERF_HISTORY_FRAMES;
    perfState.frameHistory = ri.Malloc( sizeof( framePerfData_t ) * perfState.frameHistorySize );
    Com_Memset( perfState.frameHistory, 0, sizeof( framePerfData_t ) * perfState.frameHistorySize );
    
    // Allocate event pool
    perfState.maxEvents = PERF_MAX_EVENTS;
    perfState.eventPool = ri.Malloc( sizeof( perfEvent_t ) * perfState.maxEvents );
    Com_Memset( perfState.eventPool, 0, sizeof( perfEvent_t ) * perfState.maxEvents );
    
    // Link event pool
    for ( uint32_t i = 0; i < perfState.maxEvents - 1; i++ ) {
        perfState.eventPool[i].next = &perfState.eventPool[i + 1];
    }
    perfState.events = NULL;
    
    // Mark as initialized early so R_CreatePerfMonitor will work
    perfState.initialized = qtrue;
    
    // Create default monitors
    perfMonitor_t *fpsMonitor = R_CreatePerfMonitor( "FPS", PERF_MONITOR_FPS );
    if ( fpsMonitor ) {
        fpsMonitor->updateCallback = R_UpdateFPSMonitor;
        R_SetMonitorThresholds( fpsMonitor, 60.0f, 45.0f, 30.0f );
    }
    
    perfMonitor_t *frameTimeMonitor = R_CreatePerfMonitor( "Frame Time", PERF_MONITOR_FRAMETIME );
    if ( frameTimeMonitor ) {
        frameTimeMonitor->updateCallback = R_UpdateFrameTimeMonitor;
        R_SetMonitorThresholds( frameTimeMonitor, 16.67f, 22.0f, 33.0f );
    }
    
    perfMonitor_t *cpuMonitor = R_CreatePerfMonitor( "CPU Usage", PERF_MONITOR_CPU_USAGE );
    if ( cpuMonitor ) {
        cpuMonitor->updateCallback = R_UpdateCPUMonitor;
        R_SetMonitorThresholds( cpuMonitor, 60.0f, 80.0f, 95.0f );
    }
    
    perfMonitor_t *gpuMonitor = R_CreatePerfMonitor( "GPU Usage", PERF_MONITOR_GPU_USAGE );
    if ( gpuMonitor ) {
        gpuMonitor->updateCallback = R_UpdateGPUMonitor;
        R_SetMonitorThresholds( gpuMonitor, 70.0f, 85.0f, 95.0f );
    }
    
    // Set default target performance
    perfState.targetFPS = 60.0f;
    perfState.targetFrameTime = 1000.0f / 60.0f;
    perfState.acceptableVariance = 2.0f;  // +/- 2ms
    
    // Enable monitoring
    perfState.monitoring = qtrue;
    perfState.alertsEnabled = qtrue;
    perfState.alertLevel = PERF_SEVERITY_WARNING;
    
    // Display settings
    VectorSet2( perfState.displayPosition, 10, 200 );
    perfState.displayScale = 1.0f;
    
    ri.Printf( PRINT_ALL, "Performance monitoring system initialized\n" );
    return qtrue;
}

/*
===============
R_ShutdownPerformanceMonitoring

Shutdown the performance monitoring system
===============
*/
void R_ShutdownPerformanceMonitoring( void ) {
    if ( !perfState.initialized ) {
        return;
    }
    
    // Stop recording if active
    if ( perfState.recording ) {
        R_StopPerfRecording();
    }
    
    // Free monitor samples
    for ( uint32_t i = 0; i < perfState.monitorCount; i++ ) {
        perfMonitor_t *monitor = &perfState.monitors[i];
        if ( monitor->samples ) {
            ri.Free( monitor->samples );
        }
    }
    
    // Free frame history
    if ( perfState.frameHistory ) {
        ri.Free( perfState.frameHistory );
    }
    
    // Free event pool
    if ( perfState.eventPool ) {
        ri.Free( perfState.eventPool );
    }
    
    Com_Memset( &perfState, 0, sizeof( perfState ) );
    ri.Printf( PRINT_ALL, "Performance monitoring system shutdown\n" );
}

/*
===============
R_ResetPerformanceMonitoring

Reset performance monitoring data
===============
*/
void R_ResetPerformanceMonitoring( void ) {
    if ( !perfState.initialized ) {
        return;
    }
    
    // Reset monitors
    for ( uint32_t i = 0; i < perfState.monitorCount; i++ ) {
        perfMonitor_t *monitor = &perfState.monitors[i];
        monitor->current = 0;
        monitor->average = 0;
        monitor->minimum = FLT_MAX;
        monitor->maximum = -FLT_MAX;
        monitor->variance = 0;
        monitor->stddev = 0;
        monitor->sampleCount = 0;
        monitor->sampleIndex = 0;
        monitor->smoothedValue = 0;
        monitor->predictedValue = 0;
        monitor->currentSeverity = PERF_SEVERITY_INFO;
        
        if ( monitor->samples ) {
            Com_Memset( monitor->samples, 0, sizeof( float ) * monitor->maxSamples );
        }
    }
    
    // Clear events
    R_ClearPerfEvents();
    
    // Reset frame history
    Com_Memset( perfState.frameHistory, 0, sizeof( framePerfData_t ) * perfState.frameHistorySize );
    perfState.currentFrameIndex = 0;
    
    // Reset analysis
    Com_Memset( &perfState.currentBottleneck, 0, sizeof( perfBottleneck_t ) );
    Com_Memset( &perfState.prediction, 0, sizeof( perfPrediction_t ) );
}

/*
===============
R_CreatePerfMonitor

Create a new performance monitor
===============
*/
perfMonitor_t* R_CreatePerfMonitor( const char *name, perfMonitorType_t type ) {
    if ( !perfState.initialized || perfState.monitorCount >= PERF_MAX_MONITORS ) {
        return NULL;
    }
    
    perfMonitor_t *monitor = &perfState.monitors[perfState.monitorCount++];
    Com_Memset( monitor, 0, sizeof( perfMonitor_t ) );
    
    Q_strncpyz( monitor->name, name, PERF_MAX_NAME_LENGTH );
    monitor->type = type;
    monitor->enabled = qtrue;
    monitor->autoScale = qtrue;
    
    // Allocate sample buffer
    monitor->maxSamples = PERF_MAX_SAMPLES;
    monitor->samples = ri.Malloc( sizeof( float ) * monitor->maxSamples );
    Com_Memset( monitor->samples, 0, sizeof( float ) * monitor->maxSamples );
    
    // Set default smoothing
    monitor->smoothingFactor = 0.1f;  // 10% new, 90% old
    
    // Initialize min/max
    monitor->minimum = FLT_MAX;
    monitor->maximum = -FLT_MAX;
    
    return monitor;
}

/*
===============
R_UpdatePerfMonitor

Update a performance monitor with a new value
===============
*/
void R_UpdatePerfMonitor( perfMonitor_t *monitor, float value ) {
    if ( !monitor || !monitor->enabled ) {
        return;
    }
    
    monitor->current = value;
    
    // Add to samples
    monitor->samples[monitor->sampleIndex] = value;
    monitor->sampleIndex = ( monitor->sampleIndex + 1 ) % monitor->maxSamples;
    if ( monitor->sampleCount < monitor->maxSamples ) {
        monitor->sampleCount++;
    }
    
    // Update min/max
    if ( value < monitor->minimum ) {
        monitor->minimum = value;
    }
    if ( value > monitor->maximum ) {
        monitor->maximum = value;
    }
    
    // Calculate average
    float sum = 0;
    for ( uint32_t i = 0; i < monitor->sampleCount; i++ ) {
        sum += monitor->samples[i];
    }
    monitor->average = sum / monitor->sampleCount;
    
    // Calculate variance and standard deviation
    float sumSquaredDiff = 0;
    for ( uint32_t i = 0; i < monitor->sampleCount; i++ ) {
        float diff = monitor->samples[i] - monitor->average;
        sumSquaredDiff += diff * diff;
    }
    monitor->variance = sumSquaredDiff / monitor->sampleCount;
    monitor->stddev = sqrtf( monitor->variance );
    
    // Update smoothed value
    monitor->smoothedValue = monitor->smoothedValue * ( 1.0f - monitor->smoothingFactor ) + 
                             value * monitor->smoothingFactor;
    
    // Simple linear prediction
    if ( monitor->sampleCount >= 2 ) {
        uint32_t prevIndex = ( monitor->sampleIndex + monitor->maxSamples - 2 ) % monitor->maxSamples;
        float delta = value - monitor->samples[prevIndex];
        monitor->predictedValue = value + delta;
    }
    
    // Check thresholds
    if ( monitor->warningThreshold > 0 || monitor->criticalThreshold > 0 ) {
        perfSeverity_t oldSeverity = monitor->currentSeverity;
        
        if ( monitor->type == PERF_MONITOR_FPS ) {
            // For FPS, lower is worse
            if ( value <= monitor->criticalThreshold ) {
                monitor->currentSeverity = PERF_SEVERITY_CRITICAL;
            } else if ( value <= monitor->warningThreshold ) {
                monitor->currentSeverity = PERF_SEVERITY_WARNING;
            } else {
                monitor->currentSeverity = PERF_SEVERITY_INFO;
            }
        } else {
            // For most metrics, higher is worse
            if ( value >= monitor->criticalThreshold ) {
                monitor->currentSeverity = PERF_SEVERITY_CRITICAL;
            } else if ( value >= monitor->warningThreshold ) {
                monitor->currentSeverity = PERF_SEVERITY_WARNING;
            } else {
                monitor->currentSeverity = PERF_SEVERITY_INFO;
            }
        }
        
        // Trigger alert if severity increased
        if ( monitor->currentSeverity > oldSeverity && monitor->alertCallback ) {
            monitor->alertCallback( monitor, monitor->currentSeverity );
        }
    }
    
    // Call update callback
    if ( monitor->updateCallback ) {
        monitor->updateCallback( monitor );
    }
}

/*
===============
R_SetMonitorThresholds

Set performance thresholds for a monitor
===============
*/
void R_SetMonitorThresholds( perfMonitor_t *monitor, float target, float warning, float critical ) {
    if ( !monitor ) {
        return;
    }
    
    monitor->targetValue = target;
    monitor->warningThreshold = warning;
    monitor->criticalThreshold = critical;
}

/*
===============
R_BeginFrameMonitoring

Begin monitoring a new frame
===============
*/
void R_BeginFrameMonitoring( void ) {
    if ( !perfState.initialized || !perfState.monitoring ) {
        return;
    }
    
    framePerfData_t *frame = &perfState.frameHistory[perfState.currentFrameIndex];
    
    // Save previous frame
    if ( perfState.currentFrameIndex > 0 ) {
        framePerfData_t *prevFrame = &perfState.frameHistory[perfState.currentFrameIndex - 1];
        frame->missedVSync = prevFrame->missedVSync;
    }
    
    // Initialize new frame
    frame->frameNumber = tr.frameCount;
    frame->startTime = ri.Milliseconds();
    frame->wasThrottled = qfalse;
    frame->hadStutter = qfalse;
    frame->hadHitch = qfalse;
}

/*
===============
R_EndFrameMonitoring

End frame monitoring and analyze results
===============
*/
void R_EndFrameMonitoring( void ) {
    if ( !perfState.initialized || !perfState.monitoring ) {
        return;
    }
    
    framePerfData_t *frame = &perfState.frameHistory[perfState.currentFrameIndex];
    frame->endTime = ri.Milliseconds();
    frame->frameTime = (float)( frame->endTime - frame->startTime );
    
    // Update monitors
    float fps = frame->frameTime > 0 ? 1000.0f / frame->frameTime : 0;
    
    for ( uint32_t i = 0; i < perfState.monitorCount; i++ ) {
        perfMonitor_t *monitor = &perfState.monitors[i];
        
        switch ( monitor->type ) {
            case PERF_MONITOR_FPS:
                R_UpdatePerfMonitor( monitor, fps );
                break;
            case PERF_MONITOR_FRAMETIME:
                R_UpdatePerfMonitor( monitor, frame->frameTime );
                break;
            case PERF_MONITOR_CPU_USAGE:
                R_UpdatePerfMonitor( monitor, perfState.systemMetrics.cpuUsage );
                break;
            case PERF_MONITOR_GPU_USAGE:
                R_UpdatePerfMonitor( monitor, perfState.systemMetrics.gpuUsage );
                break;
            default:
                break;
        }
    }
    
    // Analyze frame timing
    R_AnalyzeFrameTiming();
    
    // Detect performance issues
    R_DetectFrameDrops();
    R_DetectStutters();
    R_DetectHitches();
    
    // Analyze bottlenecks
    R_AnalyzePerfBottlenecks();
    
    // Update prediction
    R_UpdatePerformancePrediction();
    
    // Record if enabled
    // NOTE: File writing is not supported in renderer - recording disabled
    if ( perfState.recording ) {
        perfState.recording = qfalse; // Disable recording
    }
    
    // Move to next frame
    perfState.currentFrameIndex = ( perfState.currentFrameIndex + 1 ) % perfState.frameHistorySize;
}

/*
===============
R_UpdateFrameMetrics

Update frame timing metrics
===============
*/
void R_UpdateFrameMetrics( float cpuTime, float gpuTime, float presentTime ) {
    if ( !perfState.initialized || !perfState.monitoring ) {
        return;
    }
    
    framePerfData_t *frame = &perfState.frameHistory[perfState.currentFrameIndex];
    frame->cpuTime = cpuTime;
    frame->gpuTime = gpuTime;
    frame->presentTime = presentTime;
    
    // Calculate latencies
    frame->renderLatency = cpuTime + gpuTime + presentTime;
    frame->inputLatency = frame->renderLatency;  // Simplified - would need input timing
}

/*
===============
R_UpdateFPSMonitor

Update FPS monitor
===============
*/
static void R_UpdateFPSMonitor( perfMonitor_t *monitor ) {
    // Check for consistent low FPS
    if ( monitor->average < monitor->criticalThreshold ) {
        R_RecordPerfEvent( PERF_EVENT_FRAME_DROP, PERF_SEVERITY_CRITICAL, 
                          "FPS consistently below critical threshold" );
    }
}

/*
===============
R_UpdateFrameTimeMonitor

Update frame time monitor
===============
*/
static void R_UpdateFrameTimeMonitor( perfMonitor_t *monitor ) {
    // Check for frame time spikes
    if ( monitor->current > monitor->average * 2.0f ) {
        R_RecordPerfEventWithValue( PERF_EVENT_SPIKE, PERF_SEVERITY_WARNING, 
                                   monitor->current, "Frame time spike detected" );
    }
}

/*
===============
R_UpdateCPUMonitor

Update CPU usage monitor
===============
*/
static void R_UpdateCPUMonitor( perfMonitor_t *monitor ) {
    if ( monitor->current > monitor->criticalThreshold ) {
        R_RecordPerfEvent( PERF_EVENT_CPU_STALL, PERF_SEVERITY_CRITICAL, 
                          "CPU usage at critical level" );
    }
}

/*
===============
R_UpdateGPUMonitor

Update GPU usage monitor
===============
*/
static void R_UpdateGPUMonitor( perfMonitor_t *monitor ) {
    if ( monitor->current > monitor->criticalThreshold ) {
        R_RecordPerfEvent( PERF_EVENT_GPU_STALL, PERF_SEVERITY_CRITICAL, 
                          "GPU usage at critical level" );
    }
}

/*
===============
R_AnalyzeFrameTiming

Analyze frame timing patterns
===============
*/
static void R_AnalyzeFrameTiming( void ) {
    if ( perfState.currentFrameIndex < 10 ) {
        return;  // Need some history
    }
    
    // Calculate moving average and variance
    float sum = 0;
    float sumSquared = 0;
    uint32_t samples = Min( perfState.currentFrameIndex, 30 );
    
    for ( uint32_t i = 0; i < samples; i++ ) {
        uint32_t idx = ( perfState.currentFrameIndex - i - 1 + perfState.frameHistorySize ) % 
                       perfState.frameHistorySize;
        float frameTime = perfState.frameHistory[idx].frameTime;
        sum += frameTime;
        sumSquared += frameTime * frameTime;
    }
    
    float average = sum / samples;
    float variance = ( sumSquared / samples ) - ( average * average );
    
    // Check for unstable frame times
    if ( variance > perfState.acceptableVariance * perfState.acceptableVariance ) {
        R_RecordPerfEventWithValue( PERF_EVENT_STUTTER, PERF_SEVERITY_WARNING, 
                                   sqrtf( variance ), "Unstable frame times detected" );
    }
}

/*
===============
R_DetectFrameDrops

Detect dropped frames
===============
*/
void R_DetectFrameDrops( void ) {
    if ( !perfState.initialized || perfState.currentFrameIndex == 0 ) {
        return;
    }
    
    framePerfData_t *frame = &perfState.frameHistory[perfState.currentFrameIndex];
    float targetTime = 1000.0f / perfState.targetFPS;
    
    if ( frame->frameTime > targetTime * 1.5f ) {
        frame->missedVSync++;
        
        char message[256];
        Com_sprintf( message, sizeof( message ), 
                    "Frame drop detected: %.2fms (target: %.2fms)", 
                    frame->frameTime, targetTime );
        R_RecordPerfEventWithValue( PERF_EVENT_FRAME_DROP, PERF_SEVERITY_WARNING, 
                                   frame->frameTime, message );
    }
}

/*
===============
R_DetectStutters

Detect stuttering
===============
*/
void R_DetectStutters( void ) {
    if ( !perfState.initialized || perfState.currentFrameIndex < 3 ) {
        return;
    }
    
    // Compare current frame time with recent average
    float recentAvg = 0;
    for ( uint32_t i = 1; i <= 3; i++ ) {
        uint32_t idx = ( perfState.currentFrameIndex - i + perfState.frameHistorySize ) % 
                       perfState.frameHistorySize;
        recentAvg += perfState.frameHistory[idx].frameTime;
    }
    recentAvg /= 3.0f;
    
    framePerfData_t *frame = &perfState.frameHistory[perfState.currentFrameIndex];
    
    if ( frame->frameTime > recentAvg * 1.5f || frame->frameTime < recentAvg * 0.5f ) {
        frame->hadStutter = qtrue;
        R_RecordPerfEvent( PERF_EVENT_STUTTER, PERF_SEVERITY_WARNING, 
                          "Frame time stutter detected" );
    }
}

/*
===============
R_DetectHitches

Detect frame hitches
===============
*/
void R_DetectHitches( void ) {
    if ( !perfState.initialized ) {
        return;
    }
    
    framePerfData_t *frame = &perfState.frameHistory[perfState.currentFrameIndex];
    
    // Hitch = frame time > 100ms
    if ( frame->frameTime > 100.0f ) {
        frame->hadHitch = qtrue;
        
        char message[256];
        Com_sprintf( message, sizeof( message ), 
                    "Severe hitch detected: %.2fms", frame->frameTime );
        R_RecordPerfEventWithValue( PERF_EVENT_HITCH, PERF_SEVERITY_CRITICAL, 
                                   frame->frameTime, message );
    }
}

/*
===============
R_AnalyzeBottlenecks

Analyze performance bottlenecks
===============
*/
static void R_AnalyzePerfBottlenecks( void ) {
    if ( !perfState.initialized || perfState.currentFrameIndex == 0 ) {
        return;
    }
    
    framePerfData_t *frame = &perfState.frameHistory[perfState.currentFrameIndex];
    perfBottleneck_t *bottleneck = &perfState.currentBottleneck;
    
    // Reset bottleneck
    bottleneck->type = BOTTLENECK_NONE;
    bottleneck->severity = 0;
    
    // Check CPU vs GPU
    if ( frame->cpuTime > 0 && frame->gpuTime > 0 ) {
        float cpuPercent = frame->cpuTime / frame->frameTime;
        float gpuPercent = frame->gpuTime / frame->frameTime;
        
        if ( cpuPercent > 0.8f ) {
            bottleneck->type = BOTTLENECK_CPU;
            bottleneck->severity = cpuPercent;
            Q_strncpyz( bottleneck->description, "CPU-bound: CPU time dominates frame time", 
                       sizeof( bottleneck->description ) );
            Q_strncpyz( bottleneck->recommendation, 
                       "Reduce draw calls, optimize game logic, lower CPU-intensive settings", 
                       sizeof( bottleneck->recommendation ) );
        } else if ( gpuPercent > 0.8f ) {
            bottleneck->type = BOTTLENECK_GPU;
            bottleneck->severity = gpuPercent;
            Q_strncpyz( bottleneck->description, "GPU-bound: GPU time dominates frame time", 
                       sizeof( bottleneck->description ) );
            Q_strncpyz( bottleneck->recommendation, 
                       "Lower resolution, reduce quality settings, optimize shaders", 
                       sizeof( bottleneck->recommendation ) );
        }
    }
    
    // Check memory pressure
    if ( perfState.systemMetrics.memoryPressure > 0.9f ) {
        if ( bottleneck->type == BOTTLENECK_NONE || bottleneck->severity < perfState.systemMetrics.memoryPressure ) {
            bottleneck->type = BOTTLENECK_MEMORY;
            bottleneck->severity = perfState.systemMetrics.memoryPressure;
            Q_strncpyz( bottleneck->description, "Memory pressure: System running low on memory", 
                       sizeof( bottleneck->description ) );
            Q_strncpyz( bottleneck->recommendation, 
                       "Reduce texture quality, close background applications", 
                       sizeof( bottleneck->recommendation ) );
        }
    }
    
    // Check VSync limitation
    float targetTime = 1000.0f / perfState.targetFPS;
    if ( fabs( frame->frameTime - targetTime ) < 0.5f && frame->frameTime < frame->cpuTime + frame->gpuTime ) {
        bottleneck->type = BOTTLENECK_VSYNC;
        bottleneck->severity = 0.5f;
        Q_strncpyz( bottleneck->description, "VSync-limited: Frame rate capped by display refresh", 
                   sizeof( bottleneck->description ) );
        Q_strncpyz( bottleneck->recommendation, 
                   "Disable VSync for higher frame rates or use adaptive sync", 
                   sizeof( bottleneck->recommendation ) );
    }
}

/*
===============
R_UpdatePerformancePrediction

Update performance prediction
===============
*/
void R_UpdatePerformancePrediction( void ) {
    if ( !perfState.initialized || perfState.currentFrameIndex < 10 ) {
        return;
    }
    
    perfPrediction_t *pred = &perfState.prediction;
    
    // Simple linear regression on recent frame times
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    uint32_t samples = Min( 10, perfState.currentFrameIndex );
    
    for ( uint32_t i = 0; i < samples; i++ ) {
        uint32_t idx = ( perfState.currentFrameIndex - i - 1 + perfState.frameHistorySize ) % 
                       perfState.frameHistorySize;
        float x = (float)i;
        float y = perfState.frameHistory[idx].frameTime;
        
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }
    
    float n = (float)samples;
    float slope = ( n * sumXY - sumX * sumY ) / ( n * sumX2 - sumX * sumX );
    float intercept = ( sumY - slope * sumX ) / n;
    
    // Predict next frame
    pred->nextFrameTime = intercept;  // x=0 for next frame
    pred->trend = slope;
    
    // Calculate confidence based on variance
    float variance = 0;
    for ( uint32_t i = 0; i < samples; i++ ) {
        uint32_t idx = ( perfState.currentFrameIndex - i - 1 + perfState.frameHistorySize ) % 
                       perfState.frameHistorySize;
        float predicted = slope * i + intercept;
        float diff = perfState.frameHistory[idx].frameTime - predicted;
        variance += diff * diff;
    }
    variance /= samples;
    
    pred->confidence = 1.0f / ( 1.0f + variance / 100.0f );  // Convert variance to 0-1 confidence
    
    // Predict frames until target
    float targetTime = 1000.0f / perfState.targetFPS;
    if ( slope != 0 ) {
        pred->framesUntilTarget = (uint32_t)( ( targetTime - intercept ) / slope );
    } else {
        pred->framesUntilTarget = pred->nextFrameTime > targetTime ? UINT32_MAX : 0;
    }
    
    pred->willMissTarget = pred->nextFrameTime > targetTime;
}

/*
===============
R_RecordPerfEvent

Record a performance event
===============
*/
void R_RecordPerfEvent( perfEventType_t type, perfSeverity_t severity, const char *description ) {
    R_RecordPerfEventWithValue( type, severity, 0, description );
}

/*
===============
R_RecordPerfEventWithValue

Record a performance event with a value
===============
*/
void R_RecordPerfEventWithValue( perfEventType_t type, perfSeverity_t severity, float value, const char *description ) {
    if ( !perfState.initialized ) {
        return;
    }
    
    // Check alert level
    if ( severity < perfState.alertLevel ) {
        return;
    }
    
    perfEvent_t *event = R_AllocPerfEvent();
    if ( !event ) {
        return;  // Event pool full
    }
    
    event->type = type;
    event->severity = severity;
    event->timestamp = ri.Milliseconds();
    event->frameNumber = tr.frameCount;
    event->value = value;
    Q_strncpyz( event->description, description, sizeof( event->description ) );
    
    // Add to event list
    event->next = perfState.events;
    perfState.events = event;
    perfState.eventCount++;
    
    // Trigger global alert callback
    if ( perfState.alertsEnabled && perfState.globalAlertCallback ) {
        perfState.globalAlertCallback( type, severity, description );
    }
}

/*
===============
R_AllocPerfEvent

Allocate a performance event from the pool
===============
*/
static perfEvent_t* R_AllocPerfEvent( void ) {
    if ( !perfState.eventPool ) {
        return NULL;
    }
    
    // Find free event
    for ( uint32_t i = 0; i < perfState.maxEvents; i++ ) {
        if ( perfState.eventPool[i].frameNumber == 0 ) {
            return &perfState.eventPool[i];
        }
    }
    
    // Pool full - reuse oldest
    perfEvent_t *oldest = &perfState.eventPool[0];
    for ( uint32_t i = 1; i < perfState.maxEvents; i++ ) {
        if ( perfState.eventPool[i].timestamp < oldest->timestamp ) {
            oldest = &perfState.eventPool[i];
        }
    }
    
    return oldest;
}

/*
===============
R_ClearPerfEvents

Clear all performance events
===============
*/
void R_ClearPerfEvents( void ) {
    if ( !perfState.initialized ) {
        return;
    }
    
    perfState.events = NULL;
    perfState.eventCount = 0;
    
    if ( perfState.eventPool ) {
        Com_Memset( perfState.eventPool, 0, sizeof( perfEvent_t ) * perfState.maxEvents );
    }
}

/*
===============
R_GetCurrentBottleneck

Get current performance bottleneck
===============
*/
perfBottleneck_t* R_GetCurrentBottleneck( void ) {
    if ( !perfState.initialized ) {
        return NULL;
    }
    
    return &perfState.currentBottleneck;
}

/*
===============
R_IsCPUBound

Check if performance is CPU-bound
===============
*/
qboolean R_IsCPUBound( void ) {
    if ( !perfState.initialized ) {
        return qfalse;
    }
    
    return perfState.currentBottleneck.type == BOTTLENECK_CPU;
}

/*
===============
R_IsGPUBound

Check if performance is GPU-bound
===============
*/
qboolean R_IsGPUBound( void ) {
    if ( !perfState.initialized ) {
        return qfalse;
    }
    
    return perfState.currentBottleneck.type == BOTTLENECK_GPU;
}

/*
===============
R_DrawPerfOverlay

Draw performance overlay
===============
*/
void R_DrawPerfOverlay( void ) {
    if ( !perfState.initialized || !perfState.showMonitors ) {
        return;
    }
    
    float x = perfState.displayPosition[0];
    float y = perfState.displayPosition[1];
    float scale = perfState.displayScale;
    vec4_t color;
    
    // Title
    VectorSet4( color, 1.0f, 1.0f, 1.0f, 1.0f );
    R_DrawString( x, y, "Performance Monitors", scale * 1.2f, color );
    y += 25 * scale;
    
    // Draw monitors
    for ( uint32_t i = 0; i < perfState.monitorCount; i++ ) {
        perfMonitor_t *monitor = &perfState.monitors[i];
        if ( !monitor->enabled ) {
            continue;
        }
        
        // Get severity color
        vec4_t monColor;
        R_GetSeverityColor( monitor->currentSeverity, monColor );
        
        // Format value
        char valueStr[64];
        switch ( monitor->type ) {
            case PERF_MONITOR_FPS:
                Com_sprintf( valueStr, sizeof( valueStr ), "%.1f FPS", monitor->current );
                break;
            case PERF_MONITOR_FRAMETIME:
                Com_sprintf( valueStr, sizeof( valueStr ), "%.2f ms", monitor->current );
                break;
            case PERF_MONITOR_CPU_USAGE:
            case PERF_MONITOR_GPU_USAGE:
                Com_sprintf( valueStr, sizeof( valueStr ), "%.1f%%", monitor->current );
                break;
            default:
                Com_sprintf( valueStr, sizeof( valueStr ), "%.2f", monitor->current );
                break;
        }
        
        // Draw monitor line
        char line[128];
        Com_sprintf( line, sizeof( line ), "%s: %s (avg: %.2f)", 
                    monitor->name, valueStr, monitor->average );
        R_DrawString( x, y, line, scale, monColor );
        y += 18 * scale;
    }
    
    // Draw bottleneck info
    if ( perfState.showBottleneck && perfState.currentBottleneck.type != BOTTLENECK_NONE ) {
        y += 10 * scale;
        VectorSet4( color, 1.0f, 0.8f, 0.2f, 1.0f );
        R_DrawString( x, y, "Bottleneck:", scale, color );
        y += 18 * scale;
        R_DrawString( x + 10, y, perfState.currentBottleneck.description, scale * 0.9f, color );
        y += 18 * scale;
    }
    
    // Draw recent events
    if ( perfState.showEvents && perfState.events ) {
        y += 10 * scale;
        VectorSet4( color, 1.0f, 1.0f, 1.0f, 1.0f );
        R_DrawString( x, y, "Recent Events:", scale, color );
        y += 18 * scale;
        
        perfEvent_t *event = perfState.events;
        int count = 0;
        while ( event && count < 5 ) {
            vec4_t eventColor;
            R_GetSeverityColor( event->severity, eventColor );
            
            char eventStr[128];
            Com_sprintf( eventStr, sizeof( eventStr ), "[%s] %s", 
                        R_GetEventTypeName( event->type ), event->description );
            R_DrawString( x + 10, y, eventStr, scale * 0.85f, eventColor );
            y += 15 * scale;
            
            event = event->next;
            count++;
        }
    }
}

/*
===============
R_GetSeverityColor

Get color for severity level
===============
*/
void R_GetSeverityColor( perfSeverity_t severity, vec_t *color ) {
    static vec4_t colors[] = {
        { 0.2f, 1.0f, 0.2f, 1.0f },  // Info - Green
        { 1.0f, 1.0f, 0.2f, 1.0f },  // Warning - Yellow
        { 1.0f, 0.5f, 0.2f, 1.0f },  // Error - Orange
        { 1.0f, 0.2f, 0.2f, 1.0f }   // Critical - Red
    };
    
    if ( severity >= 0 && severity < 4 ) {
        VectorCopy4( colors[severity], color );
    } else {
        VectorCopy4( colors[0], color );
    }
}

/*
===============
R_GetMonitorTypeName

Get name for monitor type
===============
*/
const char* R_GetMonitorTypeName( perfMonitorType_t type ) {
    if ( type >= 0 && type < PERF_MONITOR_CUSTOM ) {
        return monitorTypeNames[type];
    }
    return "Unknown";
}

/*
===============
R_GetEventTypeName

Get name for event type
===============
*/
const char* R_GetEventTypeName( perfEventType_t type ) {
    if ( type >= 0 && type < PERF_EVENT_CUSTOM ) {
        return eventTypeNames[type];
    }
    return "Unknown";
}

/*
===============
R_GetSeverityName

Get name for severity level
===============
*/
const char* R_GetSeverityName( perfSeverity_t severity ) {
    if ( severity >= 0 && severity < 4 ) {
        return severityNames[severity];
    }
    return "Unknown";
}