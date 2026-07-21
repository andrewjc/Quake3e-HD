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
// tr_stats.c - Render Statistics Tracking System Implementation

#include "tr_stats.h"
#include "../tr_local.h"
#include "../tr_common_utils.h"
#include <math.h>
#include <stdlib.h>
#include <float.h>

// Helper macros
#ifndef Min
#define Min(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef Max
#define Max(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef VectorCopy4
#define VectorCopy4(a,b) ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2],(b)[3]=(a)[3])
#endif

// Platform-specific alloca
#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif

// Forward declarations
static void R_CreateDefaultMetrics( void );
static void R_UpdateMetricAggregates( statMetric_t *metric );

// Global statistics state
statsState_t renderStats;

// Category names
static const char *statCategoryNames[] = {
    "Rendering",
    "Geometry",
    "Shading",
    "Textures",
    "Lighting",
    "Shadows",
    "Post-Process",
    "Culling",
    "Memory",
    "Bandwidth",
    "Cache",
    "Pipeline",
    "Compute",
    "Transfer",
    "Synchronization",
    "Custom"
};

// Metric type names
static const char *statMetricTypeNames[] = {
    "Count",
    "Size",
    "Time",
    "Percentage",
    "Rate",
    "Throughput",
    "Bandwidth",
    "Ratio",
    "Average",
    "Accumulator"
};

/*
===============
R_InitRenderStats

Initialize the render statistics system
===============
*/
qboolean R_InitRenderStats( void ) {
    ri.Printf( PRINT_ALL, "Initializing render statistics system...\n" );
    
    Com_Memset( &renderStats, 0, sizeof( renderStats ) );
    
    // Allocate frame history
    renderStats.frameHistorySize = STATS_HISTORY_SIZE;
    renderStats.frameHistory = ri.Malloc( sizeof( frameStats_t ) * renderStats.frameHistorySize );
    Com_Memset( renderStats.frameHistory, 0, sizeof( frameStats_t ) * renderStats.frameHistorySize );
    
    // Initialize categories
    for ( int i = 0; i < STAT_CAT_CUSTOM; i++ ) {
        statCategory_s *category = &renderStats.categories[i];
        Q_strncpyz( category->name, statCategoryNames[i], STATS_MAX_NAME_LENGTH );
        category->type = (statCategory_t)i;
        category->enabled = qtrue;
        category->expanded = qfalse;
        category->metricCount = 0;
    }
    renderStats.categoryCount = STAT_CAT_CUSTOM;
    
    // Create default metrics
    R_CreateDefaultMetrics();
    
    // Set default configuration
    renderStats.sampleInterval = STATS_SAMPLE_RATE;
    renderStats.autoReset = qtrue;
    renderStats.resetInterval = 3600;  // Reset every hour
    renderStats.showOverlay = qfalse;
    renderStats.showGraphs = qfalse;
    renderStats.overlayScale = 1.0f;
    renderStats.overlayAlpha = 0.8f;
    VectorSet2( renderStats.overlayPosition, 10, 100 );
    
    renderStats.initialized = qtrue;
    renderStats.enabled = qtrue;
    
    ri.Printf( PRINT_ALL, "Render statistics system initialized\n" );
    return qtrue;
}

/*
===============
R_ShutdownRenderStats

Shutdown the render statistics system
===============
*/
void R_ShutdownRenderStats( void ) {
    if ( !renderStats.initialized ) {
        return;
    }
    
    // Free metric history buffers
    for ( uint32_t i = 0; i < renderStats.metricCount; i++ ) {
        statMetric_t *metric = &renderStats.metrics[i];
        if ( metric->history ) {
            ri.Free( metric->history );
        }
    }
    
    // Free frame history
    if ( renderStats.frameHistory ) {
        ri.Free( renderStats.frameHistory );
    }
    
    Com_Memset( &renderStats, 0, sizeof( renderStats ) );
    ri.Printf( PRINT_ALL, "Render statistics system shutdown\n" );
}

