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
// tr_perf.h - Performance Monitoring System

#ifndef __TR_PERF_H
#define __TR_PERF_H

#include "../tr_local.h"

// Performance monitoring configuration
#define PERF_MAX_MONITORS          64
#define PERF_MAX_EVENTS           256
#define PERF_MAX_SAMPLES         1024
#define PERF_HISTORY_FRAMES       300
#define PERF_MAX_NAME_LENGTH       64
#define PERF_WARNING_THRESHOLD    0.8f
#define PERF_CRITICAL_THRESHOLD   0.95f

// Performance monitor types
typedef enum {
    PERF_MONITOR_FPS = 0,
    PERF_MONITOR_FRAMETIME,
    PERF_MONITOR_CPU_USAGE,
    PERF_MONITOR_GPU_USAGE,
    PERF_MONITOR_MEMORY,
    PERF_MONITOR_BANDWIDTH,
    PERF_MONITOR_LATENCY,
    PERF_MONITOR_STALLS,
    PERF_MONITOR_CACHE,
    PERF_MONITOR_THERMAL,
    PERF_MONITOR_POWER,
    PERF_MONITOR_CUSTOM,
} perfMonitorType_t;

// Performance event types
typedef enum {
    PERF_EVENT_FRAME_DROP = 0,
    PERF_EVENT_STUTTER,
    PERF_EVENT_HITCH,
    PERF_EVENT_SPIKE,
    PERF_EVENT_THROTTLE,
    PERF_EVENT_MEMORY_PRESSURE,
    PERF_EVENT_GPU_STALL,
    PERF_EVENT_CPU_STALL,
    PERF_EVENT_PIPELINE_FLUSH,
    PERF_EVENT_CACHE_THRASH,
    PERF_EVENT_THERMAL_LIMIT,
    PERF_EVENT_POWER_LIMIT,
    PERF_EVENT_CUSTOM,
} perfEventType_t;

// Performance severity levels
typedef enum {
    PERF_SEVERITY_INFO = 0,
    PERF_SEVERITY_WARNING,
    PERF_SEVERITY_ERROR,
    PERF_SEVERITY_CRITICAL,
} perfSeverity_t;

// Performance monitor structure
typedef struct perfMonitor_s {
    char                    name[PERF_MAX_NAME_LENGTH];
    perfMonitorType_t       type;
    qboolean                enabled;
    qboolean                autoScale;
    
    // Current values
    float                   current;
    float                   average;
    float                   minimum;
    float                   maximum;
    float                   variance;
    float                   stddev;
    
    // History
    float                  *samples;
    uint32_t                sampleCount;
    uint32_t                sampleIndex;
    uint32_t                maxSamples;
    
    // Thresholds
    float                   targetValue;
    float                   warningThreshold;
    float                   criticalThreshold;
    perfSeverity_t          currentSeverity;
    
    // Smoothing
    float                   smoothingFactor;
    float                   smoothedValue;
    float                   predictedValue;
    
    // Callbacks
    void                  (*updateCallback)( struct perfMonitor_s *monitor );
    void                  (*alertCallback)( struct perfMonitor_s *monitor, perfSeverity_t severity );
} perfMonitor_t;

// Performance event structure
typedef struct perfEvent_s {
    perfEventType_t         type;
    perfSeverity_t          severity;
    uint64_t                timestamp;
    uint64_t                frameNumber;
    char                    description[256];
    float                   value;
    float                   duration;
    vec3_t                  location;      // Optional world location
    uint32_t                entityId;      // Optional entity ID
    struct perfEvent_s     *next;
} perfEvent_t;

// Frame performance data
typedef struct framePerfData_s {
    uint64_t                frameNumber;
    uint64_t                startTime;
    uint64_t                endTime;
    float                   frameTime;
    float                   cpuTime;
    float                   gpuTime;
    float                   presentTime;
    float                   inputLatency;
    float                   renderLatency;
    uint32_t                missedVSync;
    qboolean                wasThrottled;
    qboolean                hadStutter;
    qboolean                hadHitch;
} framePerfData_t;

// System performance metrics
typedef struct systemPerf_s {
    // CPU metrics
    float                   cpuUsage;
    float                   cpuFrequency;
    float                   cpuTemperature;
    uint32_t                cpuCores;
    uint32_t                cpuThreads;
    
    // GPU metrics
    float                   gpuUsage;
    float                   gpuFrequency;
    float                   gpuTemperature;
    float                   gpuMemoryUsage;
    float                   gpuMemoryBandwidth;
    
    // Memory metrics
    uint64_t                systemMemoryUsed;
    uint64_t                systemMemoryTotal;
    uint64_t                gpuMemoryUsed;
    uint64_t                gpuMemoryTotal;
    float                   memoryPressure;
    
    // Power metrics
    float                   cpuPower;
    float                   gpuPower;
    float                   totalPower;
    qboolean                powerThrottled;
    
    // Thermal metrics
    qboolean                thermalThrottled;
    float                   thermalHeadroom;
} systemPerf_t;

// Performance bottleneck analysis
typedef struct perfBottleneck_s {
    enum {
        BOTTLENECK_NONE = 0,
        BOTTLENECK_CPU,
        BOTTLENECK_GPU,
        BOTTLENECK_MEMORY,
        BOTTLENECK_BANDWIDTH,
        BOTTLENECK_VSYNC,
        BOTTLENECK_IO,
    } type;
    float                   severity;      // 0-1 severity scale
    char                    description[256];
    char                    recommendation[512];
} perfBottleneck_t;

