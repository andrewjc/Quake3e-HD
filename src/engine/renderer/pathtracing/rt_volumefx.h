/*
===========================================================================
Copyright (C) 2026 Quake3e-HD Project

Volumetric weapon effects: explosion fireballs and smoke rendered as
participating media by the path tracer instead of rasterized sprites.
===========================================================================
*/

#ifndef RT_VOLUMEFX_H
#define RT_VOLUMEFX_H

#include "../core/tr_local.h"

// Mirrored by the VolumeFX UBO in raygen.rgen
#define RT_MAX_VOLUME_FX 64

// Class ids shared with the shader (emission.w)
#define RTVFX_CLASS_NONE      0
#define RTVFX_CLASS_EXPLOSION 1   // rocket / grenade fireball -> smoke
#define RTVFX_CLASS_IMPACT    2   // bullet / lightning impact burst
#define RTVFX_CLASS_PLASMA    3   // plasma bolt burst
#define RTVFX_CLASS_RAIL      4   // rail slug impact burst
#define RTVFX_CLASS_BFG       5   // bfg burst
#define RTVFX_CLASS_SMOKE     6   // trail / gun smoke puff
#define RTVFX_CLASS_GUNSMOKE  7   // shotgun muzzle smoke (thin, quick)

// std140-compatible instance layout uploaded to the GPU
typedef struct rtVolumeFxGpu_s {
    float posRadius[4];   // xyz = world center, w = current radius
    float emission[4];    // rgb = emission / smoke tint, w = class id
    float params[4];      // x = age seconds, y = life fraction 0..1 (with linger), z = seed, w = fire/smoke boundary fraction
} rtVolumeFxGpu_t;

qboolean RT_VolumeFX_InterceptEntity(const trRefEntity_t *ent);
void     RT_VolumeFX_FrameUpdate(void);
int      RT_VolumeFX_FillGpu(rtVolumeFxGpu_t *out, int maxCount);
void     RT_VolumeFX_Clear(void);
qboolean RT_VolumeFX_Active(void);

#endif // RT_VOLUMEFX_H
