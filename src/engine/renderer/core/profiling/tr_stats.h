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
// tr_stats.h - Render Statistics Tracking System

#ifndef __TR_STATS_H
#define __TR_STATS_H

#include "../tr_local.h"

// Statistics configuration
#define STATS_MAX_CATEGORIES        32
#define STATS_MAX_METRICS          256
#define STATS_HISTORY_SIZE         600    // 10 seconds at 60 FPS
#define STATS_SAMPLE_RATE          16     // Sample every N frames
#define STATS_MAX_NAME_LENGTH      64

// Statistic categories
typedef enum {
    STAT_CAT_RENDERING = 0,
    STAT_CAT_GEOMETRY,
    STAT_CAT_SHADING,
    STAT_CAT_TEXTURES,
    STAT_CAT_LIGHTING,
    STAT_CAT_SHADOWS,
    STAT_CAT_POSTPROCESS,
    STAT_CAT_CULLING,
    STAT_CAT_MEMORY,
    STAT_CAT_BANDWIDTH,
    STAT_CAT_CACHE,
    STAT_CAT_PIPELINE,
    STAT_CAT_COMPUTE,
    STAT_CAT_TRANSFER,
    STAT_CAT_SYNCHRONIZATION,
    STAT_CAT_CUSTOM,
} statCategory_t;

// Metric types
typedef enum {
    STAT_METRIC_COUNT = 0,      // Simple counter
    STAT_METRIC_SIZE,           // Size in bytes
    STAT_METRIC_TIME,           // Time in microseconds
    STAT_METRIC_PERCENTAGE,     // Percentage (0-100)
    STAT_METRIC_RATE,           // Rate per second
    STAT_METRIC_THROUGHPUT,     // Throughput (units/sec)
    STAT_METRIC_BANDWIDTH,      // Bandwidth (bytes/sec)
    STAT_METRIC_RATIO,          // Ratio (numerator/denominator)
    STAT_METRIC_AVERAGE,        // Running average
    STAT_METRIC_ACCUMULATOR,    // Accumulating value
} statMetricType_t;

// Aggregation types
typedef enum {
    STAT_AGG_NONE = 0,
    STAT_AGG_SUM,
    STAT_AGG_AVERAGE,
    STAT_AGG_MIN,
    STAT_AGG_MAX,
    STAT_AGG_PERCENTILE,
    STAT_AGG_STDDEV,
    STAT_AGG_VARIANCE,
} statAggregationType_t;

// Statistic metric structure
typedef struct statMetric_s {
    char                    name[STATS_MAX_NAME_LENGTH];
    statCategory_t          category;
    statMetricType_t        type;
    statAggregationType_t   aggregation;
    
    // Current values
    union {
        uint64_t            count;
        float               value;
        double              dvalue;
    } current;
    
    // History buffer
    float                  *history;
    uint32_t                historyIndex;
    uint32_t                historyCount;
    
    // Aggregated values
    float                   min;
    float                   max;
    float                   avg;
    float                   sum;
    float                   stddev;
    float                   variance;
    float                   percentile95;
    float                   percentile99;
    
    // Rate calculation
    uint64_t                lastUpdateTime;
    float                   rate;
    float                   smoothedRate;
    
    // Thresholds and alerts
    float                   warningThreshold;
    float                   criticalThreshold;
    qboolean                hasThreshold;
    qboolean                isWarning;
    qboolean                isCritical;
    
    // Display properties
    qboolean                visible;
    qboolean                graphEnabled;
    vec4_t                  color;
    const char             *unit;
    float                   scale;
} statMetric_t;

// Category statistics
typedef struct statCategory_s {
    char                    name[STATS_MAX_NAME_LENGTH];
    statCategory_t          type;
    uint32_t                metricCount;
    statMetric_t           *metrics[STATS_MAX_METRICS];
    qboolean                enabled;
    qboolean                expanded;
} statCategory_s;

// Frame statistics snapshot
typedef struct frameStats_s {
    uint64_t                frameNumber;
    uint64_t                timestamp;
    
    // Rendering stats
    uint32_t                drawCalls;
    uint32_t                drawCallsSaved;
    uint32_t                instancedDraws;
    uint32_t                indirectDraws;
    uint32_t                computeDispatches;
    
    // Geometry stats
    uint32_t                trianglesSubmitted;
    uint32_t                trianglesRendered;
    uint32_t                verticesSubmitted;
    uint32_t                verticesProcessed;
    uint32_t                primitivesClipped;
    
    // Shading stats
    uint32_t                vertexShaderInvocations;
    uint32_t                fragmentShaderInvocations;
    uint32_t                computeShaderInvocations;
    uint32_t                meshShaderInvocations;
    uint32_t                taskShaderInvocations;
    
    // Texture stats
    uint32_t                textureBinds;
    uint32_t                textureSamples;
    uint64_t                textureBandwidth;
    uint32_t                textureCacheMisses;
    
    // Memory stats
    uint64_t                gpuMemoryUsed;
    uint64_t                gpuMemoryAllocated;
    uint64_t                systemMemoryUsed;
    uint64_t                uploadBandwidth;
    uint64_t                downloadBandwidth;
    
    // Pipeline stats
    uint32_t                pipelineChanges;
    uint32_t                descriptorSetChanges;
    uint32_t                pushConstantUpdates;
    uint32_t                barrierCount;
    uint32_t                renderPassCount;
    
    // Culling stats
    uint32_t                objectsTested;
    uint32_t                objectsCulled;
    uint32_t                frustumCulled;
    uint32_t                occlusionCulled;
    uint32_t                backfaceCulled;
    uint32_t                smallFeatureCulled;
    
    // Shadow stats
    uint32_t                shadowCasters;
    uint32_t                shadowMaps;
    uint32_t                shadowSamples;
    uint64_t                shadowMemory;
    
    // Post-processing stats
    uint32_t                postEffectPasses;
    float                   postEffectTime;
    uint32_t                fullscreenPasses;
} frameStats_t;

