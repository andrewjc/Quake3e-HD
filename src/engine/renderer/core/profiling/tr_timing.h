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
// tr_timing.h - Frame Timing Analysis System

#ifndef __TR_TIMING_H
#define __TR_TIMING_H

#include "../tr_local.h"

// Timing configuration
#define TIMING_MAX_PHASES           32
#define TIMING_MAX_MARKERS          64
#define TIMING_HISTORY_SIZE        120
#define TIMING_MAX_NAME_LENGTH      64
#define TIMING_SPIKE_THRESHOLD      1.5f   // 150% of average

// Frame phase types
typedef enum {
    FRAME_PHASE_INPUT = 0,
    FRAME_PHASE_GAME_LOGIC,
    FRAME_PHASE_ANIMATION,
    FRAME_PHASE_PHYSICS,
    FRAME_PHASE_VISIBILITY,
    FRAME_PHASE_CULLING,
    FRAME_PHASE_SCENE_SETUP,
    FRAME_PHASE_SHADOW_MAPS,
    FRAME_PHASE_GBUFFER,
    FRAME_PHASE_LIGHTING,
    FRAME_PHASE_TRANSPARENCY,
    FRAME_PHASE_POST_PROCESS,
    FRAME_PHASE_UI,
    FRAME_PHASE_PRESENT,
    FRAME_PHASE_CUSTOM,
} framePhase_t;

// Timing marker structure
typedef struct timingMarker_s {
    char                    name[TIMING_MAX_NAME_LENGTH];
    uint64_t                timestamp;
    framePhase_t            phase;
    float                   cpuTime;
    float                   gpuTime;
    qboolean                isStart;       // Start or end marker
    uint32_t                depth;          // Nesting depth
} timingMarker_t;

// Frame phase timing
typedef struct phaseTiming_s {
    char                    name[TIMING_MAX_NAME_LENGTH];
    framePhase_t            type;
    uint64_t                startTime;
    uint64_t                endTime;
    float                   duration;
    float                   cpuDuration;
    float                   gpuDuration;
    float                   avgDuration;
    float                   minDuration;
    float                   maxDuration;
    uint32_t                spikeCount;
    uint32_t                subPhaseCount;
    struct phaseTiming_s   *subPhases[TIMING_MAX_PHASES];
    struct phaseTiming_s   *parent;
} phaseTiming_t;

// Frame timing data
typedef struct frameTiming_s {
    uint64_t                frameNumber;
    uint64_t                frameStartTime;
    uint64_t                frameEndTime;
    float                   totalDuration;
    float                   cpuDuration;
    float                   gpuDuration;
    float                   syncDuration;
    
    // Phase breakdown
    phaseTiming_t           phases[TIMING_MAX_PHASES];
    uint32_t                phaseCount;
    phaseTiming_t          *currentPhase;
    
    // Markers
    timingMarker_t          markers[TIMING_MAX_MARKERS];
    uint32_t                markerCount;
    
    // Analysis
    float                   variance;
    float                   jitter;
    qboolean                hadSpike;
    qboolean                missedTarget;
} frameTiming_t;

// Timing statistics
typedef struct timingStats_s {
    float                   avgFrameTime;
    float                   minFrameTime;
    float                   maxFrameTime;
    float                   percentile95;
    float                   percentile99;
    float                   stdDev;
    float                   jitterAvg;
    uint32_t                totalFrames;
    uint32_t                droppedFrames;
    uint32_t                spikeFrames;
} timingStats_t;

// Timing state
typedef struct timingState_s {
    qboolean                initialized;
    qboolean                enabled;
    qboolean                detailed;       // Detailed phase tracking
    
    // Current frame
    frameTiming_t           currentFrame;
    
    // Frame history
    frameTiming_t          *frameHistory;
    uint32_t                historySize;
    uint32_t                historyIndex;
    
    // Statistics
    timingStats_t           stats;
    timingStats_t           phaseStats[FRAME_PHASE_CUSTOM];
    
    // Target timing
    float                   targetFrameTime;
    float                   vsyncInterval;
    qboolean                adaptiveSync;
    
    // Analysis
    float                   smoothFactor;
    float                   predictedFrameTime;
    float                   frameTimeAccumulator;
    uint32_t                accumulatorFrames;
    
    // Display
    qboolean                showTimeline;
    qboolean                showBreakdown;
    qboolean                showGraph;
} timingState_t;

// Global timing state
extern timingState_t frameTimingState;

// Initialization
qboolean R_InitFrameTiming( void );
void R_ShutdownFrameTiming( void );
void R_ResetFrameTiming( void );

// Frame timing
void R_BeginFrameTiming( void );
void R_EndFrameTiming( void );
void R_MarkFrameTime( const char *marker );

// Phase timing
void R_BeginPhase( framePhase_t phase, const char *name );
void R_EndPhase( framePhase_t phase );
void R_BeginSubPhase( const char *name );
void R_EndSubPhase( void );

// Timing queries
float R_GetFrameTime( void );
float R_GetPhaseTime( framePhase_t phase );
float R_GetAverageFrameTime( void );
float R_GetFrameTimeVariance( void );
float R_GetFrameJitter( void );

// Analysis
void R_AnalyzeFrameTiming( frameTiming_t *frame );
void R_DetectTimingSpikes( void );
void R_CalculateTimingStats( void );
void R_PredictFrameTiming( void );

// Reporting
void R_PrintTimingBreakdown( void );
void R_DrawTimingGraph( void );
void R_DrawTimingTimeline( void );
void R_ExportTimingData( const char *filename );

// Helpers
const char* R_GetPhaseName( framePhase_t phase );
void R_GetPhaseColor( framePhase_t phase, vec4_t color );
float R_GetPhasePercentage( framePhase_t phase );

// Configuration
void R_SetTargetFrameTime( float ms );
void R_SetTimingDetail( qboolean detailed );
void R_SetTimingDisplay( qboolean timeline, qboolean breakdown, qboolean graph );

#endif // __TR_TIMING_H