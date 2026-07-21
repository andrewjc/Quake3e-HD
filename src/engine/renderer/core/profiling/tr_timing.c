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
// tr_timing.c - Frame Timing Analysis Implementation

#include "tr_timing.h"
#include "../tr_local.h"
#include "../tr_common_utils.h"
#include <math.h>
#include <float.h>

// Platform-specific includes for alloca
#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif

// Helper macros
#ifndef Min
#define Min(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef Max
#define Max(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef Clamp
#define Clamp(v,min,max) ((v) < (min) ? (min) : ((v) > (max) ? (max) : (v)))
#endif
#ifndef VectorCopy4
#define VectorCopy4(a,b) ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2],(b)[3]=(a)[3])
#endif

// Global timing state
timingState_t frameTimingState;

// Phase names
static const char *phaseNames[] = {
    "Input",
    "Game Logic",
    "Animation",
    "Physics",
    "Visibility",
    "Culling",
    "Scene Setup",
    "Shadow Maps",
    "G-Buffer",
    "Lighting",
    "Transparency",
    "Post-Process",
    "UI",
    "Present",
    "Custom"
};

// Phase colors for visualization
static const vec4_t phaseColors[] = {
    { 0.2f, 0.8f, 0.2f, 1.0f },    // Input - Green
    { 0.8f, 0.2f, 0.8f, 1.0f },    // Game Logic - Magenta
    { 0.8f, 0.8f, 0.2f, 1.0f },    // Animation - Yellow
    { 0.2f, 0.8f, 0.8f, 1.0f },    // Physics - Cyan
    { 0.8f, 0.5f, 0.2f, 1.0f },    // Visibility - Orange
    { 0.5f, 0.5f, 0.8f, 1.0f },    // Culling - Light Blue
    { 0.8f, 0.2f, 0.2f, 1.0f },    // Scene Setup - Red
    { 0.4f, 0.4f, 0.4f, 1.0f },    // Shadow Maps - Gray
    { 0.2f, 0.5f, 0.8f, 1.0f },    // G-Buffer - Blue
    { 0.8f, 0.8f, 0.5f, 1.0f },    // Lighting - Light Yellow
    { 0.5f, 0.8f, 0.5f, 1.0f },    // Transparency - Light Green
    { 0.8f, 0.5f, 0.8f, 1.0f },    // Post-Process - Pink
    { 0.5f, 0.8f, 0.8f, 1.0f },    // UI - Light Cyan
    { 0.3f, 0.3f, 0.3f, 1.0f },    // Present - Dark Gray
    { 0.6f, 0.6f, 0.6f, 1.0f }     // Custom - Medium Gray
};

/*
===============
R_InitFrameTiming

Initialize frame timing system
===============
*/
qboolean R_InitFrameTiming( void ) {
    ri.Printf( PRINT_ALL, "Initializing frame timing system...\n" );
    
    Com_Memset( &frameTimingState, 0, sizeof( frameTimingState ) );
    
    // Allocate frame history
    frameTimingState.historySize = TIMING_HISTORY_SIZE;
    frameTimingState.frameHistory = ri.Malloc( sizeof( frameTiming_t ) * frameTimingState.historySize );
    Com_Memset( frameTimingState.frameHistory, 0, sizeof( frameTiming_t ) * frameTimingState.historySize );
    
    // Initialize phase names
    for ( int i = 0; i < FRAME_PHASE_CUSTOM; i++ ) {
        frameTiming_t *frame = &frameTimingState.currentFrame;
        phaseTiming_t *phase = &frame->phases[i];
        Q_strncpyz( phase->name, phaseNames[i], TIMING_MAX_NAME_LENGTH );
        phase->type = (framePhase_t)i;
        phase->minDuration = FLT_MAX;
        phase->maxDuration = -FLT_MAX;
    }
    
    // Set default target
    frameTimingState.targetFrameTime = 16.67f;  // 60 FPS
    frameTimingState.vsyncInterval = 16.67f;
    frameTimingState.smoothFactor = 0.1f;
    
    frameTimingState.initialized = qtrue;
    frameTimingState.enabled = qtrue;
    
    ri.Printf( PRINT_ALL, "Frame timing system initialized\n" );
    return qtrue;
}