/*
===============
R_ResetRenderStats

Reset all statistics
===============
*/
void R_ResetRenderStats( void ) {
    if ( !renderStats.initialized ) {
        return;
    }
    
    // Reset metrics
    for ( uint32_t i = 0; i < renderStats.metricCount; i++ ) {
        statMetric_t *metric = &renderStats.metrics[i];
        metric->current.value = 0;
        metric->min = FLT_MAX;
        metric->max = -FLT_MAX;
        metric->avg = 0;
        metric->sum = 0;
        metric->stddev = 0;
        metric->variance = 0;
        metric->historyIndex = 0;
        metric->historyCount = 0;
        
        if ( metric->history ) {
            Com_Memset( metric->history, 0, sizeof( float ) * STATS_HISTORY_SIZE );
        }
    }
    
    // Reset frame history
    Com_Memset( renderStats.frameHistory, 0, sizeof( frameStats_t ) * renderStats.frameHistorySize );
    renderStats.currentFrameIndex = 0;
    
    // Reset current frame
    Com_Memset( &renderStats.currentFrame, 0, sizeof( frameStats_t ) );
    Com_Memset( &renderStats.lastFrame, 0, sizeof( frameStats_t ) );
}

/*
===============
R_CreateDefaultMetrics

Create default metric definitions
===============
*/
static void R_CreateDefaultMetrics( void ) {
    // Rendering metrics
    R_CreateStatMetric( "Draw Calls", STAT_CAT_RENDERING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Draw Calls Saved", STAT_CAT_RENDERING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Instanced Draws", STAT_CAT_RENDERING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Indirect Draws", STAT_CAT_RENDERING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Render Passes", STAT_CAT_RENDERING, STAT_METRIC_COUNT );
    
    // Geometry metrics
    R_CreateStatMetric( "Triangles Submitted", STAT_CAT_GEOMETRY, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Triangles Rendered", STAT_CAT_GEOMETRY, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Vertices Submitted", STAT_CAT_GEOMETRY, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Vertices Processed", STAT_CAT_GEOMETRY, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Primitives Clipped", STAT_CAT_GEOMETRY, STAT_METRIC_COUNT );
    
    // Shading metrics
    R_CreateStatMetric( "Vertex Shader Invocations", STAT_CAT_SHADING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Fragment Shader Invocations", STAT_CAT_SHADING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Compute Shader Invocations", STAT_CAT_SHADING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Mesh Shader Invocations", STAT_CAT_SHADING, STAT_METRIC_COUNT );
    
    // Texture metrics
    R_CreateStatMetric( "Texture Binds", STAT_CAT_TEXTURES, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Texture Samples", STAT_CAT_TEXTURES, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Texture Bandwidth", STAT_CAT_TEXTURES, STAT_METRIC_BANDWIDTH );
    R_CreateStatMetric( "Texture Cache Misses", STAT_CAT_TEXTURES, STAT_METRIC_COUNT );
    
    // Memory metrics
    R_CreateStatMetric( "GPU Memory Used", STAT_CAT_MEMORY, STAT_METRIC_SIZE );
    R_CreateStatMetric( "GPU Memory Allocated", STAT_CAT_MEMORY, STAT_METRIC_SIZE );
    R_CreateStatMetric( "System Memory Used", STAT_CAT_MEMORY, STAT_METRIC_SIZE );
    R_CreateStatMetric( "Upload Bandwidth", STAT_CAT_MEMORY, STAT_METRIC_BANDWIDTH );
    R_CreateStatMetric( "Download Bandwidth", STAT_CAT_MEMORY, STAT_METRIC_BANDWIDTH );
    
    // Pipeline metrics
    R_CreateStatMetric( "Pipeline Changes", STAT_CAT_PIPELINE, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Descriptor Set Changes", STAT_CAT_PIPELINE, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Push Constant Updates", STAT_CAT_PIPELINE, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Pipeline Barriers", STAT_CAT_PIPELINE, STAT_METRIC_COUNT );
    
    // Culling metrics
    R_CreateStatMetric( "Objects Tested", STAT_CAT_CULLING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Objects Culled", STAT_CAT_CULLING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Frustum Culled", STAT_CAT_CULLING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Occlusion Culled", STAT_CAT_CULLING, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Backface Culled", STAT_CAT_CULLING, STAT_METRIC_COUNT );
    
    // Shadow metrics
    R_CreateStatMetric( "Shadow Casters", STAT_CAT_SHADOWS, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Shadow Maps", STAT_CAT_SHADOWS, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Shadow Samples", STAT_CAT_SHADOWS, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Shadow Memory", STAT_CAT_SHADOWS, STAT_METRIC_SIZE );
    
    // Post-processing metrics
    R_CreateStatMetric( "Post Effect Passes", STAT_CAT_POSTPROCESS, STAT_METRIC_COUNT );
    R_CreateStatMetric( "Post Effect Time", STAT_CAT_POSTPROCESS, STAT_METRIC_TIME );
    R_CreateStatMetric( "Fullscreen Passes", STAT_CAT_POSTPROCESS, STAT_METRIC_COUNT );
}

/*
===============
R_BeginFrameStats

Begin collecting statistics for a new frame
===============
*/
void R_BeginFrameStats( void ) {
    if ( !renderStats.initialized || !renderStats.enabled ) {
        return;
    }
    
    // Save last frame
    renderStats.lastFrame = renderStats.currentFrame;
    
    // Reset current frame
    Com_Memset( &renderStats.currentFrame, 0, sizeof( frameStats_t ) );
    renderStats.currentFrame.frameNumber = tr.frameCount;
    renderStats.currentFrame.timestamp = ri.Milliseconds();
    
    renderStats.collecting = qtrue;
}

/*
===============
R_EndFrameStats

End frame statistics collection
===============
*/
void R_EndFrameStats( void ) {
    if ( !renderStats.initialized || !renderStats.enabled || !renderStats.collecting ) {
        return;
    }
    
    renderStats.collecting = qfalse;
    
    // Sample at specified interval
    renderStats.framesSinceLastSample++;
    if ( renderStats.framesSinceLastSample >= renderStats.sampleInterval ) {
        R_CollectFrameStats();
        renderStats.framesSinceLastSample = 0;
    }
    
    // Auto-reset if enabled
    if ( renderStats.autoReset && 
         renderStats.currentFrame.frameNumber % renderStats.resetInterval == 0 ) {
        R_ResetRenderStats();
    }
    
    // Export if enabled
    if ( renderStats.exportEnabled && 
         renderStats.currentFrame.frameNumber - renderStats.lastExportFrame >= renderStats.exportInterval ) {
        R_ExportStatReport( renderStats.exportPath );
        renderStats.lastExportFrame = renderStats.currentFrame.frameNumber;
    }
}

/*
===============
R_CollectFrameStats

Collect and store frame statistics
===============
*/
void R_CollectFrameStats( void ) {
    if ( !renderStats.initialized ) {
        return;
    }
    
    // Store in history
    frameStats_t *frame = &renderStats.frameHistory[renderStats.currentFrameIndex];
    *frame = renderStats.currentFrame;
    
    renderStats.currentFrameIndex = ( renderStats.currentFrameIndex + 1 ) % renderStats.frameHistorySize;
    
    // Update metrics
    for ( uint32_t i = 0; i < renderStats.metricCount; i++ ) {
        statMetric_t *metric = &renderStats.metrics[i];
        
        // Add to history
        if ( metric->history ) {
            metric->history[metric->historyIndex] = metric->current.value;
            metric->historyIndex = ( metric->historyIndex + 1 ) % STATS_HISTORY_SIZE;
            if ( metric->historyCount < STATS_HISTORY_SIZE ) {
                metric->historyCount++;
            }
        }
        
        // Update aggregates
        R_UpdateMetricAggregates( metric );
    }
    
    // Aggregate frame stats
    R_AggregateStats();
}

/*
===============
R_CreateStatMetric

Create a new statistic metric
===============
*/
statMetric_t* R_CreateStatMetric( const char *name, statCategory_t category, statMetricType_t type ) {
    if ( !renderStats.initialized || renderStats.metricCount >= STATS_MAX_METRICS ) {
        return NULL;
    }
    
    statMetric_t *metric = &renderStats.metrics[renderStats.metricCount++];
    Com_Memset( metric, 0, sizeof( statMetric_t ) );
    
    Q_strncpyz( metric->name, name, STATS_MAX_NAME_LENGTH );
    metric->category = category;
    metric->type = type;
    metric->aggregation = STAT_AGG_AVERAGE;
    
    // Allocate history buffer
    metric->history = ri.Malloc( sizeof( float ) * STATS_HISTORY_SIZE );
    Com_Memset( metric->history, 0, sizeof( float ) * STATS_HISTORY_SIZE );
    
    // Set default display properties
    metric->visible = qtrue;
    metric->graphEnabled = qfalse;
    metric->scale = 1.0f;
    VectorSet4( metric->color, 1.0f, 1.0f, 1.0f, 1.0f );
    
    // Set default unit based on type
    switch ( type ) {
        case STAT_METRIC_SIZE:
            metric->unit = "bytes";
            break;
        case STAT_METRIC_TIME:
            metric->unit = "ms";
            break;
        case STAT_METRIC_PERCENTAGE:
            metric->unit = "%";
            break;
        case STAT_METRIC_BANDWIDTH:
            metric->unit = "MB/s";
            break;
        default:
            metric->unit = "";
            break;
    }
    
    // Initialize min/max
    metric->min = FLT_MAX;
    metric->max = -FLT_MAX;
    
    // Add to category
    if ( category < STAT_CAT_CUSTOM ) {
        statCategory_s *cat = &renderStats.categories[category];
        if ( cat->metricCount < STATS_MAX_METRICS ) {
            cat->metrics[cat->metricCount++] = metric;
        }
    }
    
    return metric;
}

/*
===============
R_UpdateMetricAggregates

Update metric aggregate values
===============
*/
static void R_UpdateMetricAggregates( statMetric_t *metric ) {
    if ( !metric || metric->historyCount == 0 ) {
        return;
    }
    
    float value = metric->current.value;
    
    // Update min/max
    if ( value < metric->min ) {
        metric->min = value;
    }
    if ( value > metric->max ) {
        metric->max = value;
    }
    
    // Calculate average
    metric->sum = 0;
    for ( uint32_t i = 0; i < metric->historyCount; i++ ) {
        metric->sum += metric->history[i];
    }
    metric->avg = metric->sum / metric->historyCount;
    
    // Calculate variance and standard deviation
    float sumSquaredDiff = 0;
    for ( uint32_t i = 0; i < metric->historyCount; i++ ) {
        float diff = metric->history[i] - metric->avg;
        sumSquaredDiff += diff * diff;
    }
    metric->variance = sumSquaredDiff / metric->historyCount;
    metric->stddev = sqrtf( metric->variance );
    
    // Calculate percentiles
    R_CalculateStatPercentiles( metric );
    
    // Update rate if applicable
    if ( metric->type == STAT_METRIC_RATE || 
         metric->type == STAT_METRIC_THROUGHPUT || 
         metric->type == STAT_METRIC_BANDWIDTH ) {
        uint64_t currentTime = ri.Milliseconds();
        float deltaTime = ( currentTime - metric->lastUpdateTime ) / 1000.0f;
        
        if ( deltaTime > 0 ) {
            metric->rate = value / deltaTime;
            metric->smoothedRate = metric->smoothedRate * 0.9f + metric->rate * 0.1f;
        }
        
        metric->lastUpdateTime = currentTime;
    }
    
    // Check thresholds
    if ( metric->hasThreshold ) {
        metric->isWarning = value >= metric->warningThreshold;
        metric->isCritical = value >= metric->criticalThreshold;
    }
}

/*
===============
R_UpdateStatMetric

Update a statistic metric value
===============
*/
void R_UpdateStatMetric( statMetric_t *metric, float value ) {
    if ( !metric ) {
        return;
    }
    
    metric->current.value = value;
}

/*
===============
R_IncrementStatMetric

Increment a statistic metric value
===============
*/
void R_IncrementStatMetric( statMetric_t *metric, float delta ) {
    if ( !metric ) {
        return;
    }
    
    metric->current.value += delta;
}

/*
===============
R_UpdateDrawCallStats

Update draw call statistics
===============
*/
void R_UpdateDrawCallStats( uint32_t draws, uint32_t saved ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.drawCalls += draws;
    renderStats.currentFrame.drawCallsSaved += saved;
}

/*
===============
R_UpdateGeometryStats

Update geometry statistics
===============
*/
void R_UpdateGeometryStats( uint32_t triangles, uint32_t vertices ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.trianglesSubmitted += triangles;
    renderStats.currentFrame.verticesSubmitted += vertices;
}

/*
===============
R_UpdateShaderStats

Update shader invocation statistics
===============
*/
void R_UpdateShaderStats( uint32_t vertex, uint32_t fragment, uint32_t compute ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.vertexShaderInvocations += vertex;
    renderStats.currentFrame.fragmentShaderInvocations += fragment;
    renderStats.currentFrame.computeShaderInvocations += compute;
}

/*
===============
R_UpdateTextureStats

Update texture statistics
===============
*/
void R_UpdateTextureStats( uint32_t binds, uint32_t samples, uint64_t bandwidth ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.textureBinds += binds;
    renderStats.currentFrame.textureSamples += samples;
    renderStats.currentFrame.textureBandwidth += bandwidth;
}

/*
===============
R_UpdateMemoryStats

Update memory statistics
===============
*/
void R_UpdateMemoryStats( uint64_t gpu, uint64_t system, uint64_t upload, uint64_t download ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.gpuMemoryUsed = gpu;
    renderStats.currentFrame.systemMemoryUsed = system;
    renderStats.currentFrame.uploadBandwidth += upload;
    renderStats.currentFrame.downloadBandwidth += download;
}

/*
===============
R_UpdatePipelineStats

Update pipeline statistics
===============
*/
void R_UpdatePipelineStats( uint32_t changes, uint32_t descriptors, uint32_t barriers ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.pipelineChanges += changes;
    renderStats.currentFrame.descriptorSetChanges += descriptors;
    renderStats.currentFrame.barrierCount += barriers;
}

/*
===============
R_UpdateCullingStats

Update culling statistics
===============
*/
void R_UpdateCullingStats( uint32_t tested, uint32_t culled, uint32_t frustum, uint32_t occlusion ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.objectsTested += tested;
    renderStats.currentFrame.objectsCulled += culled;
    renderStats.currentFrame.frustumCulled += frustum;
    renderStats.currentFrame.occlusionCulled += occlusion;
}

/*
===============
R_UpdateShadowStats

Update shadow rendering statistics
===============
*/
void R_UpdateShadowStats( uint32_t casters, uint32_t maps, uint64_t memory ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.shadowCasters += casters;
    renderStats.currentFrame.shadowMaps += maps;
    renderStats.currentFrame.shadowMemory += memory;
}

/*
===============
R_UpdatePostProcessStats

Update post-processing statistics
===============
*/
void R_UpdatePostProcessStats( uint32_t passes, float time ) {
    if ( !renderStats.collecting ) {
        return;
    }
    
    renderStats.currentFrame.postEffectPasses += passes;
    renderStats.currentFrame.postEffectTime += time;
}

/*
===============
R_AggregateStats

Aggregate statistics across frames
===============
*/
void R_AggregateStats( void ) {
    if ( !renderStats.initialized || renderStats.currentFrameIndex == 0 ) {
        return;
    }
    
    // Initialize aggregates
    Com_Memset( &renderStats.avgStats, 0, sizeof( frameStats_t ) );
    Com_Memset( &renderStats.minStats, 0xFF, sizeof( frameStats_t ) );
    Com_Memset( &renderStats.maxStats, 0, sizeof( frameStats_t ) );
    
    uint32_t count = Min( renderStats.currentFrameIndex, renderStats.frameHistorySize );
    
    for ( uint32_t i = 0; i < count; i++ ) {
        frameStats_t *frame = &renderStats.frameHistory[i];
        
        // Sum for average
        renderStats.avgStats.drawCalls += frame->drawCalls;
        renderStats.avgStats.trianglesSubmitted += frame->trianglesSubmitted;
        renderStats.avgStats.verticesSubmitted += frame->verticesSubmitted;
        renderStats.avgStats.fragmentShaderInvocations += frame->fragmentShaderInvocations;
        renderStats.avgStats.textureBinds += frame->textureBinds;
        renderStats.avgStats.pipelineChanges += frame->pipelineChanges;
        
        // Update min/max
        #define UPDATE_MIN_MAX(field) \
            if ( frame->field < renderStats.minStats.field ) renderStats.minStats.field = frame->field; \
            if ( frame->field > renderStats.maxStats.field ) renderStats.maxStats.field = frame->field;
        
        UPDATE_MIN_MAX( drawCalls );
        UPDATE_MIN_MAX( trianglesSubmitted );
        UPDATE_MIN_MAX( verticesSubmitted );
        UPDATE_MIN_MAX( fragmentShaderInvocations );
        UPDATE_MIN_MAX( textureBinds );
        UPDATE_MIN_MAX( pipelineChanges );
        
        #undef UPDATE_MIN_MAX
    }
    
    // Calculate averages
    if ( count > 0 ) {
        renderStats.avgStats.drawCalls /= count;
        renderStats.avgStats.trianglesSubmitted /= count;
        renderStats.avgStats.verticesSubmitted /= count;
        renderStats.avgStats.fragmentShaderInvocations /= count;
        renderStats.avgStats.textureBinds /= count;
        renderStats.avgStats.pipelineChanges /= count;
    }
}

/*
===============
R_CalculateStatPercentiles

Calculate percentile values for a metric
===============
*/
void R_CalculateStatPercentiles( statMetric_t *metric ) {
    if ( !metric || metric->historyCount == 0 ) {
        return;
    }
    
    // Create sorted copy of history
    float *sorted = alloca( sizeof( float ) * metric->historyCount );
    memcpy( sorted, metric->history, sizeof( float ) * metric->historyCount );
    
    // Simple bubble sort (fine for small datasets)
    for ( uint32_t i = 0; i < metric->historyCount - 1; i++ ) {
        for ( uint32_t j = 0; j < metric->historyCount - i - 1; j++ ) {
            if ( sorted[j] > sorted[j + 1] ) {
                float temp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = temp;
            }
        }
    }
    
    // Calculate percentiles
    uint32_t idx95 = (uint32_t)( metric->historyCount * 0.95f );
    uint32_t idx99 = (uint32_t)( metric->historyCount * 0.99f );
    
    metric->percentile95 = sorted[Min( idx95, metric->historyCount - 1 )];
    metric->percentile99 = sorted[Min( idx99, metric->historyCount - 1 )];
}

/*
===============
R_GetStatMetricValue

Get current value of a metric by name
===============
*/
float R_GetStatMetricValue( const char *name ) {
    if ( !renderStats.initialized ) {
        return 0;
    }
    
    for ( uint32_t i = 0; i < renderStats.metricCount; i++ ) {
        if ( Q_stricmp( renderStats.metrics[i].name, name ) == 0 ) {
            return renderStats.metrics[i].current.value;
        }
    }
    
    return 0;
}

/*
===============
R_GetRenderEfficiency

Calculate overall render efficiency
===============
*/
float R_GetRenderEfficiency( void ) {
    if ( !renderStats.initialized || renderStats.currentFrame.trianglesSubmitted == 0 ) {
        return 0;
    }
    
    float efficiency = 0;
    
    // Triangle efficiency (rendered vs submitted)
    if ( renderStats.currentFrame.trianglesSubmitted > 0 ) {
        efficiency += ( float )renderStats.currentFrame.trianglesRendered / 
                     ( float )renderStats.currentFrame.trianglesSubmitted;
    }
    
    // Draw call efficiency (saved vs total)
    if ( renderStats.currentFrame.drawCalls > 0 ) {
        efficiency += ( float )renderStats.currentFrame.drawCallsSaved / 
                     ( float )renderStats.currentFrame.drawCalls;
    }
    
    // Culling efficiency
    if ( renderStats.currentFrame.objectsTested > 0 ) {
        efficiency += ( float )renderStats.currentFrame.objectsCulled / 
                     ( float )renderStats.currentFrame.objectsTested;
    }
    
    return efficiency / 3.0f;  // Average of efficiencies
}

/*
===============
R_DrawStatOverlay

Draw statistics overlay on screen
===============
*/
void R_DrawStatOverlay( void ) {
    if ( !renderStats.initialized || !renderStats.showOverlay ) {
        return;
    }
    
    float x = renderStats.overlayPosition[0];
    float y = renderStats.overlayPosition[1];
    float scale = renderStats.overlayScale;
    vec4_t color;
    
    VectorSet4( color, 1.0f, 1.0f, 1.0f, renderStats.overlayAlpha );
    
    // Draw title
    R_DrawString( x, y, "Render Statistics", scale * 1.2f, color );
    y += 20 * scale;
    
    // Draw categories
    for ( uint32_t i = 0; i < renderStats.categoryCount; i++ ) {
        statCategory_s *category = &renderStats.categories[i];
        if ( !category->enabled ) {
            continue;
        }
        
        // Category header
        R_DrawString( x, y, category->name, scale, color );
        y += 15 * scale;
        
        if ( category->expanded ) {
            // Draw metrics
            for ( uint32_t j = 0; j < category->metricCount; j++ ) {
                statMetric_t *metric = category->metrics[j];
                if ( !metric->visible ) {
                    continue;
                }
                
                // Get color based on thresholds
                vec4_t metricColor;
                if ( metric->hasThreshold ) {
                    R_GetStatSeverityColor( metric->current.value, 
                                           metric->warningThreshold, 
                                           metric->criticalThreshold,
                                           metricColor );
                } else {
                    VectorCopy4( color, metricColor );
                }
                
                // Format value
                char valueStr[64];
                switch ( metric->type ) {
                    case STAT_METRIC_SIZE:
                        Com_sprintf( valueStr, sizeof( valueStr ), "%.2f MB", 
                                   metric->current.value / ( 1024.0f * 1024.0f ) );
                        break;
                    case STAT_METRIC_TIME:
                        Com_sprintf( valueStr, sizeof( valueStr ), "%.2f ms", 
                                   metric->current.value );
                        break;
                    case STAT_METRIC_PERCENTAGE:
                        Com_sprintf( valueStr, sizeof( valueStr ), "%.1f%%", 
                                   metric->current.value );
                        break;
                    case STAT_METRIC_BANDWIDTH:
                        Com_sprintf( valueStr, sizeof( valueStr ), "%.1f MB/s", 
                                   metric->current.value / ( 1024.0f * 1024.0f ) );
                        break;
                    default:
                        Com_sprintf( valueStr, sizeof( valueStr ), "%.0f", 
                                   metric->current.value );
                        break;
                }
                
                // Draw metric name and value
                char line[128];
                Com_sprintf( line, sizeof( line ), "  %s: %s", metric->name, valueStr );
                R_DrawString( x + 10 * scale, y, line, scale * 0.9f, metricColor );
                y += 12 * scale;
            }
        }
    }
    
    // Draw summary
    y += 10 * scale;
    R_DrawString( x, y, "Summary", scale * 1.1f, color );
    y += 15 * scale;
    
    char summary[256];
    Com_sprintf( summary, sizeof( summary ), 
                "Efficiency: %.1f%% | Draw Calls: %d | Triangles: %dk", 
                R_GetRenderEfficiency() * 100.0f,
                renderStats.currentFrame.drawCalls,
                renderStats.currentFrame.trianglesSubmitted / 1000 );
    R_DrawString( x, y, summary, scale, color );
}

/*
===============
R_GetStatSeverityColor

Get color based on value severity
===============
*/
void R_GetStatSeverityColor( float value, float warning, float critical, vec4_t result ) {
    if ( value >= critical ) {
        // Red
        VectorSet4( result, 1.0f, 0.2f, 0.2f, 1.0f );
    } else if ( value >= warning ) {
        // Yellow
        VectorSet4( result, 1.0f, 1.0f, 0.2f, 1.0f );
    } else {
        // Green
        VectorSet4( result, 0.2f, 1.0f, 0.2f, 1.0f );
    }
}

/*
===============
R_GenerateStatReport

Generate a statistics report
===============
*/
void R_GenerateStatReport( void ) {
    if ( !renderStats.initialized ) {
        return;
    }
    
    ri.Printf( PRINT_ALL, "========== Render Statistics Report ==========\n" );
    ri.Printf( PRINT_ALL, "Frame: %llu | Time: %llu ms\n", 
              renderStats.currentFrame.frameNumber,
              renderStats.currentFrame.timestamp );
    
    ri.Printf( PRINT_ALL, "\n--- Rendering ---\n" );
    ri.Printf( PRINT_ALL, "Draw Calls: %d (Saved: %d)\n", 
              renderStats.currentFrame.drawCalls,
              renderStats.currentFrame.drawCallsSaved );
    ri.Printf( PRINT_ALL, "Instanced Draws: %d\n", renderStats.currentFrame.instancedDraws );
    ri.Printf( PRINT_ALL, "Indirect Draws: %d\n", renderStats.currentFrame.indirectDraws );
    
    ri.Printf( PRINT_ALL, "\n--- Geometry ---\n" );
    ri.Printf( PRINT_ALL, "Triangles: %d submitted, %d rendered\n",
              renderStats.currentFrame.trianglesSubmitted,
              renderStats.currentFrame.trianglesRendered );
    ri.Printf( PRINT_ALL, "Vertices: %d submitted, %d processed\n",
              renderStats.currentFrame.verticesSubmitted,
              renderStats.currentFrame.verticesProcessed );
    
    ri.Printf( PRINT_ALL, "\n--- Shading ---\n" );
    ri.Printf( PRINT_ALL, "Vertex Invocations: %d\n", 
              renderStats.currentFrame.vertexShaderInvocations );
    ri.Printf( PRINT_ALL, "Fragment Invocations: %d\n", 
              renderStats.currentFrame.fragmentShaderInvocations );
    ri.Printf( PRINT_ALL, "Compute Invocations: %d\n", 
              renderStats.currentFrame.computeShaderInvocations );
    
    ri.Printf( PRINT_ALL, "\n--- Memory ---\n" );
    ri.Printf( PRINT_ALL, "GPU Memory: %.2f MB used\n", 
              renderStats.currentFrame.gpuMemoryUsed / ( 1024.0f * 1024.0f ) );
    ri.Printf( PRINT_ALL, "Upload Bandwidth: %.2f MB\n", 
              renderStats.currentFrame.uploadBandwidth / ( 1024.0f * 1024.0f ) );
    
    ri.Printf( PRINT_ALL, "\n--- Efficiency ---\n" );
    ri.Printf( PRINT_ALL, "Overall: %.1f%%\n", R_GetRenderEfficiency() * 100.0f );
    
    float cullRate = 0;
    if ( renderStats.currentFrame.objectsTested > 0 ) {
        cullRate = ( float )renderStats.currentFrame.objectsCulled / 
                  ( float )renderStats.currentFrame.objectsTested;
    }
    ri.Printf( PRINT_ALL, "Cull Rate: %.1f%%\n", cullRate * 100.0f );
    
    ri.Printf( PRINT_ALL, "===========================================\n" );
}