/*
===========================================================================
Copyright (C) 2026 Quake3e-HD Project

Volumetric weapon effects.

The cgame VM renders weapon explosions and smoke as animated sprites and
flash models. When the RTX backend is active those entities are intercepted
here (classified by their custom shader name), tracked as persistent volume
instances, and uploaded to the path tracer, which ray-marches them as
participating media: emissive fireballs that cool into smoke, and lit smoke
that rises and billows through the environment. Instances outlive their
source entities so smoke lingers and dissipates naturally instead of
popping off with the sprite.
===========================================================================
*/

#include "rt_volumefx.h"
#include "rt_pathtracer.h"

// Per-class behavior. Durations are authoritative here (driven by the
// entity's shaderTime), not by how long cgame keeps the sprite alive.
typedef struct {
    const char *shaderName;
    int         classId;
    float       fireDuration;   // seconds of emissive fireball phase
    float       lingerDuration; // seconds of smoke dissipation after the fire
    float       radiusScale;    // world radius = entity radius * scale (or fallback)
    float       fallbackRadius; // used when the entity has no radius (flash models)
    vec3_t      emission;       // fireball tint (linear); smoke classes: albedo tint
} rtVolumeFxDef_t;

static const rtVolumeFxDef_t rtVolumeFxDefs[] = {
    { "rocketExplosion",    RTVFX_CLASS_EXPLOSION, 0.90f, 2.4f, 1.35f, 42.0f, { 1.00f, 0.52f, 0.18f } },
    { "grenadeExplosion",   RTVFX_CLASS_EXPLOSION, 0.90f, 2.4f, 1.35f, 42.0f, { 1.00f, 0.52f, 0.18f } },
    { "bulletExplosion",    RTVFX_CLASS_IMPACT,    0.30f, 0.7f, 1.00f, 14.0f, { 1.00f, 0.62f, 0.28f } },
    { "lightningExplosion", RTVFX_CLASS_IMPACT,    0.30f, 0.4f, 1.00f, 16.0f, { 0.75f, 0.82f, 1.00f } },
    { "plasmaExplosion",    RTVFX_CLASS_PLASMA,    0.45f, 0.6f, 1.00f, 24.0f, { 0.35f, 0.55f, 1.00f } },
    { "railExplosion",      RTVFX_CLASS_RAIL,      0.50f, 0.6f, 1.00f, 24.0f, { 0.55f, 0.75f, 1.00f } },
    { "bfgExplosion",       RTVFX_CLASS_BFG,       0.80f, 1.2f, 1.20f, 48.0f, { 0.35f, 1.00f, 0.40f } },
    { "smokePuff",          RTVFX_CLASS_SMOKE,     0.00f, 2.6f, 1.15f, 24.0f, { 0.16f, 0.15f, 0.145f } },
    { "shotgunSmokePuff",   RTVFX_CLASS_GUNSMOKE,  0.00f, 1.2f, 1.00f, 16.0f, { 0.13f, 0.125f, 0.12f } },
};

#define RT_NUM_VOLUME_FX_DEFS ( (int)ARRAY_LEN( rtVolumeFxDefs ) )

// Shader-handle -> class cache. Handles are stable for the life of a level;
// the cache resets with the level (RT_VolumeFX_Clear on world load).
#define RTVFX_SHADER_CACHE_SIZE 128

typedef struct {
    qhandle_t handle;
    int       defIndex;     // -1 = not an effect shader
} rtVolumeFxShaderEntry_t;

typedef struct {
    qboolean  active;
    int       defIndex;
    float     shaderTime;     // spawn identity from the source entity
    vec3_t    origin;         // latest position from cgame (moving smoke trails)
    vec3_t    spawnOrigin;
    float     radius;         // largest radius seen (sprites animate it)
    float     seed;
    float     lastSeenTime;   // renderer time an entity last fed this instance
    float     birthTime;      // renderer time this instance appeared
} rtVolumeFxInstance_t;

