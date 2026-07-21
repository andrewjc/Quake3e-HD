/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

This file is part of Quake3e-HD.

Quake3e-HD is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
===========================================================================
*/
// tr_profile_utils.c - Profiling utility functions

#include "tr_profile.h"
#include "../tr_local.h"

/*
================
R_ResetFrameStats

Reset frame statistics
================
*/
void R_ResetFrameStats( void ) {
    // Stub implementation
}

/*
================
R_RegisterProfileThread

Register a thread for profiling
================
*/
void R_RegisterProfileThread( const char *name ) {
    // Stub implementation
    (void)name;
}

/*
================
R_UpdateThreadUtilization

Update thread utilization stats
================
*/
void R_UpdateThreadUtilization( void ) {
    // Stub implementation
}

/*
================
R_WriteProfileReport

Write profiling report to file
================
*/
void R_WriteProfileReport( const char *filename ) {
    // Stub implementation
    (void)filename;
}

/*
================
R_ExportStatReport

Export statistics report
================
*/
void R_ExportStatReport( const char *filename ) {
    // Stub implementation
    (void)filename;
}

/*
================
R_StopPerfRecording

Stop performance recording
================
*/
void R_StopPerfRecording( void ) {
    // Stub implementation
}

/*
================
R_DestroyMemoryPool

Destroy a memory pool
================
*/
void R_DestroyMemoryPool( void *pool ) {
    // Stub implementation
    (void)pool;
}