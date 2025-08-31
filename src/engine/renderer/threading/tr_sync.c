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

#include "../tr_local.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#endif

/*
================================================================================
Platform-specific synchronization primitives

These functions provide a cross-platform abstraction for mutexes and semaphores
used by the multi-threaded renderer.
================================================================================
*/

#ifdef _WIN32

// ============= Windows Implementation =============

/*
================
Sys_CreateMutex

Create a mutex for thread synchronization
================
*/
void* Sys_CreateMutex(void) {
    CRITICAL_SECTION *cs = (CRITICAL_SECTION*)Z_Malloc(sizeof(CRITICAL_SECTION));
    InitializeCriticalSection(cs);
    return cs;
}

/*
================
Sys_DestroyMutex

Destroy a mutex
================
*/
void Sys_DestroyMutex(void* mutex) {
    if (mutex) {
        CRITICAL_SECTION *cs = (CRITICAL_SECTION*)mutex;
        DeleteCriticalSection(cs);
        Z_Free(mutex);
    }
}

/*
================
Sys_LockMutex

Lock a mutex
================
*/
void Sys_LockMutex(void* mutex) {
    if (mutex) {
        CRITICAL_SECTION *cs = (CRITICAL_SECTION*)mutex;
        EnterCriticalSection(cs);
    }
}

/*
================
Sys_UnlockMutex

Unlock a mutex
================
*/
void Sys_UnlockMutex(void* mutex) {
    if (mutex) {
        CRITICAL_SECTION *cs = (CRITICAL_SECTION*)mutex;
        LeaveCriticalSection(cs);
    }
}

/*
================
Sys_CreateSemaphore

Create a semaphore for signaling between threads
================
*/
void* Sys_CreateSemaphore(void) {
    HANDLE sem = CreateSemaphore(
        NULL,       // Security attributes
        0,          // Initial count
        1,          // Maximum count
        NULL        // Name
    );
    return sem;
}

/*
================
Sys_DestroySemaphore

Destroy a semaphore
================
*/
void Sys_DestroySemaphore(void* sem) {
    if (sem) {
        CloseHandle((HANDLE)sem);
    }
}

/*
================
Sys_WaitSemaphore

Wait on a semaphore (blocks until signaled)
================
*/
void Sys_WaitSemaphore(void* sem) {
    if (sem) {
        WaitForSingleObject((HANDLE)sem, INFINITE);
    }
}

/*
================
Sys_SignalSemaphore

Signal a semaphore (wakes up one waiting thread)
================
*/
void Sys_SignalSemaphore(void* sem) {
    if (sem) {
        ReleaseSemaphore((HANDLE)sem, 1, NULL);
    }
}

#else

// ============= POSIX Implementation (Linux/Unix/macOS) =============

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
} posix_sem_t;

/*
================
Sys_CreateMutex

Create a mutex for thread synchronization
================
*/
void* Sys_CreateMutex(void) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)Z_Malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    
    if (pthread_mutex_init(mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        Z_Free(mutex);
        return NULL;
    }
    
    pthread_mutexattr_destroy(&attr);
    return mutex;
}

/*
================
Sys_DestroyMutex

Destroy a mutex
================
*/
void Sys_DestroyMutex(void* mutex) {
    if (mutex) {
        pthread_mutex_t *m = (pthread_mutex_t*)mutex;
        pthread_mutex_destroy(m);
        Z_Free(mutex);
    }
}

/*
================
Sys_LockMutex

Lock a mutex
================
*/
void Sys_LockMutex(void* mutex) {
    if (mutex) {
        pthread_mutex_t *m = (pthread_mutex_t*)mutex;
        pthread_mutex_lock(m);
    }
}

/*
================
Sys_UnlockMutex

Unlock a mutex
================
*/
void Sys_UnlockMutex(void* mutex) {
    if (mutex) {
        pthread_mutex_t *m = (pthread_mutex_t*)mutex;
        pthread_mutex_unlock(m);
    }
}

/*
================
Sys_CreateSemaphore

Create a semaphore for signaling between threads
Using condition variables on POSIX systems for better portability
================
*/
void* Sys_CreateSemaphore(void) {
    posix_sem_t *sem = (posix_sem_t*)Z_Malloc(sizeof(posix_sem_t));
    
    if (pthread_mutex_init(&sem->mutex, NULL) != 0) {
        Z_Free(sem);
        return NULL;
    }
    
    if (pthread_cond_init(&sem->cond, NULL) != 0) {
        pthread_mutex_destroy(&sem->mutex);
        Z_Free(sem);
        return NULL;
    }
    
    sem->count = 0;
    return sem;
}

/*
================
Sys_DestroySemaphore

Destroy a semaphore
================
*/
void Sys_DestroySemaphore(void* semaphore) {
    if (semaphore) {
        posix_sem_t *sem = (posix_sem_t*)semaphore;
        pthread_cond_destroy(&sem->cond);
        pthread_mutex_destroy(&sem->mutex);
        Z_Free(semaphore);
    }
}

/*
================
Sys_WaitSemaphore

Wait on a semaphore (blocks until signaled)
================
*/
void Sys_WaitSemaphore(void* semaphore) {
    if (semaphore) {
        posix_sem_t *sem = (posix_sem_t*)semaphore;
        
        pthread_mutex_lock(&sem->mutex);
        while (sem->count <= 0) {
            pthread_cond_wait(&sem->cond, &sem->mutex);
        }
        sem->count--;
        pthread_mutex_unlock(&sem->mutex);
    }
}

/*
================
Sys_SignalSemaphore

Signal a semaphore (wakes up one waiting thread)
================
*/
void Sys_SignalSemaphore(void* semaphore) {
    if (semaphore) {
        posix_sem_t *sem = (posix_sem_t*)semaphore;
        
        pthread_mutex_lock(&sem->mutex);
        sem->count++;
        pthread_cond_signal(&sem->cond);
        pthread_mutex_unlock(&sem->mutex);
    }
}

#endif // _WIN32

/*
================
Sys_GetProcessorCount

Get the number of processor cores available
================
*/
int Sys_GetProcessorCount(void) {
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return sysInfo.dwNumberOfProcessors;
#else
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs < 1) {
        return 1;
    }
    return (int)nprocs;
#endif
}

/*
================
Sys_Sleep

Sleep for specified milliseconds
================
*/
#ifndef _WIN32
void Sys_Sleep(int msec) {
    usleep(msec * 1000);
}
#endif

/*
================
Sys_Yield

Yield CPU to other threads
================
*/
void Sys_Yield(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}