static rtVolumeFxShaderEntry_t rtVfxShaderCache[RTVFX_SHADER_CACHE_SIZE];
static int  rtVfxShaderCacheCount = 0;
static rtVolumeFxInstance_t rtVfxInstances[RT_MAX_VOLUME_FX];
static int  rtVfxActiveCount = 0;

cvar_t *rt_volumetricFX;

/*
================
RT_VolumeFX_ClassifyShader

Resolve a custom shader handle to an effect definition index (-1 = none),
with a per-handle cache so the strcmp only ever runs once per shader.
================
*/
static int RT_VolumeFX_ClassifyShader( qhandle_t hShader ) {
    if ( hShader <= 0 ) {
        return -1;
    }

    for ( int i = 0; i < rtVfxShaderCacheCount; i++ ) {
        if ( rtVfxShaderCache[i].handle == hShader ) {
            return rtVfxShaderCache[i].defIndex;
        }
    }

    const shader_t *shader = R_GetShaderByHandle( hShader );
    int defIndex = -1;
    if ( shader ) {
        for ( int i = 0; i < RT_NUM_VOLUME_FX_DEFS; i++ ) {
            if ( !Q_stricmp( shader->name, rtVolumeFxDefs[i].shaderName ) ) {
                defIndex = i;
                break;
            }
        }
    }

    if ( rtVfxShaderCacheCount < RTVFX_SHADER_CACHE_SIZE ) {
        rtVfxShaderCache[rtVfxShaderCacheCount].handle = hShader;
        rtVfxShaderCache[rtVfxShaderCacheCount].defIndex = defIndex;
        rtVfxShaderCacheCount++;
    }
    return defIndex;
}

/*
================
RT_VolumeFX_Now

Renderer time in seconds, matching refEntity shaderTime units.
================
*/
static float RT_VolumeFX_Now( void ) {
    return tr.refdef.floatTime;
}

/*
================
RT_VolumeFX_InterceptEntity

Called from the entity-add path. Returns qtrue when the entity was consumed
as a volume effect and must not be rasterized. Instances are matched by
spawn identity (shaderTime + class + proximity) so a sprite feeding the
same explosion across frames updates one instance instead of spawning new
ones.
================
*/
qboolean RT_VolumeFX_InterceptEntity( const trRefEntity_t *ent ) {
    if ( !rt_volumetricFX || !rt_volumetricFX->integer ) {
        return qfalse;
    }
    if ( !RT_VolumeFX_Active() ) {
        return qfalse;
    }

    const refEntity_t *e = &ent->e;
    int defIndex = RT_VolumeFX_ClassifyShader( e->customShader );
    if ( defIndex < 0 ) {
        return qfalse;
    }

    const rtVolumeFxDef_t *def = &rtVolumeFxDefs[defIndex];
    float now = RT_VolumeFX_Now();
    float shaderTime = ent->e.shaderTime.f;
    float radius = ( e->radius > 1.0f ) ? e->radius * def->radiusScale
                                        : def->fallbackRadius * def->radiusScale;

    // Update an existing instance for this spawn
    rtVolumeFxInstance_t *slot = NULL;
    for ( int i = 0; i < RT_MAX_VOLUME_FX; i++ ) {
        rtVolumeFxInstance_t *inst = &rtVfxInstances[i];
        if ( !inst->active || inst->defIndex != defIndex ) {
            continue;
        }
        if ( fabsf( inst->shaderTime - shaderTime ) < 0.002f &&
             Distance( inst->spawnOrigin, e->origin ) < 220.0f ) {
            VectorCopy( e->origin, inst->origin );
            if ( radius > inst->radius ) {
                inst->radius = radius;
            }
            inst->lastSeenTime = now;
            return qtrue;
        }
    }

    // New instance: reuse a free slot, else evict the oldest
    for ( int i = 0; i < RT_MAX_VOLUME_FX; i++ ) {
        if ( !rtVfxInstances[i].active ) {
            slot = &rtVfxInstances[i];
            break;
        }
    }
    if ( !slot ) {
        float oldest = 1e30f;
        for ( int i = 0; i < RT_MAX_VOLUME_FX; i++ ) {
            if ( rtVfxInstances[i].birthTime < oldest ) {
                oldest = rtVfxInstances[i].birthTime;
                slot = &rtVfxInstances[i];
            }
        }
    }

    Com_Memset( slot, 0, sizeof( *slot ) );
    slot->active = qtrue;
    slot->defIndex = defIndex;
    slot->shaderTime = shaderTime;
    VectorCopy( e->origin, slot->origin );
    VectorCopy( e->origin, slot->spawnOrigin );
    slot->radius = radius;
    // Deterministic per-instance seed from the spawn identity
    slot->seed = fabsf( sinf( shaderTime * 91.17f + e->origin[0] * 0.013f + e->origin[1] * 0.007f ) );
    slot->lastSeenTime = now;
    slot->birthTime = now;
    rtVfxActiveCount++;
    return qtrue;
}