/*
===============
R_ShutdownFrameTiming

Shutdown frame timing system
===============
*/
void R_ShutdownFrameTiming( void ) {
    if ( !frameTimingState.initialized ) {
        return;
    }
    
    if ( frameTimingState.frameHistory ) {
        ri.Free( frameTimingState.frameHistory );
    }
    
    Com_Memset( &frameTimingState, 0, sizeof( frameTimingState ) );
    ri.Printf( PRINT_ALL, "Frame timing system shutdown\n" );
}

/*
===============
R_ResetFrameTiming

Reset frame timing data
===============
*/
void R_ResetFrameTiming( void ) {
    if ( !frameTimingState.initialized ) {
        return;
    }
    
    // Clear history
    Com_Memset( frameTimingState.frameHistory, 0, 
               sizeof( frameTiming_t ) * frameTimingState.historySize );
    frameTimingState.historyIndex = 0;
    
    // Reset statistics
    Com_Memset( &frameTimingState.stats, 0, sizeof( timingStats_t ) );
    Com_Memset( frameTimingState.phaseStats, 0, 
               sizeof( timingStats_t ) * FRAME_PHASE_CUSTOM );
    
    // Reset current frame
    frameTiming_t *frame = &frameTimingState.currentFrame;
    uint64_t frameNum = frame->frameNumber;
    Com_Memset( frame, 0, sizeof( frameTiming_t ) );
    frame->frameNumber = frameNum;
    
    // Reset accumulator
    frameTimingState.frameTimeAccumulator = 0;
    frameTimingState.accumulatorFrames = 0;
    frameTimingState.predictedFrameTime = frameTimingState.targetFrameTime;
}

