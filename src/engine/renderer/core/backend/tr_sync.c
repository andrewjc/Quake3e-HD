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
// tr_sync.c - Platform-specific synchronization primitives

#include "../tr_local.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif
#endif

/*
================
Mutex Implementation
================
*/

#ifdef _WIN32

void* Sys_CreateMutex(void) {
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*)Z_Malloc(sizeof(CRITICAL_SECTION));
    InitializeCriticalSection(cs);
    return cs;
}

void Sys_DestroyMutex(void* mutex) {
    if (mutex) {
        DeleteCriticalSection((CRITICAL_SECTION*)mutex);
        Z_Free(mutex);
    }
}

void Sys_LockMutex(void* mutex) {
    if (mutex) {
        EnterCriticalSection((CRITICAL_SECTION*)mutex);
    }
}

void Sys_UnlockMutex(void* mutex) {
    if (mutex) {
        LeaveCriticalSection((CRITICAL_SECTION*)mutex);
    }
}

qboolean Sys_TryLockMutex(void* mutex) {
    if (mutex) {
        return TryEnterCriticalSection((CRITICAL_SECTION*)mutex) != 0;
    }
    return qfalse;
}

#elif defined(__linux__) || defined(__APPLE__)

void* Sys_CreateMutex(void) {
    pthread_mutex_t* mutex = (pthread_mutex_t*)Z_Malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    
    if (pthread_mutex_init(mutex, &attr) != 0) {
        Z_Free(mutex);
        mutex = NULL;
    }
    
    pthread_mutexattr_destroy(&attr);
    return mutex;
}

void Sys_DestroyMutex(void* mutex) {
    if (mutex) {
        pthread_mutex_destroy((pthread_mutex_t*)mutex);
        Z_Free(mutex);
    }
}

void Sys_LockMutex(void* mutex) {
    if (mutex) {
        pthread_mutex_lock((pthread_mutex_t*)mutex);
    }
}

void Sys_UnlockMutex(void* mutex) {
    if (mutex) {
        pthread_mutex_unlock((pthread_mutex_t*)mutex);
    }
}

qboolean Sys_TryLockMutex(void* mutex) {
    if (mutex) {
        return pthread_mutex_trylock((pthread_mutex_t*)mutex) == 0;
    }
    return qfalse;
}

#endif

/*
================
Semaphore Implementation
================
*/

#ifdef _WIN32

typedef struct {
    HANDLE handle;
} win32_semaphore_t;

void* Sys_CreateSemaphore(void) {
    win32_semaphore_t* sem = (win32_semaphore_t*)Z_Malloc(sizeof(win32_semaphore_t));
    
    // Create with initial count 0, max count high
    sem->handle = CreateSemaphore(NULL, 0, 32767, NULL);
    
    if (!sem->handle) {
        Z_Free(sem);
        return NULL;
    }
    
    return sem;
}

void Sys_DestroySemaphore(void* sem) {
    if (sem) {
        win32_semaphore_t* s = (win32_semaphore_t*)sem;
        if (s->handle) {
            CloseHandle(s->handle);
        }
        Z_Free(sem);
    }
}

void Sys_WaitSemaphore(void* sem) {
    if (sem) {
        win32_semaphore_t* s = (win32_semaphore_t*)sem;
        WaitForSingleObject(s->handle, INFINITE);
    }
}

qboolean Sys_TryWaitSemaphore(void* sem) {
    if (sem) {
        win32_semaphore_t* s = (win32_semaphore_t*)sem;
        return WaitForSingleObject(s->handle, 0) == WAIT_OBJECT_0;
    }
    return qfalse;
}

void Sys_SignalSemaphore(void* sem) {
    if (sem) {
        win32_semaphore_t* s = (win32_semaphore_t*)sem;
        ReleaseSemaphore(s->handle, 1, NULL);
    }
}

#elif defined(__APPLE__)

// macOS doesn't have POSIX semaphores, use GCD dispatch semaphores
typedef struct {
    dispatch_semaphore_t sem;
} macos_semaphore_t;

void* Sys_CreateSemaphore(void) {
    macos_semaphore_t* sem = (macos_semaphore_t*)Z_Malloc(sizeof(macos_semaphore_t));
    
    sem->sem = dispatch_semaphore_create(0);
    if (!sem->sem) {
        Z_Free(sem);
        return NULL;
    }
    
    return sem;
}