/*
================
RT_VolumeFX_FrameUpdate

Expire instances whose full life (fire + linger past the spawn time) has
elapsed, or that stopped being fed long before their natural end (level
change, killed local entities).
================
*/
void RT_VolumeFX_FrameUpdate( void ) {
    float now = RT_VolumeFX_Now();
    rtVfxActiveCount = 0;

    for ( int i = 0; i < RT_MAX_VOLUME_FX; i++ ) {
        rtVolumeFxInstance_t *inst = &rtVfxInstances[i];
        if ( !inst->active ) {
            continue;
        }
        const rtVolumeFxDef_t *def = &rtVolumeFxDefs[inst->defIndex];
        float age = now - inst->shaderTime;
        float fullLife = def->fireDuration + def->lingerDuration;

        // A time jump backwards (map restart, demo rewind) invalidates ages
        if ( age < -0.25f || age > fullLife || now - inst->lastSeenTime > fullLife ) {
            inst->active = qfalse;
            continue;
        }
        rtVfxActiveCount++;
    }
}

/*
================
RT_VolumeFX_FillGpu

Write the active instances into the GPU-layout array; returns the count.
================
*/
int RT_VolumeFX_FillGpu( rtVolumeFxGpu_t *out, int maxCount ) {
    float now = RT_VolumeFX_Now();
    int count = 0;

    for ( int i = 0; i < RT_MAX_VOLUME_FX && count < maxCount; i++ ) {
        const rtVolumeFxInstance_t *inst = &rtVfxInstances[i];
        if ( !inst->active ) {
            continue;
        }
        const rtVolumeFxDef_t *def = &rtVolumeFxDefs[inst->defIndex];
        float age = now - inst->shaderTime;
        float fullLife = def->fireDuration + def->lingerDuration;
        if ( age < 0.0f || fullLife <= 0.0f ) {
            continue;
        }

        rtVolumeFxGpu_t *g = &out[count++];
        g->posRadius[0] = inst->origin[0];
        g->posRadius[1] = inst->origin[1];
        g->posRadius[2] = inst->origin[2];
        g->posRadius[3] = inst->radius;
        g->emission[0] = def->emission[0];
        g->emission[1] = def->emission[1];
        g->emission[2] = def->emission[2];
        g->emission[3] = (float)def->classId;
        g->params[0] = age;
        g->params[1] = age / fullLife;
        g->params[2] = inst->seed;
        // Fire/smoke boundary as a fraction of full life; 0 = pure smoke
        g->params[3] = def->fireDuration / fullLife;
    }
    return count;
}

/*
================
RT_VolumeFX_Clear

Level transitions: shader handles and times restart.
================
*/
void RT_VolumeFX_Clear( void ) {
    Com_Memset( rtVfxInstances, 0, sizeof( rtVfxInstances ) );
    rtVfxShaderCacheCount = 0;
    rtVfxActiveCount = 0;
}

/*
================
RT_VolumeFX_Active

True when the RTX backend is tracing frames, so interception only happens
when the volume will actually be rendered.
================
*/
qboolean RT_VolumeFX_Active( void ) {
    return RT_IsBackendActive();
}