/*
===============
R_BeginFrameTiming

Begin timing a new frame
===============
*/
void R_BeginFrameTiming( void ) {
    if ( !frameTimingState.initialized || !frameTimingState.enabled ) {
        return;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    
    // Save previous frame to history
    if ( frame->frameNumber > 0 ) {
        frameTiming_t *histFrame = &frameTimingState.frameHistory[frameTimingState.historyIndex];
        *histFrame = *frame;
        frameTimingState.historyIndex = ( frameTimingState.historyIndex + 1 ) % frameTimingState.historySize;
    }
    
    // Reset current frame
    Com_Memset( frame, 0, sizeof( frameTiming_t ) );
    frame->frameNumber = tr.frameCount;
    frame->frameStartTime = ri.Microseconds() / 1000;
    
    // Initialize phases
    for ( int i = 0; i < FRAME_PHASE_CUSTOM; i++ ) {
        phaseTiming_t *phase = &frame->phases[i];
        Q_strncpyz( phase->name, phaseNames[i], TIMING_MAX_NAME_LENGTH );
        phase->type = (framePhase_t)i;
        phase->minDuration = FLT_MAX;
        phase->maxDuration = -FLT_MAX;
    }
}

/*
===============
R_EndFrameTiming

End frame timing and perform analysis
===============
*/
void R_EndFrameTiming( void ) {
    if ( !frameTimingState.initialized || !frameTimingState.enabled ) {
        return;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    frame->frameEndTime = ri.Microseconds() / 1000;
    frame->totalDuration = (float)( frame->frameEndTime - frame->frameStartTime );
    
    // Analyze frame
    R_AnalyzeFrameTiming( frame );
    
    // Update statistics
    R_CalculateTimingStats();
    
    // Detect spikes
    R_DetectTimingSpikes();
    
    // Update prediction
    R_PredictFrameTiming();
    
    // Check target
    frame->missedTarget = frame->totalDuration > frameTimingState.targetFrameTime;
    
    // Update accumulator
    frameTimingState.frameTimeAccumulator += frame->totalDuration;
    frameTimingState.accumulatorFrames++;
}

/*
===============
R_BeginPhase

Begin timing a frame phase
===============
*/
void R_BeginPhase( framePhase_t phase, const char *name ) {
    if ( !frameTimingState.initialized || !frameTimingState.enabled ) {
        return;
    }
    
    if ( phase >= FRAME_PHASE_CUSTOM ) {
        return;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    phaseTiming_t *phaseData = &frame->phases[phase];
    
    phaseData->startTime = ri.Microseconds() / 1000;
    phaseData->type = phase;
    
    if ( name ) {
        Q_strncpyz( phaseData->name, name, TIMING_MAX_NAME_LENGTH );
    }
    
    frame->currentPhase = phaseData;
}

/*
===============
R_EndPhase

End timing a frame phase
===============
*/
void R_EndPhase( framePhase_t phase ) {
    if ( !frameTimingState.initialized || !frameTimingState.enabled ) {
        return;
    }
    
    if ( phase >= FRAME_PHASE_CUSTOM ) {
        return;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    phaseTiming_t *phaseData = &frame->phases[phase];
    
    phaseData->endTime = ri.Microseconds() / 1000;
    phaseData->duration = (float)( phaseData->endTime - phaseData->startTime );
    
    // Update phase statistics
    if ( phaseData->duration < phaseData->minDuration ) {
        phaseData->minDuration = phaseData->duration;
    }
    if ( phaseData->duration > phaseData->maxDuration ) {
        phaseData->maxDuration = phaseData->duration;
    }
    
    // Running average
    phaseData->avgDuration = phaseData->avgDuration * 0.95f + phaseData->duration * 0.05f;
    
    // Check for spike
    if ( phaseData->avgDuration > 0 && 
         phaseData->duration > phaseData->avgDuration * TIMING_SPIKE_THRESHOLD ) {
        phaseData->spikeCount++;
        frame->hadSpike = qtrue;
    }
    
    frame->currentPhase = phaseData->parent;
}

/*
===============
R_BeginSubPhase

Begin timing a sub-phase
===============
*/
void R_BeginSubPhase( const char *name ) {
    if ( !frameTimingState.initialized || !frameTimingState.enabled || !frameTimingState.detailed ) {
        return;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    if ( !frame->currentPhase || frame->phaseCount >= TIMING_MAX_PHASES ) {
        return;
    }
    
    phaseTiming_t *subPhase = &frame->phases[frame->phaseCount++];
    Com_Memset( subPhase, 0, sizeof( phaseTiming_t ) );
    
    Q_strncpyz( subPhase->name, name, TIMING_MAX_NAME_LENGTH );
    subPhase->type = FRAME_PHASE_CUSTOM;
    subPhase->startTime = ri.Microseconds() / 1000;
    subPhase->parent = frame->currentPhase;
    subPhase->minDuration = FLT_MAX;
    subPhase->maxDuration = -FLT_MAX;
    
    // Add to parent's sub-phases
    if ( frame->currentPhase->subPhaseCount < TIMING_MAX_PHASES ) {
        frame->currentPhase->subPhases[frame->currentPhase->subPhaseCount++] = subPhase;
    }
    
    frame->currentPhase = subPhase;
}

/*
===============
R_EndSubPhase

End timing a sub-phase
===============
*/
void R_EndSubPhase( void ) {
    if ( !frameTimingState.initialized || !frameTimingState.enabled || !frameTimingState.detailed ) {
        return;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    if ( !frame->currentPhase || frame->currentPhase->type != FRAME_PHASE_CUSTOM ) {
        return;
    }
    
    phaseTiming_t *subPhase = frame->currentPhase;
    subPhase->endTime = ri.Microseconds() / 1000;
    subPhase->duration = (float)( subPhase->endTime - subPhase->startTime );
    
    // Update statistics
    if ( subPhase->duration < subPhase->minDuration ) {
        subPhase->minDuration = subPhase->duration;
    }
    if ( subPhase->duration > subPhase->maxDuration ) {
        subPhase->maxDuration = subPhase->duration;
    }
    
    subPhase->avgDuration = subPhase->avgDuration * 0.95f + subPhase->duration * 0.05f;
    
    frame->currentPhase = subPhase->parent;
}

/*
===============
R_MarkFrameTime

Add a timing marker
===============
*/
void R_MarkFrameTime( const char *marker ) {
    if ( !frameTimingState.initialized || !frameTimingState.enabled ) {
        return;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    if ( frame->markerCount >= TIMING_MAX_MARKERS ) {
        return;
    }
    
    timingMarker_t *mark = &frame->markers[frame->markerCount++];
    Q_strncpyz( mark->name, marker, TIMING_MAX_NAME_LENGTH );
    mark->timestamp = ri.Microseconds() / 1000;
    
    if ( frame->currentPhase ) {
        mark->phase = frame->currentPhase->type;
        mark->depth = 0;
        
        // Calculate depth
        phaseTiming_t *phase = frame->currentPhase;
        while ( phase->parent ) {
            mark->depth++;
            phase = phase->parent;
        }
    }
}

/*
===============
R_AnalyzeFrameTiming

Analyze frame timing data
===============
*/
void R_AnalyzeFrameTiming( frameTiming_t *frame ) {
    if ( !frame ) {
        return;
    }
    
    // Calculate total phase time
    float totalPhaseTime = 0;
    for ( int i = 0; i < FRAME_PHASE_CUSTOM; i++ ) {
        totalPhaseTime += frame->phases[i].duration;
    }
    
    // Calculate CPU vs GPU split (simplified)
    frame->cpuDuration = frame->phases[FRAME_PHASE_GAME_LOGIC].duration +
                         frame->phases[FRAME_PHASE_ANIMATION].duration +
                         frame->phases[FRAME_PHASE_PHYSICS].duration +
                         frame->phases[FRAME_PHASE_VISIBILITY].duration +
                         frame->phases[FRAME_PHASE_CULLING].duration +
                         frame->phases[FRAME_PHASE_SCENE_SETUP].duration;
    
    frame->gpuDuration = frame->phases[FRAME_PHASE_SHADOW_MAPS].duration +
                         frame->phases[FRAME_PHASE_GBUFFER].duration +
                         frame->phases[FRAME_PHASE_LIGHTING].duration +
                         frame->phases[FRAME_PHASE_TRANSPARENCY].duration +
                         frame->phases[FRAME_PHASE_POST_PROCESS].duration;
    
    frame->syncDuration = frame->totalDuration - totalPhaseTime;
    
    // Calculate jitter (variation from previous frame)
    if ( frameTimingState.historyIndex > 0 ) {
        uint32_t prevIdx = ( frameTimingState.historyIndex - 1 + frameTimingState.historySize ) % 
                          frameTimingState.historySize;
        frameTiming_t *prevFrame = &frameTimingState.frameHistory[prevIdx];
        frame->jitter = fabsf( frame->totalDuration - prevFrame->totalDuration );
    }
    
    // Calculate variance from average
    if ( frameTimingState.stats.avgFrameTime > 0 ) {
        float diff = frame->totalDuration - frameTimingState.stats.avgFrameTime;
        frame->variance = diff * diff;
    }
}

/*
===============
R_DetectTimingSpikes

Detect timing spikes in frame history
===============
*/
void R_DetectTimingSpikes( void ) {
    if ( !frameTimingState.initialized || frameTimingState.historyIndex < 10 ) {
        return;
    }
    
    // Calculate recent average
    float recentAvg = 0;
    uint32_t samples = Min( 10, frameTimingState.historyIndex );
    
    for ( uint32_t i = 0; i < samples; i++ ) {
        uint32_t idx = ( frameTimingState.historyIndex - i - 1 + frameTimingState.historySize ) % 
                       frameTimingState.historySize;
        recentAvg += frameTimingState.frameHistory[idx].totalDuration;
    }
    recentAvg /= samples;
    
    // Check current frame for spike
    frameTiming_t *frame = &frameTimingState.currentFrame;
    if ( frame->totalDuration > recentAvg * TIMING_SPIKE_THRESHOLD ) {
        frame->hadSpike = qtrue;
        frameTimingState.stats.spikeFrames++;
        
        ri.Printf( PRINT_DEVELOPER, "Frame timing spike: %.2fms (avg: %.2fms)\n",
                  frame->totalDuration, recentAvg );
    }
}

/*
===============
R_CalculateTimingStats

Calculate timing statistics
===============
*/
void R_CalculateTimingStats( void ) {
    if ( !frameTimingState.initialized || frameTimingState.historyIndex == 0 ) {
        return;
    }
    
    timingStats_t *stats = &frameTimingState.stats;
    uint32_t count = Min( frameTimingState.historyIndex, frameTimingState.historySize );
    
    // Reset stats
    stats->avgFrameTime = 0;
    stats->minFrameTime = FLT_MAX;
    stats->maxFrameTime = -FLT_MAX;
    stats->jitterAvg = 0;
    
    // Calculate basic stats
    for ( uint32_t i = 0; i < count; i++ ) {
        frameTiming_t *frame = &frameTimingState.frameHistory[i];
        
        stats->avgFrameTime += frame->totalDuration;
        stats->jitterAvg += frame->jitter;
        
        if ( frame->totalDuration < stats->minFrameTime ) {
            stats->minFrameTime = frame->totalDuration;
        }
        if ( frame->totalDuration > stats->maxFrameTime ) {
            stats->maxFrameTime = frame->totalDuration;
        }
        
        if ( frame->missedTarget ) {
            stats->droppedFrames++;
        }
    }
    
    stats->avgFrameTime /= count;
    stats->jitterAvg /= count;
    stats->totalFrames = count;
    
    // Calculate standard deviation
    float sumSquaredDiff = 0;
    for ( uint32_t i = 0; i < count; i++ ) {
        frameTiming_t *frame = &frameTimingState.frameHistory[i];
        float diff = frame->totalDuration - stats->avgFrameTime;
        sumSquaredDiff += diff * diff;
    }
    stats->stdDev = sqrtf( sumSquaredDiff / count );
    
    // Calculate percentiles
    float *sorted = alloca( sizeof( float ) * count );
    for ( uint32_t i = 0; i < count; i++ ) {
        sorted[i] = frameTimingState.frameHistory[i].totalDuration;
    }
    
    // Simple bubble sort
    for ( uint32_t i = 0; i < count - 1; i++ ) {
        for ( uint32_t j = 0; j < count - i - 1; j++ ) {
            if ( sorted[j] > sorted[j + 1] ) {
                float temp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = temp;
            }
        }
    }
    
    uint32_t idx95 = (uint32_t)( count * 0.95f );
    uint32_t idx99 = (uint32_t)( count * 0.99f );
    stats->percentile95 = sorted[Min( idx95, count - 1 )];
    stats->percentile99 = sorted[Min( idx99, count - 1 )];
    
    // Calculate phase statistics
    for ( int phase = 0; phase < FRAME_PHASE_CUSTOM; phase++ ) {
        timingStats_t *phaseStats = &frameTimingState.phaseStats[phase];
        
        phaseStats->avgFrameTime = 0;
        phaseStats->minFrameTime = FLT_MAX;
        phaseStats->maxFrameTime = -FLT_MAX;
        
        for ( uint32_t i = 0; i < count; i++ ) {
            frameTiming_t *frame = &frameTimingState.frameHistory[i];
            phaseTiming_t *phaseData = &frame->phases[phase];
            
            phaseStats->avgFrameTime += phaseData->duration;
            
            if ( phaseData->duration < phaseStats->minFrameTime ) {
                phaseStats->minFrameTime = phaseData->duration;
            }
            if ( phaseData->duration > phaseStats->maxFrameTime ) {
                phaseStats->maxFrameTime = phaseData->duration;
            }
        }
        
        phaseStats->avgFrameTime /= count;
    }
}

/*
===============
R_PredictFrameTiming

Predict next frame timing
===============
*/
void R_PredictFrameTiming( void ) {
    if ( !frameTimingState.initialized || frameTimingState.historyIndex < 5 ) {
        return;
    }
    
    // Simple exponential smoothing prediction
    frameTiming_t *currentFrame = &frameTimingState.currentFrame;
    float alpha = frameTimingState.smoothFactor;
    
    frameTimingState.predictedFrameTime = 
        frameTimingState.predictedFrameTime * ( 1.0f - alpha ) +
        currentFrame->totalDuration * alpha;
    
    // Adjust for trends
    if ( frameTimingState.historyIndex >= 10 ) {
        float trend = 0;
        for ( uint32_t i = 1; i < 5; i++ ) {
            uint32_t idx1 = ( frameTimingState.historyIndex - i + frameTimingState.historySize ) % 
                           frameTimingState.historySize;
            uint32_t idx2 = ( frameTimingState.historyIndex - i - 1 + frameTimingState.historySize ) % 
                           frameTimingState.historySize;
            trend += frameTimingState.frameHistory[idx1].totalDuration - 
                     frameTimingState.frameHistory[idx2].totalDuration;
        }
        trend /= 4.0f;
        
        frameTimingState.predictedFrameTime += trend * 0.5f;
    }
    
    // Clamp to reasonable bounds
    frameTimingState.predictedFrameTime = Clamp( frameTimingState.predictedFrameTime, 
                                                  frameTimingState.stats.minFrameTime,
                                                  frameTimingState.stats.maxFrameTime * 1.5f );
}

/*
===============
R_GetFrameTime

Get current frame time
===============
*/
float R_GetFrameTime( void ) {
    if ( !frameTimingState.initialized ) {
        return 0;
    }
    
    return frameTimingState.currentFrame.totalDuration;
}

/*
===============
R_GetPhaseTime

Get time for a specific phase
===============
*/
float R_GetPhaseTime( framePhase_t phase ) {
    if ( !frameTimingState.initialized || phase >= FRAME_PHASE_CUSTOM ) {
        return 0;
    }
    
    return frameTimingState.currentFrame.phases[phase].duration;
}

/*
===============
R_GetAverageFrameTime

Get average frame time
===============
*/
float R_GetAverageFrameTime( void ) {
    if ( !frameTimingState.initialized ) {
        return 0;
    }
    
    return frameTimingState.stats.avgFrameTime;
}

/*
===============
R_GetFrameTimeVariance

Get frame time variance
===============
*/
float R_GetFrameTimeVariance( void ) {
    if ( !frameTimingState.initialized ) {
        return 0;
    }
    
    return frameTimingState.stats.stdDev * frameTimingState.stats.stdDev;
}

/*
===============
R_GetFrameJitter

Get frame jitter
===============
*/
float R_GetFrameJitter( void ) {
    if ( !frameTimingState.initialized ) {
        return 0;
    }
    
    return frameTimingState.currentFrame.jitter;
}

/*
===============
R_PrintTimingBreakdown

Print timing breakdown to console
===============
*/
void R_PrintTimingBreakdown( void ) {
    if ( !frameTimingState.initialized ) {
        return;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    
    ri.Printf( PRINT_ALL, "===== Frame %llu Timing Breakdown =====\n", frame->frameNumber );
    ri.Printf( PRINT_ALL, "Total: %.2fms (CPU: %.2fms, GPU: %.2fms, Sync: %.2fms)\n",
              frame->totalDuration, frame->cpuDuration, frame->gpuDuration, frame->syncDuration );
    
    ri.Printf( PRINT_ALL, "\nPhase Breakdown:\n" );
    for ( int i = 0; i < FRAME_PHASE_CUSTOM; i++ ) {
        phaseTiming_t *phase = &frame->phases[i];
        if ( phase->duration > 0 ) {
            float percentage = ( phase->duration / frame->totalDuration ) * 100.0f;
            ri.Printf( PRINT_ALL, "  %-15s: %6.2fms (%5.1f%%) [avg: %.2fms]\n",
                      phase->name, phase->duration, percentage, phase->avgDuration );
            
            // Print sub-phases if detailed
            if ( frameTimingState.detailed ) {
                for ( uint32_t j = 0; j < phase->subPhaseCount; j++ ) {
                    phaseTiming_t *subPhase = phase->subPhases[j];
                    float subPercentage = ( subPhase->duration / frame->totalDuration ) * 100.0f;
                    ri.Printf( PRINT_ALL, "    %-13s: %6.2fms (%5.1f%%)\n",
                              subPhase->name, subPhase->duration, subPercentage );
                }
            }
        }
    }
    
    ri.Printf( PRINT_ALL, "\nStatistics:\n" );
    ri.Printf( PRINT_ALL, "  Average: %.2fms\n", frameTimingState.stats.avgFrameTime );
    ri.Printf( PRINT_ALL, "  Min/Max: %.2fms / %.2fms\n", 
              frameTimingState.stats.minFrameTime, frameTimingState.stats.maxFrameTime );
    ri.Printf( PRINT_ALL, "  95th/99th: %.2fms / %.2fms\n",
              frameTimingState.stats.percentile95, frameTimingState.stats.percentile99 );
    ri.Printf( PRINT_ALL, "  Std Dev: %.2fms\n", frameTimingState.stats.stdDev );
    ri.Printf( PRINT_ALL, "  Jitter: %.2fms\n", frameTimingState.stats.jitterAvg );
    ri.Printf( PRINT_ALL, "  Dropped: %d / %d (%.1f%%)\n",
              frameTimingState.stats.droppedFrames, frameTimingState.stats.totalFrames,
              ( frameTimingState.stats.droppedFrames * 100.0f ) / frameTimingState.stats.totalFrames );
}

/*
===============
R_DrawTimingTimeline

Draw timing timeline visualization
===============
*/
void R_DrawTimingTimeline( void ) {
    if ( !frameTimingState.initialized || !frameTimingState.showTimeline ) {
        return;
    }
    
    float x = 10;
    float y = 400;
    float width = 800;
    float height = 20;
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    float scale = width / frameTimingState.targetFrameTime;
    
    // Draw background
    vec4_t bgColor = { 0.1f, 0.1f, 0.1f, 0.8f };
    R_DrawRect( x, y, width, height, bgColor );
    
    // Draw target line
    vec4_t targetColor = { 0.5f, 0.5f, 0.5f, 1.0f };
    R_DrawLine( x + width, y, x + width, y + height, targetColor );
    
    // Draw phases
    float currentX = x;
    for ( int i = 0; i < FRAME_PHASE_CUSTOM; i++ ) {
        phaseTiming_t *phase = &frame->phases[i];
        if ( phase->duration > 0 ) {
            float phaseWidth = phase->duration * scale;
            vec4_t color;
            VectorCopy4( phaseColors[i], color );
            R_DrawRect( currentX, y, phaseWidth, height, color );
            currentX += phaseWidth;
        }
    }
    
    // Draw overrun if any
    if ( frame->totalDuration > frameTimingState.targetFrameTime ) {
        vec4_t overrunColor = { 1.0f, 0.2f, 0.2f, 0.5f };
        float overrunWidth = ( frame->totalDuration - frameTimingState.targetFrameTime ) * scale;
        R_DrawRect( x + width, y, overrunWidth, height, overrunColor );
    }
    
    // Draw labels
    vec4_t textColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    char label[64];
    Com_sprintf( label, sizeof( label ), "Frame: %.2fms", frame->totalDuration );
    R_DrawString( x, y - 20, label, 1.0f, textColor );
    
    Com_sprintf( label, sizeof( label ), "Target: %.2fms", frameTimingState.targetFrameTime );
    R_DrawString( x + width - 100, y - 20, label, 1.0f, textColor );
}

/*
===============
R_GetPhaseName

Get name for a phase
===============
*/
const char* R_GetPhaseName( framePhase_t phase ) {
    if ( phase >= 0 && phase < FRAME_PHASE_CUSTOM ) {
        return phaseNames[phase];
    }
    return "Unknown";
}

/*
===============
R_GetPhaseColor

Get color for a phase
===============
*/
void R_GetPhaseColor( framePhase_t phase, vec4_t color ) {
    static vec4_t defaultColor = { 0.5f, 0.5f, 0.5f, 1.0f };
    
    if ( phase >= 0 && phase < FRAME_PHASE_CUSTOM ) {
        VectorCopy4(phaseColors[phase], color);
    } else {
        VectorCopy4(defaultColor, color);
    }
}

/*
===============
R_GetPhasePercentage

Get percentage of frame time for a phase
===============
*/
float R_GetPhasePercentage( framePhase_t phase ) {
    if ( !frameTimingState.initialized || phase >= FRAME_PHASE_CUSTOM ) {
        return 0;
    }
    
    frameTiming_t *frame = &frameTimingState.currentFrame;
    if ( frame->totalDuration <= 0 ) {
        return 0;
    }
    
    return ( frame->phases[phase].duration / frame->totalDuration ) * 100.0f;
}