// Performance prediction
typedef struct perfPrediction_s {
    float                   nextFrameTime;
    float                   confidence;
    float                   trend;         // Positive = getting worse
    uint32_t                framesUntilTarget;
    qboolean                willMissTarget;
} perfPrediction_t;

// Performance state
typedef struct perfState_s {
    qboolean                initialized;
    qboolean                monitoring;
    qboolean                recording;
    
    // Monitors
    perfMonitor_t           monitors[PERF_MAX_MONITORS];
    uint32_t                monitorCount;
    
    // Events
    perfEvent_t            *events;
    perfEvent_t            *eventPool;
    uint32_t                eventCount;
    uint32_t                maxEvents;
    
    // Frame history
    framePerfData_t        *frameHistory;
    uint32_t                frameHistorySize;
    uint32_t                currentFrameIndex;
    
    // System metrics
    systemPerf_t            systemMetrics;
    uint64_t                lastSystemUpdate;
    
    // Analysis
    perfBottleneck_t        currentBottleneck;
    perfPrediction_t        prediction;
    
    // Target performance
    float                   targetFPS;
    float                   targetFrameTime;
    float                   acceptableVariance;
    
    // Alerts
    qboolean                alertsEnabled;
    perfSeverity_t          alertLevel;
    void                  (*globalAlertCallback)( perfEventType_t type, perfSeverity_t severity, const char *message );
    
    // Recording
    fileHandle_t            recordFile;
    uint32_t                recordedFrames;
    uint64_t                recordStartTime;
    
    // Display
    qboolean                showMonitors;
    qboolean                showEvents;
    qboolean                showBottleneck;
    vec2_t                  displayPosition;
    float                   displayScale;
} perfState_t;

// Global performance state
extern perfState_t perfState;

// Initialization
qboolean R_InitPerformanceMonitoring( void );
void R_ShutdownPerformanceMonitoring( void );
void R_ResetPerformanceMonitoring( void );

// Frame monitoring
void R_BeginFrameMonitoring( void );
void R_EndFrameMonitoring( void );
void R_UpdateFrameMetrics( float cpuTime, float gpuTime, float presentTime );

// Monitor management
perfMonitor_t* R_CreatePerfMonitor( const char *name, perfMonitorType_t type );
void R_DestroyPerfMonitor( perfMonitor_t *monitor );
void R_UpdatePerfMonitor( perfMonitor_t *monitor, float value );
void R_SetMonitorThresholds( perfMonitor_t *monitor, float target, float warning, float critical );
void R_EnablePerfMonitor( perfMonitor_t *monitor, qboolean enable );

// Event tracking
void R_RecordPerfEvent( perfEventType_t type, perfSeverity_t severity, const char *description );
void R_RecordPerfEventWithValue( perfEventType_t type, perfSeverity_t severity, float value, const char *description );
void R_ClearPerfEvents( void );
perfEvent_t* R_GetRecentPerfEvents( uint32_t maxCount );

// System monitoring
void R_UpdateSystemMetrics( void );
void R_GetSystemPerformance( systemPerf_t *metrics );
float R_GetCPUUsage( void );
float R_GetGPUUsage( void );
float R_GetMemoryPressure( void );

// Bottleneck analysis
void R_AnalyzeBottlenecks( void );
perfBottleneck_t* R_GetCurrentBottleneck( void );
const char* R_GetBottleneckRecommendation( void );
qboolean R_IsCPUBound( void );
qboolean R_IsGPUBound( void );

// Performance prediction
void R_UpdatePerformancePrediction( void );
perfPrediction_t* R_GetPerformancePrediction( void );
float R_PredictNextFrameTime( void );
uint32_t R_GetFramesUntilTarget( void );

// Frame analysis
void R_DetectFrameDrops( void );
void R_DetectStutters( void );
void R_DetectHitches( void );
qboolean R_IsPerformanceStable( void );
float R_GetFrameTimeVariance( void );

// Alerts and notifications
void R_SetPerfAlertCallback( void (*callback)( perfEventType_t, perfSeverity_t, const char* ) );
void R_SetPerfAlertLevel( perfSeverity_t level );
void R_EnablePerfAlerts( qboolean enable );
void R_TriggerPerfAlert( perfEventType_t type, perfSeverity_t severity, const char *message );

// Recording and playback
qboolean R_StartPerfRecording( const char *filename );
void R_StopPerfRecording( void );
qboolean R_LoadPerfRecording( const char *filename );
void R_PlaybackPerfFrame( uint32_t frameIndex );

// Reporting
void R_GeneratePerfReport( void );
void R_PrintPerfSummary( void );
void R_ExportPerfData( const char *filename );
void R_DrawPerfOverlay( void );

// Optimization suggestions
void R_AnalyzeOptimizationOpportunities( void );
void R_SuggestQualitySettings( void );
void R_AutoAdjustSettings( void );

// Helpers
const char* R_GetMonitorTypeName( perfMonitorType_t type );
const char* R_GetEventTypeName( perfEventType_t type );
const char* R_GetSeverityName( perfSeverity_t severity );
void R_GetSeverityColor( perfSeverity_t severity, vec4_t color );

// Configuration
void R_SetTargetFPS( float fps );
void R_SetPerformanceMode( const char *mode );  // "quality", "balanced", "performance"
void R_SetMonitoringEnabled( qboolean enable );
void R_SetPerfOverlay( qboolean show );

#endif // __TR_PERF_H