void Sys_DestroySemaphore(void* sem) {
    if (sem) {
        macos_semaphore_t* s = (macos_semaphore_t*)sem;
        if (s->sem) {
            dispatch_release(s->sem);
        }
        Z_Free(sem);
    }
}

void Sys_WaitSemaphore(void* sem) {
    if (sem) {
        macos_semaphore_t* s = (macos_semaphore_t*)sem;
        dispatch_semaphore_wait(s->sem, DISPATCH_TIME_FOREVER);
    }
}

qboolean Sys_TryWaitSemaphore(void* sem) {
    if (sem) {
        macos_semaphore_t* s = (macos_semaphore_t*)sem;
        return dispatch_semaphore_wait(s->sem, DISPATCH_TIME_NOW) == 0;
    }
    return qfalse;
}

void Sys_SignalSemaphore(void* sem) {
    if (sem) {
        macos_semaphore_t* s = (macos_semaphore_t*)sem;
        dispatch_semaphore_signal(s->sem);
    }
}

#elif defined(__linux__)

typedef struct {
    sem_t sem;
    qboolean initialized;
} linux_semaphore_t;

void* Sys_CreateSemaphore(void) {
    linux_semaphore_t* sem = (linux_semaphore_t*)Z_Malloc(sizeof(linux_semaphore_t));
    
    if (sem_init(&sem->sem, 0, 0) != 0) {
        Z_Free(sem);
        return NULL;
    }
    
    sem->initialized = qtrue;
    return sem;
}

void Sys_DestroySemaphore(void* sem) {
    if (sem) {
        linux_semaphore_t* s = (linux_semaphore_t*)sem;
        if (s->initialized) {
            sem_destroy(&s->sem);
        }
        Z_Free(sem);
    }
}

void Sys_WaitSemaphore(void* sem) {
    if (sem) {
        linux_semaphore_t* s = (linux_semaphore_t*)sem;
        int result;
        
        // Handle interrupts
        do {
            result = sem_wait(&s->sem);
        } while (result == -1 && errno == EINTR);
    }
}

qboolean Sys_TryWaitSemaphore(void* sem) {
    if (sem) {
        linux_semaphore_t* s = (linux_semaphore_t*)sem;
        return sem_trywait(&s->sem) == 0;
    }
    return qfalse;
}

void Sys_SignalSemaphore(void* sem) {
    if (sem) {
        linux_semaphore_t* s = (linux_semaphore_t*)sem;
        sem_post(&s->sem);
    }
}

#endif

/*
================
Atomic Operations
================
*/

#ifdef _WIN32

int Sys_AtomicAdd(volatile int* value, int add) {
    return InterlockedExchangeAdd((volatile LONG*)value, add) + add;
}

int Sys_AtomicCompareExchange(volatile int* dest, int exchange, int compare) {
    return InterlockedCompareExchange((volatile LONG*)dest, exchange, compare);
}

void* Sys_AtomicCompareExchangePointer(void* volatile* dest, void* exchange, void* compare) {
    return InterlockedCompareExchangePointer(dest, exchange, compare);
}

#elif defined(__GNUC__) || defined(__clang__)

int Sys_AtomicAdd(volatile int* value, int add) {
    return __sync_add_and_fetch(value, add);
}

int Sys_AtomicCompareExchange(volatile int* dest, int exchange, int compare) {
    return __sync_val_compare_and_swap(dest, compare, exchange);
}

void* Sys_AtomicCompareExchangePointer(void* volatile* dest, void* exchange, void* compare) {
    return __sync_val_compare_and_swap(dest, compare, exchange);
}

#endif

/*
================
Memory Barriers
================
*/

void Sys_MemoryBarrier(void) {
#ifdef _WIN32
    MemoryBarrier();
#elif defined(__GNUC__) || defined(__clang__)
    __sync_synchronize();
#endif
}

void Sys_ReadBarrier(void) {
#ifdef _WIN32
    _ReadBarrier();
#elif defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : : "memory");
#endif
}

void Sys_WriteBarrier(void) {
#ifdef _WIN32
    _WriteBarrier();
#elif defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : : "memory");
#endif
}

/*
================
Thread Utilities
================
*/

int Sys_GetProcessorCount(void) {
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return sysInfo.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    return sysconf(_SC_NPROCESSORS_ONLN);
#else
    return 1;  // Fallback
#endif
}

// Platform functions Sys_Sleep and Sys_Yield are provided by win_main.c/unix_main.c