// Statistics state
typedef struct statsState_s {
    qboolean                initialized;
    qboolean                enabled;
    qboolean                collecting;
    
    // Metrics
    statMetric_t            metrics[STATS_MAX_METRICS];
    uint32_t                metricCount;
    
    // Categories
    statCategory_s          categories[STATS_MAX_CATEGORIES];
    uint32_t                categoryCount;
    
    // Frame history
    frameStats_t           *frameHistory;
    uint32_t                frameHistorySize;
    uint32_t                currentFrameIndex;
    
    // Current frame stats
    frameStats_t            currentFrame;
    frameStats_t            lastFrame;
    
    // Aggregated stats
    frameStats_t            avgStats;
    frameStats_t            minStats;
    frameStats_t            maxStats;
    
    // Collection control
    uint32_t                sampleInterval;
    uint32_t                framesSinceLastSample;
    qboolean                autoReset;
    uint32_t                resetInterval;
    
    // Display options
    qboolean                showOverlay;
    qboolean                showGraphs;
    qboolean                showDetails;
    vec2_t                  overlayPosition;
    float                   overlayScale;
    float                   overlayAlpha;
    
    // Export options
    qboolean                exportEnabled;
    const char             *exportPath;
    uint32_t                exportInterval;
    uint32_t                lastExportFrame;
} statsState_t;

// Global statistics state
extern statsState_t renderStats;

// Initialization
qboolean R_InitRenderStats( void );
void R_ShutdownRenderStats( void );
void R_ResetRenderStats( void );

// Frame management
void R_BeginFrameStats( void );
void R_EndFrameStats( void );
void R_CollectFrameStats( void );

// Metric management
statMetric_t* R_CreateStatMetric( const char *name, statCategory_t category, statMetricType_t type );
void R_DestroyStatMetric( statMetric_t *metric );
void R_UpdateStatMetric( statMetric_t *metric, float value );
void R_IncrementStatMetric( statMetric_t *metric, float delta );
void R_SetStatMetricThreshold( statMetric_t *metric, float warning, float critical );

// Category management
statCategory_s* R_CreateStatCategory( const char *name, statCategory_t type );
void R_AddMetricToCategory( statCategory_s *category, statMetric_t *metric );
void R_EnableStatCategory( statCategory_t category, qboolean enable );

// Statistic updates
void R_UpdateDrawCallStats( uint32_t draws, uint32_t saved );
void R_UpdateGeometryStats( uint32_t triangles, uint32_t vertices );
void R_UpdateShaderStats( uint32_t vertex, uint32_t fragment, uint32_t compute );
void R_UpdateTextureStats( uint32_t binds, uint32_t samples, uint64_t bandwidth );
void R_UpdateMemoryStats( uint64_t gpu, uint64_t system, uint64_t upload, uint64_t download );
void R_UpdatePipelineStats( uint32_t changes, uint32_t descriptors, uint32_t barriers );
void R_UpdateCullingStats( uint32_t tested, uint32_t culled, uint32_t frustum, uint32_t occlusion );
void R_UpdateShadowStats( uint32_t casters, uint32_t maps, uint64_t memory );
void R_UpdatePostProcessStats( uint32_t passes, float time );

// Aggregation and analysis
void R_AggregateStats( void );
void R_CalculateStatPercentiles( statMetric_t *metric );
void R_CalculateStatVariance( statMetric_t *metric );
void R_AnalyzeStatTrends( void );
void R_DetectStatAnomalies( void );

// Reporting
void R_GenerateStatReport( void );
void R_PrintStatReport( void );
void R_ExportStatReport( const char *filename );
void R_DrawStatOverlay( void );
void R_DrawStatGraphs( void );

// Queries
float R_GetStatMetricValue( const char *name );
float R_GetStatMetricAverage( const char *name );
float R_GetStatMetricPercentile( const char *name, float percentile );
frameStats_t* R_GetFrameStats( uint32_t frameOffset );
void R_GetAggregatedStats( frameStats_t *avg, frameStats_t *min, frameStats_t *max );

// Comparison
void R_CompareFrameStats( const frameStats_t *a, const frameStats_t *b, frameStats_t *diff );
float R_GetStatEfficiency( statCategory_t category );
float R_GetRenderEfficiency( void );

// Configuration
void R_SetStatCollectionEnabled( qboolean enable );
void R_SetStatSampleRate( uint32_t rate );
void R_SetStatHistorySize( uint32_t size );
void R_SetStatOverlay( qboolean show );
void R_SetStatExport( qboolean enable, const char *path );

// Helpers
const char* R_GetStatCategoryName( statCategory_t category );
const char* R_GetStatMetricTypeName( statMetricType_t type );
void R_GetStatSeverityColor( float value, float warning, float critical, vec4_t result );

#endif // __TR_STATS_H