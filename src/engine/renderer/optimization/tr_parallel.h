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

#ifndef TR_PARALLEL_H
#define TR_PARALLEL_H

#include "../core/tr_local.h"
#include "../lighting/tr_light_dynamic.h"
#include "../shadows/tr_shadow_volume.h"

/*
================================================================================
Phase 10: Parallel Processing System

Multi-threaded shadow generation and light processing.
================================================================================
*/

#define MAX_PARALLEL_JOBS   16
#define MAX_WORKER_THREADS  8

// Job types
typedef enum {
    JOB_NONE,
    JOB_SHADOW_VOLUME,
    JOB_LIGHT_CULL,
    JOB_SURFACE_SORT,
    JOB_VISIBILITY_TEST
} jobType_t;

// Job status
typedef enum {
    JOB_PENDING,
    JOB_RUNNING,
    JOB_COMPLETE,
    JOB_FAILED
} jobStatus_t;

// Shadow generation job
typedef struct shadowJob_s {
    renderLight_t       *light;
    srfTriangles_t      *triangles;
    shadowVolume_t      *result;
    volatile jobStatus_t status;
} shadowJob_t;

// Light culling job
typedef struct cullJob_s {
    renderLight_t       *lights;
    int                 numLights;
    int                 startIndex;
    int                 endIndex;
    cplane_t            *frustum;
    volatile jobStatus_t status;
} cullJob_t;

// Generic job structure
typedef struct parallelJob_s {
    jobType_t           type;
    void                *data;
    void                (*function)(void *data);
    volatile jobStatus_t status;
    struct parallelJob_s *next;
} parallelJob_t;

// Worker thread data
typedef struct workerThread_s {
    int                 id;
    qboolean            active;
    qboolean            terminate;
    void                *threadHandle;
    parallelJob_t       *currentJob;
} workerThread_t;

// Parallel system state
typedef struct {
    qboolean            initialized;
    int                 numWorkers;
    workerThread_t      workers[MAX_WORKER_THREADS];
    
    // Job queue
    parallelJob_t       jobs[MAX_PARALLEL_JOBS];
    parallelJob_t       *freeJobs;
    parallelJob_t       *pendingJobs;
    parallelJob_t       *activeJobs;
    
    // Synchronization (platform-specific implementation required)
    void                *jobMutex;
    void                *jobCondition;
    
    // Statistics
    int                 totalJobs;
    int                 completedJobs;
    int                 failedJobs;
    float               avgJobTime;
} parallelSystem_t;

// Function declarations

// System management
void R_InitParallelSystem(void);
void R_ShutdownParallelSystem(void);

// Job submission
parallelJob_t* R_AddParallelJob(void (*function)(void*), void *data, jobType_t type);
void R_WaitForJob(parallelJob_t *job);
void R_WaitForAllJobs(void);

// Parallel operations
void R_ParallelShadowGeneration(void);
void R_ParallelLightCulling(void);
void R_ParallelSurfaceSorting(void);

// Worker functions
void R_GenerateShadowJob(void *data);
void R_CullLightsJob(void *data);
void R_SortSurfacesJob(void *data);

// Utilities
int R_GetNumProcessors(void);
void R_ParallelStatistics(void);

#endif // TR_PARALLEL_H