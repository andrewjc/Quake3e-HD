#include <limits.h>
#include <float.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>

#include "../core/tr_local.h"
#include "rt_rtx.h"
#include "rt_volumefx.h"
#include "../lighting/tr_light_dynamic.h"
#ifdef USE_VULKAN
#include "../vulkan/vk.h"
#endif

#ifdef USE_VULKAN
static void RT_DestroySceneLightBuffer(void);
#endif
static void RT_Status_f(void);
static void RT_RebuildSceneLights(void);
static void RT_BuildLightGrid(void);
static void RT_DestroyLightGridHostData(void);
static void VectorLerp( const vec3_t from, const vec3_t to, float lerp, vec3_t out );
static void RT_ApplyStaticLightAutoScale(void);

#ifdef USE_VULKAN
typedef struct rtxLightGpuUpload_s {
	uint32_t numLights;
	rtxLightGpu_t lights[RT_MAX_SCENE_LIGHTS];
} rtxLightGpuUpload_t;
static void RT_DestroyLightGridBuffers(void);

// Scene light data staged on the CPU, recorded into the frame command buffer
// by RT_RecordSceneLightUpload so the transfer is ordered with the dispatch
// that reads it (host-mapped writes would race frames in flight).
static rtxLightGpuUpload_t rtPendingLightUpload;
static size_t rtPendingLightBytes = 0;
static qboolean rtLightUploadPending = qfalse;
#endif

pathTracer_t rt;

// Demand-driven probe refresh: probes update only while consumers sample
// the grid (see RT_SampleProbeGrid / RT_RenderPathTracedLighting).
#define RT_PROBE_DEMAND_WINDOW 120
static int rtProbeLastSampleFrame = -RT_PROBE_DEMAND_WINDOW;

static vec3_t rtSkyDirectionAccum = { 0.0f, 0.0f, 0.0f };
static vec3_t rtSkyColorAccum = { 0.0f, 0.0f, 0.0f };
static float rtSkyWeightAccum = 0.0f;
cvar_t *rt_enable;
cvar_t *rt_mode;
cvar_t *rt_quality;
cvar_t *rt_bounces;
cvar_t *rt_samples;
cvar_t *rt_denoise;
cvar_t *rt_temporal;
cvar_t *r_rt_mode;
cvar_t *r_rt_backend;
cvar_t *rt_probes;
cvar_t *rt_cache;
cvar_t *rt_debug;
cvar_t *rt_staticLights;
cvar_t *rt_gpuValidate;
cvar_t *rt_staticLightScale;
cvar_t *rt_staticLightRadiusScale;
cvar_t *rt_lightGridCellSize;
cvar_t *rt_skyLightScale;
cvar_t *rt_staticLightAutoScale;
cvar_t *rt_staticLightAutoTarget;
cvar_t *rt_skyAmbientFactor;
cvar_t *rt_exposure;
cvar_t *rt_dlightIntensity;
cvar_t *rt_volumetric;
cvar_t *rt_volumetricDensity;
cvar_t *rt_volumetricScatter;
cvar_t *rt_pbrMaps;
cvar_t *rt_cloudCoverage;
cvar_t *rt_caustics;
cvar_t *rt_reflections;

static qboolean rtBackendActive = qfalse;

qboolean RT_IsBackendActive( void ) {
    return rtBackendActive;
}
static qboolean rtBackendInitFailureLogged = qfalse;
static qboolean rtBackendHardwareWarned = qfalse;
static char rtBackendLastChoice[MAX_QPATH] = "auto";
static int rtBackendLastEnableState = 0;
static int rtBackendLastTracerEnableState = 0;
static uint32_t g_seed = 1u;

#ifndef VectorDistance
static ID_INLINE float VectorDistance(const vec3_t p1, const vec3_t p2) {
    vec3_t delta;
    VectorSubtract(p2, p1, delta);
    return VectorLength(delta);
}
#endif

static float FastRandom(void) {
	g_seed = 1664525u * g_seed + 1013904223u;
	return (float)(g_seed & 0x00FFFFFFu) / 16777215.0f;
}

static float RT_SafeRadius( float radius ) {
	const float minRadius = 16.0f;
	const float maxRadius = 131072.0f;

	if ( radius < minRadius ) {
		return minRadius;
	}

	if ( radius > maxRadius ) {
		return maxRadius;
	}

	return radius;
}

static float RT_TranslateStaticLightIntensity( float rawEnergy, const vec3_t color ) {
	if ( rawEnergy <= 0.0f ) {
		return 0.01f;
	}

	float positiveEnergy = MAX( rawEnergy, 1.0f );
	float intensity = powf( positiveEnergy, 0.8f ) * 0.18f;

	float colorStrength = ( fabsf( color[0] ) + fabsf( color[1] ) + fabsf( color[2] ) ) / 3.0f;
	colorStrength = Com_Clamp( 0.1f, 4.0f, sqrtf( MAX( colorStrength, 0.0001f ) ) * 1.5f );
	intensity *= colorStrength;

	if ( rt_staticLightScale ) {
		float scale = rt_staticLightScale->value;
		if ( scale < 0.0f ) {
			scale = 0.0f;
		}
		intensity *= scale;
	}

	if ( intensity < 0.01f ) {
		intensity = 0.01f;
	}

	return intensity;
}

static float RT_TranslateStaticLightRadius( float requestedRadius, float rawEnergy ) {
	float radius = requestedRadius;
	float positiveEnergy = MAX( rawEnergy, 1.0f );

	if ( radius <= 0.0f || !isfinite( radius ) ) {
		float base = 96.0f + sqrtf( positiveEnergy ) * 12.0f;
		radius = base;
	}

	float radiusScale = ( rt_staticLightRadiusScale ) ? rt_staticLightRadiusScale->value : 1.0f;
	if ( radiusScale < 0.05f ) {
		radiusScale = 0.05f;
	}

	radius *= radiusScale;
	return RT_SafeRadius( radius );
}

static void RT_ApplyStaticLightAutoScale(void) {
    if (!rt.staticLights || rt.numStaticLights <= 0) {
        return;
    }
    if (!rt_staticLightAutoScale || rt_staticLightAutoScale->integer == 0) {
        return;
    }

    int contributing = 0;
    double total = 0.0;
    for (int i = 0; i < rt.numStaticLights; ++i) {
        const staticLight_t *sl = &rt.staticLights[i];
        if (sl->intensity > 0.0f) {
            contributing++;
            total += sl->intensity;
        }
    }

    if (contributing <= 0 || total <= 0.0) {
        return;
    }

    float average = (float)(total / (double)contributing);
    float target = (rt_staticLightAutoTarget) ? rt_staticLightAutoTarget->value : 0.0f;
    if (target <= 0.0f) {
        target = 35.0f;
    }

    float scale = target / MAX(average, 1e-3f);
    // Upscaling is capped to avoid amplifying near-black maps into noise,
    // but downscaling must be unbounded: legacy content ships intensities
    // far above the PBR target and clamping left them overexposed.
    if (scale > 1.0f) {
        scale = MIN(scale, 6.0f);
    }

    // Cap individual lights relative to the target as well: normalizing the
    // average still lets single outliers (10x the target) blow out nearby
    // walls; lightmap-era content never delivered that much local energy.
    const float maxIntensity = target * 3.0f;

    if (fabsf(scale - 1.0f) >= 0.05f) {
        for (int i = 0; i < rt.numStaticLights; ++i) {
            staticLight_t *sl = &rt.staticLights[i];
            sl->intensity *= scale;
        }
    }

    for (int i = 0; i < rt.numStaticLights; ++i) {
        staticLight_t *sl = &rt.staticLights[i];
        if (sl->intensity > maxIntensity) {
            sl->intensity = maxIntensity;
        }
    }

    if (rt_debug && rt_debug->integer >= 2) {
        ri.Printf(PRINT_ALL,
                  "RT: auto scaled %d static lights by %.2f (avg=%.2f target=%.2f)\n",
                  contributing, scale, average, target);
    }
}

static qboolean RT_ComputeSkyLight(vec3_t outDirection, vec3_t outColor, float *outIntensity) {
	vec3_t direction;
	VectorCopy(rtSkyDirectionAccum, direction);

    if (VectorNormalize(direction) <= 0.0f || !isfinite(direction[0])) {
        VectorSet(direction, 0.0f, -1.0f, 0.0f);
    }
    vec3_t downward = { 0.0f, 0.0f, -1.0f };
    if (direction[2] > -0.35f) {
        vec3_t blended;
        VectorLerp(direction, downward, 0.6f, blended);
        if (VectorNormalize(blended) > 0.0f && blended[2] < -0.2f) {
            VectorCopy(blended, direction);
        } else {
            VectorCopy(downward, direction);
        }
    }

    float weight = rtSkyWeightAccum;
    vec3_t avgColor;
    if (weight > 0.0f && isfinite(weight)) {
        VectorScale(rtSkyColorAccum, 1.0f / weight, avgColor);
	} else {
		VectorSet(avgColor, 0.65f, 0.75f, 1.0f);
	}

	float maxChannel = MAX(MAX(avgColor[0], avgColor[1]), avgColor[2]);
	if (maxChannel <= 0.0f || !isfinite(maxChannel)) {
		maxChannel = 1.0f;
		VectorSet(avgColor, 0.65f, 0.75f, 1.0f);
	}

    // The accumulated sky color carries arbitrary radiometric units (sky
    // surface contributions come in at ~90 per channel), so it only defines
    // the light's hue — never its intensity. The skylight uses a fixed base
    // calibrated for the PBR pipeline: 3.0 puts a fully lit surface
    // (NdotL ~ 0.7, albedo ~ 0.5) at ~0.35 linear before GI fill.
    float intensity = 3.0f;
    vec3_t colorNormalized;
    VectorScale(avgColor, 1.0f / maxChannel, colorNormalized);

    // The dome keeps the full chroma of the map's sky art. The LIGHT the
    // sky casts is a different thing: atmospheres scatter toward white, and
    // lighting an entire map with the raw dome hue monochromes it — so the
    // skylight and ambient use a strongly desaturated version.
    VectorCopy(colorNormalized, rt.skyDomeColor);
    {
        float gray = colorNormalized[0] * 0.2126f +
                     colorNormalized[1] * 0.7152f +
                     colorNormalized[2] * 0.0722f;
        for (int c = 0; c < 3; c++) {
            colorNormalized[c] = gray + (colorNormalized[c] - gray) * 0.12f;
        }
        float m = MAX(MAX(colorNormalized[0], colorNormalized[1]), colorNormalized[2]);
        if (m > 0.0001f) {
            VectorScale(colorNormalized, 1.0f / m, colorNormalized);
        }
    }

    float scale = (rt_skyLightScale && rt_skyLightScale->value >= 0.0f) ? rt_skyLightScale->value : 1.0f;
    intensity *= scale;
    intensity = Com_Clamp(0.0f, 20.0f, intensity);
	if (intensity <= 0.0f) {
		return qfalse;
	}

    if (rt_debug && rt_debug->integer >= 3) {
        ri.Printf(PRINT_DEVELOPER,
            "RT_ComputeSkyLight: weight=%.2f avgColor=(%.2f,%.2f,%.2f) maxChannel=%.2f scale=%.2f -> intensity=%.3f\n",
            weight,
            avgColor[0], avgColor[1], avgColor[2],
            maxChannel,
            scale,
            intensity);
    }

	VectorCopy(direction, outDirection);
	VectorCopy(colorNormalized, outColor);
	*outIntensity = intensity;

    float ambientFactor = 0.00085f;
    if (rt_skyAmbientFactor) {
        float configured = rt_skyAmbientFactor->value;
        if (configured >= 0.0f) {
            ambientFactor = configured;
        }
    }
    if (ambientFactor < 0.0f) {
        ambientFactor = 0.0f;
    } else if (ambientFactor > 0.05f) {
        ambientFactor = 0.05f;
    }

    VectorCopy(colorNormalized, rt.skyAmbientColor);
    rt.skyAmbientIntensity = intensity * ambientFactor;
    float ambientClamp = MAX(scale, 1.0f) * 5.0f;
    if (rt.skyAmbientIntensity > ambientClamp) {
        rt.skyAmbientIntensity = ambientClamp;
    }
    if (rt_debug && rt_debug->integer >= 2) {
        ri.Printf(PRINT_ALL,
            "RT: sky ambient color=(%.2f,%.2f,%.2f) intensity=%.3f (factor=%.5f)\n",
            rt.skyAmbientColor[0], rt.skyAmbientColor[1], rt.skyAmbientColor[2],
            rt.skyAmbientIntensity, ambientFactor);
    }
    return qtrue;
}

static float RT_ComputeSpotCosFromFov( float fovDegrees ) {
	if ( fovDegrees <= 0.0f ) {
		return 1.0f;
	}

	float radians = DEG2RAD( 0.5f * fovDegrees );
	float cosTheta = cosf( radians );

	if ( cosTheta > 1.0f ) {
		cosTheta = 1.0f;
	} else if ( cosTheta < -1.0f ) {
		cosTheta = -1.0f;
	}

	return cosTheta;
}

static uint32_t RT_ComputeSceneLightHash( const rtSceneLight_t *lights, int count ) {
	if ( !lights || count <= 0 ) {
		return 0u;
	}

	return (uint32_t)Com_BlockChecksum( lights, count * (int)sizeof( rtSceneLight_t ) );
}

static void VectorLerp( const vec3_t from, const vec3_t to, float lerp, vec3_t out ) {
	out[0] = from[0] + ( to[0] - from[0] ) * lerp;
	out[1] = from[1] + ( to[1] - from[1] ) * lerp;
	out[2] = from[2] + ( to[2] - from[2] ) * lerp;
}

static char rtBackendStatusMessage[128] = "Software backend inactive";

const char *RT_GetBackendStatus(void) {
	return rtBackendStatusMessage;
}

void RT_SetBackendStatus( const char *fmt, ... ) {
	if ( !fmt || !fmt[0] ) {
		Q_strncpyz( rtBackendStatusMessage, "Software backend inactive", sizeof( rtBackendStatusMessage ) );
		return;
	}

	va_list args;
	va_start( args, fmt );
	Q_vsnprintf( rtBackendStatusMessage, sizeof( rtBackendStatusMessage ), fmt, args );
	va_end( args );
}

void RT_ResetBackendLogs( void ) {
	rtBackendInitFailureLogged = qfalse;
	rtBackendHardwareWarned = qfalse;
}

#define RT_BACKEND_INDEX_COMPUTE   0
#define RT_BACKEND_INDEX_HARDWARE  1
#define RT_BACKEND_RMSE_THRESHOLD  0.0025f

static const char *rtValidationMaps[] = {
    "q3dm1",
    "q3dm7",
    "q3dm17",
    NULL
};

static qboolean RT_MapIsValidationTarget(const char *mapName);
static void RT_GetValidationMapName(char *buffer, size_t bufferSize);
static void RT_RecordBackendValidation(const float *rgba, int width, int height, qboolean validated);
static void RT_ReportBackendParity(void);

#ifdef USE_VULKAN
static void RT_DestroySceneLightBuffer(void);
static uint32_t rtLastUploadedLightHash = 0u;
#endif

static void RT_SelectBackend(void);
static void RT_SyncModeAlias(void);

static void RT_SelectBackend(void) {
    const char *backendStr = r_rt_backend ? r_rt_backend->string : "auto";
    if (!backendStr || !backendStr[0]) {
        backendStr = "auto";
    }

    if (r_rt_backend && r_rt_backend->modified) {
        ri.Printf(PRINT_ALL, "r_rt_backend set to '%s'\n", backendStr);
        r_rt_backend->modified = qfalse;
        RT_ResetBackendLogs();
    }
#ifdef USE_VULKAN
    if (rtx_enable && rtx_enable->modified) {
        ri.Printf(PRINT_ALL, "rtx_enable set to %d\n", rtx_enable->integer);
        rtx_enable->modified = qfalse;
        RT_ResetBackendLogs();
    }
#endif

    if (Q_stricmp(backendStr, rtBackendLastChoice) != 0) {
        Q_strncpyz(rtBackendLastChoice, backendStr, sizeof(rtBackendLastChoice));
        RT_ResetBackendLogs();
    }

    int enableState = ri.Cvar_VariableIntegerValue("rtx_enable");
    if (enableState != rtBackendLastEnableState) {
        rtBackendLastEnableState = enableState;
        RT_ResetBackendLogs();
    }

    qboolean tracerEnabled = (rt_enable && rt_enable->integer) ? qtrue : qfalse;
    int tracerState = tracerEnabled ? 1 : 0;
    if (tracerState != rtBackendLastTracerEnableState) {
        rtBackendLastTracerEnableState = tracerState;
        RT_ResetBackendLogs();
    }

    if (rtBackendActive && rt.useRTX) {
        RT_SetBackendStatus("RTX hardware backend active (%s)", backendStr);
    }

    qboolean forceHardware = !Q_stricmp(backendStr, "hardware");
    qboolean wantHardware = tracerEnabled && (enableState != 0) &&
        (forceHardware || !Q_stricmp(backendStr, "auto"));

    if (!wantHardware) {
        if (rtBackendActive) {
            RT_ShutdownBackend();
        } else {
            rt.useRTX = qfalse;
            RT_SetBackendStatus("Software backend active (backend=%s)", backendStr);
        }
        return;
    }

#ifdef USE_VULKAN
    if (RTX_IsAvailable()) {
        if (!rtBackendActive) {
            ri.Printf(PRINT_ALL, "RTX hardware backend enabled\n");
            if (tr.world) {
                if (rtx.numBLAS == 0 || rtx.tlas.numInstances == 0) {
                    RTX_PopulateWorld();
                } else {
                    RTX_RequestWorldRefit();
                }
            }
            rt.sceneLightBufferDirty = qtrue;
            RT_SetBackendStatus("RTX hardware backend active (%s)", backendStr);
        }
        rtBackendActive = qtrue;
        rt.useRTX = qtrue;
        RT_ResetBackendLogs();
        return;
    }

    if (!rtBackendInitFailureLogged) {
        if (RTX_Init()) {
            if (RTX_IsAvailable()) {
                if (!rtBackendActive) {
                    ri.Printf(PRINT_ALL, "RTX hardware backend enabled\n");
                    if (tr.world) {
                        if (rtx.numBLAS == 0 || rtx.tlas.numInstances == 0) {
                            RTX_PopulateWorld();
                        } else {
                            RTX_RequestWorldRefit();
                        }
                    }
                    rt.sceneLightBufferDirty = qtrue;
                    RT_SetBackendStatus("RTX hardware backend active (%s)", backendStr);
                }
                rtBackendActive = qtrue;
                rt.useRTX = qtrue;
                RT_ResetBackendLogs();
                return;
            }
        } else {
            rtBackendInitFailureLogged = qtrue;
        }
    }
#else
    if (!rtBackendHardwareWarned) {
        ri.Printf(PRINT_WARNING, "RTX: hardware backend requested but Vulkan RTX is not available in this build; using software path\n");
        rtBackendHardwareWarned = qtrue;
    }
#endif

    if (rtBackendActive) {
        RT_ShutdownBackend();
    } else {
        rt.useRTX = qfalse;
        RT_SetBackendStatus("Software backend active (RTX unavailable)");
    }

#ifdef USE_VULKAN
    if (forceHardware && !rtBackendHardwareWarned) {
        ri.Printf(PRINT_WARNING, "RTX: hardware backend requested but RTX initialization failed; using software path\n");
        rtBackendHardwareWarned = qtrue;
    }
#endif
}

static qboolean RT_MapIsValidationTarget(const char *mapName) {
    if (!mapName || !mapName[0]) {
        return qfalse;
    }

    for (int i = 0; rtValidationMaps[i]; ++i) {
        if (!Q_stricmp(mapName, rtValidationMaps[i])) {
            return qtrue;
        }
    }

    return qfalse;
}

static void RT_GetValidationMapName(char *buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) {
        return;
    }

    buffer[0] = '\0';

    if (!tr.world || !tr.world->name[0]) {
        Q_strncpyz(buffer, "unknown", bufferSize);
        return;
    }

    const char *name = tr.world->name;
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;

    Q_strncpyz(buffer, base, bufferSize);

    char *dot = strrchr(buffer, '.');
    if (dot) {
        *dot = '\0';
    }
}

static void RT_RecordBackendValidation(const float *rgba, int width, int height, qboolean validated) {
    if (!rt_gpuValidate || rt_gpuValidate->integer <= 0) {
        rt.backendValidation[RT_BACKEND_INDEX_COMPUTE].valid = qfalse;
        rt.backendValidation[RT_BACKEND_INDEX_HARDWARE].valid = qfalse;
        rt.backendRMSEDelta = 0.0;
        rt.backendMaxErrorDelta = 0.0;
        rt.backendParityFrame = 0;
        rt.backendParityMap[0] = '\0';
        return;
    }

    if (!rgba || width <= 0 || height <= 0) {
        return;
    }

    char mapName[MAX_QPATH];
    RT_GetValidationMapName(mapName, sizeof(mapName));

    if (!RT_MapIsValidationTarget(mapName)) {
        return;
    }

#ifdef USE_VULKAN
    qboolean hardwareActive = (rt.useRTX && RTX_IsAvailable());
#else
    qboolean hardwareActive = qfalse;
#endif

    int backendIndex = hardwareActive ? RT_BACKEND_INDEX_HARDWARE : RT_BACKEND_INDEX_COMPUTE;
    rtBackendValidation_t *entry = &rt.backendValidation[backendIndex];

    entry->hardware = hardwareActive;
    entry->valid = validated;
    Q_strncpyz(entry->map, mapName, sizeof(entry->map));
    entry->width = width;
    entry->height = height;
    entry->frame = rt.currentFrame;
    entry->samples = validated ? rt.validationSamples : 0;
    entry->rmse = validated ? rt.validationRMSE : 0.0;
    entry->maxError = validated ? rt.validationMaxError : 0.0;

    size_t pixelCount = (size_t)width * (size_t)height;
    size_t byteCount = pixelCount * 4 * sizeof(float);
    if (byteCount > (size_t)INT_MAX) {
        byteCount = (size_t)INT_MAX;
    }
    entry->hash = Com_BlockChecksum(rgba, (int)byteCount);

    if (entry->valid) {
        RT_ReportBackendParity();
    }
}

static void RT_ReportBackendParity(void) {
    rtBackendValidation_t *hardware = &rt.backendValidation[RT_BACKEND_INDEX_HARDWARE];
    rtBackendValidation_t *compute = &rt.backendValidation[RT_BACKEND_INDEX_COMPUTE];

    if (!hardware->valid || !compute->valid) {
        return;
    }

    if (Q_stricmp(hardware->map, compute->map) != 0) {
        return;
    }

    if (hardware->width != compute->width || hardware->height != compute->height) {
        return;
    }

    int newestFrame = (hardware->frame > compute->frame) ? hardware->frame : compute->frame;

    if (newestFrame == rt.backendParityFrame &&
        !Q_stricmp(rt.backendParityMap, hardware->map)) {
        return;
    }

    double rmseDelta = hardware->rmse - compute->rmse;
    double maxDelta = hardware->maxError - compute->maxError;

    rt.backendParityFrame = newestFrame;
    rt.backendRMSEDelta = rmseDelta;
    rt.backendMaxErrorDelta = maxDelta;
    Q_strncpyz(rt.backendParityMap, hardware->map, sizeof(rt.backendParityMap));

    if (rmseDelta > RT_BACKEND_RMSE_THRESHOLD) {
        ri.Printf(PRINT_WARNING,
                  "rt_gpuValidate: Hardware backend RMSE regression on %s (HW=%.5f, Compute=%.5f, Δ=%.5f)\n",
                  hardware->map,
                  (float)hardware->rmse,
                  (float)compute->rmse,
                  (float)rmseDelta);
    } else {
        ri.Printf(PRINT_DEVELOPER,
                  "rt_gpuValidate: Backend parity on %s (HW=%.5f, Compute=%.5f, Δ=%.5f, maxΔ=%.5f)\n",
                  hardware->map,
                  (float)hardware->rmse,
                  (float)compute->rmse,
                  (float)rmseDelta,
                  (float)maxDelta);
    }
}

qboolean RT_TraceShadowRaySoftware(const vec3_t origin, const vec3_t direction, float maxDist) {
    ray_t ray;
    hitInfo_t hit;

    VectorCopy(origin, ray.origin);
    VectorCopy(direction, ray.direction);
    ray.tMin = 0.001f;
    ray.tMax = maxDist;

    return RT_TraceRay(&ray, &hit);
}

#ifdef USE_VULKAN
static VkDeviceSize RT_GetSceneLightCapacity(void) {
    return (VkDeviceSize)sizeof(rtxLightGpuUpload_t);
}

static void RT_InitSceneLightBuffer(void) {
    if (!vk.device || !vk.physical_device) {
        return;
    }

    if (rt.sceneLightBuffer != VK_NULL_HANDLE && rt.sceneLightBufferMemory != VK_NULL_HANDLE) {
        return;
    }

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .size = RT_GetSceneLightCapacity(),
        // Device address is required because validation reported a device-address
        // lookup on this buffer; add the flag proactively.
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL
    };

    if (vkCreateBuffer(vk.device, &bufferInfo, NULL, &rt.sceneLightBuffer) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RT_InitSceneLightBuffer: failed to create buffer of size %llu\n",
            (unsigned long long)bufferInfo.size);
        rt.sceneLightBuffer = VK_NULL_HANDLE;
        rt.sceneLightBufferMemory = VK_NULL_HANDLE;
        rt.sceneLightBufferSize = 0;
        return;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RT_InitSceneLightBuffer: created buffer %p (%llu bytes)\n",
                  (void*)rt.sceneLightBuffer,
                  (unsigned long long)bufferInfo.size);
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RT_InitSceneLightBuffer: created buffer %p (%llu bytes)\n",
                  (void*)rt.sceneLightBuffer,
                  (unsigned long long)bufferInfo.size);
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, rt.sceneLightBuffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    // Pad host-visible scene-light buffer to absorb small driver overruns.
    const VkDeviceSize pad = 1024 * 1024; // 1 MiB
    VkDeviceSize padAligned = (pad + memReqs.alignment - 1) & ~(memReqs.alignment - 1);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &allocFlags,
        .allocationSize = memReqs.size + padAligned,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RT_InitSceneLightBuffer: alloc=%llu (raw=%llu pad=%llu align=%llu)\n",
                  (unsigned long long)allocInfo.allocationSize,
                  (unsigned long long)memReqs.size,
                  (unsigned long long)padAligned,
                  (unsigned long long)memReqs.alignment);
    }

    if (vkAllocateMemory(vk.device, &allocInfo, NULL, &rt.sceneLightBufferMemory) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RT_InitSceneLightBuffer: failed to allocate %llu bytes for scene lights\n",
                  (unsigned long long)allocInfo.allocationSize);
        vkDestroyBuffer(vk.device, rt.sceneLightBuffer, NULL);
        rt.sceneLightBuffer = VK_NULL_HANDLE;
        rt.sceneLightBufferMemory = VK_NULL_HANDLE;
        rt.sceneLightBufferSize = 0;
        return;
    }

    if (vkBindBufferMemory(vk.device, rt.sceneLightBuffer, rt.sceneLightBufferMemory, 0) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RT_InitSceneLightBuffer: vkBindBufferMemory failed\n");
        vkFreeMemory(vk.device, rt.sceneLightBufferMemory, NULL);
        vkDestroyBuffer(vk.device, rt.sceneLightBuffer, NULL);
        rt.sceneLightBuffer = VK_NULL_HANDLE;
        rt.sceneLightBufferMemory = VK_NULL_HANDLE;
        rt.sceneLightBufferSize = 0;
        return;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RT_InitSceneLightBuffer: bound memory %p\n",
                  (void*)rt.sceneLightBufferMemory);
        VkDeviceAddress addr = RTX_GetBufferDeviceAddressVK(rt.sceneLightBuffer);
        if (addr) {
            ri.Printf(PRINT_DEVELOPER,
                      "RT_InitSceneLightBuffer: deviceAddr=0x%llx size=%llu\n",
                      (unsigned long long)addr,
                      (unsigned long long)allocInfo.allocationSize);
        }
    }

    rt.sceneLightBufferSize = bufferInfo.size;
    rt.sceneLightBufferDirty = qtrue;
}

static void RT_DestroySceneLightBuffer(void) {
    if (!vk.device) {
        return;
    }

    if (rt.sceneLightBuffer != VK_NULL_HANDLE) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RT_DestroySceneLightBuffer: destroying buffer %p\n",
                      (void*)rt.sceneLightBuffer);
        }
        vkDestroyBuffer(vk.device, rt.sceneLightBuffer, NULL);
        rt.sceneLightBuffer = VK_NULL_HANDLE;
    }

    if (rt.sceneLightBufferMemory != VK_NULL_HANDLE) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER,
                      "RT_DestroySceneLightBuffer: freeing memory %p\n",
                      (void*)rt.sceneLightBufferMemory);
        }
        vkFreeMemory(vk.device, rt.sceneLightBufferMemory, NULL);
        rt.sceneLightBufferMemory = VK_NULL_HANDLE;
    }

    rt.sceneLightBufferSize = 0;
    rt.sceneLightBufferDirty = qtrue;
    rtLastUploadedLightHash = 0u;
}

VkBuffer RT_GetSceneLightBuffer(void) {
    return rt.sceneLightBuffer;
}

VkDeviceSize RT_GetSceneLightBufferSize(void) {
    VkDeviceSize count = (VkDeviceSize)(rt.numSceneLights > 0 ? rt.numSceneLights : 1);
    VkDeviceSize desired = count * (VkDeviceSize)sizeof(rtxLightGpu_t);
    if (rt.sceneLightBufferSize > 0 && desired > rt.sceneLightBufferSize) {
        desired = rt.sceneLightBufferSize;
    }
    return desired;
}

static void RT_FillGpuLight(const rtSceneLight_t *src, rtxLightGpu_t *dst) {
    vec3_t direction;
    VectorCopy(src->direction, direction);

    float typeTag = 1.0f; // default to point
    float innerCos = 0.0f;
    float outerCos = 0.0f;
    float radius = RT_SafeRadius(src->radius);

    switch (src->type) {
    case RT_LIGHT_TYPE_DIRECTIONAL:
        typeTag = 0.0f;
        if (VectorNormalize(direction) <= 0.0f) {
            VectorSet(direction, 0.0f, 0.0f, -1.0f);
        }
        dst->position[0] = 0.0f;
        dst->position[1] = 0.0f;
        dst->position[2] = 0.0f;
        break;
    case RT_LIGHT_TYPE_SPOT:
        typeTag = 2.0f;
        dst->position[0] = src->origin[0];
        dst->position[1] = src->origin[1];
        dst->position[2] = src->origin[2];
        if (VectorNormalize(direction) <= 0.0f) {
            VectorSet(direction, 0.0f, 0.0f, -1.0f);
        }
        outerCos = Com_Clamp(-1.0f, 1.0f, src->spotCos);
        float innerBias = 0.1f * (1.0f - outerCos);
        innerCos = Com_Clamp(-1.0f, 1.0f, outerCos + innerBias);
        break;
    default: // point
        typeTag = 1.0f;
        dst->position[0] = src->origin[0];
        dst->position[1] = src->origin[1];
        dst->position[2] = src->origin[2];
        VectorClear(direction);
        break;
    }

    dst->position[3] = typeTag;

    dst->direction[0] = direction[0];
    dst->direction[1] = direction[1];
    dst->direction[2] = direction[2];
    dst->direction[3] = innerCos;

    float intensity = src->intensity > 0.0f ? src->intensity
        : (fabsf(src->color[0]) + fabsf(src->color[1]) + fabsf(src->color[2])) / 3.0f;
    if (intensity <= 0.0f) {
        intensity = 1.0f;
    }

    dst->color[0] = src->color[0];
    dst->color[1] = src->color[1];
    dst->color[2] = src->color[2];
    dst->color[3] = intensity;

    float constant = 1.0f;
    float linear = 0.0f;
    float quadratic = 0.0f;

    if (typeTag == 1.0f || typeTag == 2.0f) {
        float invRadius = radius > 0.0f ? 1.0f / radius : 1.0f;
        constant = 1.0f;
        linear = 2.0f * invRadius;
        quadratic = invRadius * invRadius;
    }

    dst->attenuation[0] = constant;
    dst->attenuation[1] = linear;
    dst->attenuation[2] = quadratic;
    dst->attenuation[3] = outerCos;
}

void RT_UpdateSceneLightBuffer(void) {
    if (!vk.device) {
        return;
    }

    if (rt.sceneLightBuffer == VK_NULL_HANDLE || rt.sceneLightBufferMemory == VK_NULL_HANDLE) {
        RT_InitSceneLightBuffer();
        if (rt.sceneLightBuffer == VK_NULL_HANDLE || rt.sceneLightBufferMemory == VK_NULL_HANDLE) {
            return;
        }
    }

    if (!rt.sceneLightBufferDirty && rtLastUploadedLightHash == rt.sceneLightHash) {
        return;
    }

    size_t lightCount = (size_t)(rt.numSceneLights > 0 ? rt.numSceneLights : 0);
    if (lightCount > (size_t)RT_MAX_SCENE_LIGHTS) {
        if (rt_debug && rt_debug->integer >= 1) {
            ri.Printf(PRINT_WARNING, "RT_UpdateSceneLightBuffer: clamping %zu lights to %d GPU entries\n",
                lightCount, RT_MAX_SCENE_LIGHTS);
        }
        lightCount = RT_MAX_SCENE_LIGHTS;
    }

    rtPendingLightUpload.numLights = (uint32_t)lightCount;

    for (size_t i = 0; i < lightCount; ++i) {
        RT_FillGpuLight(&rt.sceneLights[i], &rtPendingLightUpload.lights[i]);
    }

    const size_t headerSize = sizeof(rtPendingLightUpload.numLights);
    size_t uploadBytes = headerSize + lightCount * sizeof(rtxLightGpu_t);
    if (uploadBytes > (size_t)rt.sceneLightBufferSize) {
        uploadBytes = rt.sceneLightBufferSize;
    }
    if (uploadBytes < headerSize) {
        uploadBytes = headerSize;
    }

    rtPendingLightBytes = uploadBytes;
    rtLightUploadPending = qtrue;

    if (rt_debug && rt_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER, "RT_UpdateSceneLightBuffer: staged %u lights (%zu bytes)\n",
            rtPendingLightUpload.numLights, uploadBytes);
    }

    rt.sceneLightBufferDirty = qfalse;
    rtLastUploadedLightHash = rt.sceneLightHash;
}

/*
================
RT_RecordSceneLightUpload

Record the staged scene light data into the frame command buffer. Chunked
because vkCmdUpdateBuffer accepts at most 65536 bytes per call. The caller
is responsible for transfer/shader barriers around the update.
================
*/
void RT_RecordSceneLightUpload(VkCommandBuffer cmd) {
    if (!rtLightUploadPending || cmd == VK_NULL_HANDLE ||
        rt.sceneLightBuffer == VK_NULL_HANDLE) {
        return;
    }

    const byte *src = (const byte *)&rtPendingLightUpload;
    size_t remaining = rtPendingLightBytes;
    VkDeviceSize offset = 0;

    while (remaining > 0) {
        size_t chunk = remaining > 65536 ? 65536 : remaining;
        // vkCmdUpdateBuffer requires size to be a multiple of 4
        chunk &= ~(size_t)3;
        if (chunk == 0) {
            break;
        }
        vkCmdUpdateBuffer(cmd, rt.sceneLightBuffer, offset, chunk, src + offset);
        offset += chunk;
        remaining -= chunk;
    }

    rtLightUploadPending = qfalse;
}

static void RT_SyncModeAlias(void) {
    if (!rt_mode || !r_rt_mode) {
        return;
    }

    if (Q_stricmp(r_rt_mode->string, rt_mode->string)) {
        ri.Cvar_Set("rt_mode", r_rt_mode->string);
    }
}
#endif

static ID_INLINE int RT_LightGridFlattenIndex(const rtLightGrid_t *grid, int x, int y, int z) {
    return (z * grid->dims[1] + y) * grid->dims[0] + x;
}

static ID_INLINE int RT_LightGridClampIndex(float coordinate, float origin, float invCell, int dim) {
    int idx = (int)floorf((coordinate - origin) * invCell);
    if (idx < 0) {
        idx = 0;
    } else if (idx >= dim) {
        idx = dim - 1;
    }
    return idx;
}

static void RT_DestroyLightGridHostData(void) {
    rtLightGrid_t *grid = &rt.lightGrid;

    if (grid->offsets) {
        ri.Free(grid->offsets);
        grid->offsets = NULL;
    }
    if (grid->indices) {
        ri.Free(grid->indices);
        grid->indices = NULL;
    }

    grid->offsetCount = 0;
    grid->indexCount = 0;
    grid->cellCount = 0;
    grid->directionalCount = 0;
    grid->dirty = qtrue;
}

static void RT_ValidateLightGrid(rtLightGrid_t *grid) {
    if (!grid) {
        return;
    }

    qboolean mutated = qfalse;
    uint32_t invalidOffsetFixes = 0;
    uint32_t invalidIndexFixes = 0;

    if (grid->directionalCount < 0) {
        grid->directionalCount = 0;
        mutated = qtrue;
    }

    uint32_t indexCount = grid->indices && grid->indexCount > 0 ? grid->indexCount : 0u;
    uint32_t offsetCount = grid->offsets && grid->offsetCount > 0 ? grid->offsetCount : 0u;

    if ((uint32_t)grid->directionalCount > indexCount) {
        grid->directionalCount = (int)indexCount;
        mutated = qtrue;
    }

    if (offsetCount > 0) {
        uint32_t previous = 0u;
        for (uint32_t i = 0u; i < offsetCount; ++i) {
            uint32_t clamped = grid->offsets[i];
            if (clamped > indexCount) {
                clamped = indexCount;
                ++invalidOffsetFixes;
            }
            if (clamped < previous) {
                clamped = previous;
                ++invalidOffsetFixes;
            }
            if (clamped != grid->offsets[i]) {
                grid->offsets[i] = clamped;
                mutated = qtrue;
            }
            previous = clamped;
        }
        if (previous != indexCount && offsetCount > 0) {
            grid->offsets[offsetCount - 1] = indexCount;
            mutated = qtrue;
        }
    }

    uint32_t lightCount = (rt.numSceneLights > 0) ? (uint32_t)rt.numSceneLights : 0u;
    if (indexCount > 0 && grid->indices) {
        if (lightCount == 0u) {
            grid->directionalCount = 0;
            Com_Memset(grid->indices, 0, (size_t)indexCount * sizeof(uint32_t));
            mutated = qtrue;
        } else {
            uint32_t maxValid = lightCount - 1u;
            for (uint32_t i = 0u; i < indexCount; ++i) {
                uint32_t stored = grid->indices[i];
                if (stored > maxValid) {
                    grid->indices[i] = maxValid;
                    ++invalidIndexFixes;
                    mutated = qtrue;
                }
            }
        }
    }

    if (mutated) {
        grid->dirty = qtrue;
        if (rt_debug && rt_debug->integer >= 1) {
            ri.Printf(PRINT_WARNING,
                      "RT_ValidateLightGrid: adjusted grid (dir=%d indexCount=%u lights=%u fixOffsets=%u fixIndices=%u)\n",
                      grid->directionalCount,
                      indexCount,
                      lightCount,
                      invalidOffsetFixes,
                      invalidIndexFixes);
        }
    }
}

static void RT_GuardGridExtents(vec3_t mins, vec3_t maxs) {
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(mins[i]) || !isfinite(maxs[i])) {
            mins[i] = -1024.0f;
            maxs[i] = 1024.0f;
            continue;
        }
        if (maxs[i] - mins[i] < 1.0f) {
            mins[i] -= 512.0f;
            maxs[i] += 512.0f;
        }
    }
}

static void RT_BuildLightGrid(void) {
    rtLightGrid_t *grid = &rt.lightGrid;

    grid->dirty = qtrue;

    // Reset previous data before rebuilding
    RT_DestroyLightGridHostData();

    vec3_t mins;
    vec3_t maxs;

    if (tr.world && tr.world->bmodels) {
        VectorCopy(tr.world->bmodels[0].bounds[0], mins);
        VectorCopy(tr.world->bmodels[0].bounds[1], maxs);
    } else {
        VectorCopy(tr.refdef.vieworg, mins);
        VectorCopy(tr.refdef.vieworg, maxs);
    }

    // The grid covers only the static prefix of the scene light array;
    // dynamic lights are iterated linearly by the consumers.
    const int gridLightCount = rt.staticSceneLightCount;

    float largestRadius = 0.0f;
    for (int i = 0; i < gridLightCount; ++i) {
        const rtSceneLight_t *light = &rt.sceneLights[i];

        if (light->type == RT_LIGHT_TYPE_DIRECTIONAL) {
            continue;
        }

        float radius = RT_SafeRadius(light->radius);
        largestRadius = MAX(largestRadius, radius);

        for (int axis = 0; axis < 3; ++axis) {
            float minVal = light->origin[axis] - radius;
            float maxVal = light->origin[axis] + radius;
            if (minVal < mins[axis]) {
                mins[axis] = minVal;
            }
            if (maxVal > maxs[axis]) {
                maxs[axis] = maxVal;
            }
        }
    }

    if (largestRadius <= 0.0f) {
        largestRadius = 128.0f;
    }

    for (int axis = 0; axis < 3; ++axis) {
        mins[axis] -= largestRadius * 0.1f;
        maxs[axis] += largestRadius * 0.1f;
    }

    RT_GuardGridExtents(mins, maxs);

    float targetCellSize = RT_LIGHT_GRID_TARGET_CELL_SIZE;
    if (rt_lightGridCellSize) {
        float requested = rt_lightGridCellSize->value;
        if (requested >= 32.0f && isfinite(requested)) {
            targetCellSize = requested;
        }
    }

    int computedDims[3];
    float axisExtents[3];
    for (int axis = 0; axis < 3; ++axis) {
        float extent = maxs[axis] - mins[axis];
        if (extent <= 0.0f || !isfinite(extent)) {
            extent = targetCellSize * (float)RT_LIGHT_GRID_MIN_DIM;
        }
        axisExtents[axis] = extent;
        int dim = (int)ceilf(extent / targetCellSize);
        dim = Com_Clamp(RT_LIGHT_GRID_MIN_DIM, RT_LIGHT_GRID_MAX_DIM, dim);
        computedDims[axis] = dim;
    }

    grid->dims[0] = computedDims[0];
    grid->dims[1] = computedDims[1];
    grid->dims[2] = computedDims[2];
    grid->cellCount = grid->dims[0] * grid->dims[1] * grid->dims[2];

    VectorCopy(mins, grid->origin);

    for (int axis = 0; axis < 3; ++axis) {
        float extent = axisExtents[axis];
        float dim = (float)grid->dims[axis];
        if (extent <= 0.0f || !isfinite(extent)) {
            extent = targetCellSize * dim;
        }

        grid->cellSize[axis] = extent / dim;
        if (grid->cellSize[axis] <= 0.0f || !isfinite(grid->cellSize[axis])) {
            grid->cellSize[axis] = targetCellSize;
        }
        grid->invCellSize[axis] = (grid->cellSize[axis] > 0.0f)
            ? 1.0f / grid->cellSize[axis]
            : 0.0f;
    }

    uint32_t directionalIndices[RT_MAX_SCENE_LIGHTS];
    uint32_t directionalCount = 0;

    uint32_t *cellCounts = (uint32_t *)ri.Malloc((size_t)grid->cellCount * sizeof(uint32_t));
    if (!cellCounts) {
        ri.Printf(PRINT_WARNING, "RT_BuildLightGrid: failed to allocate cellCounts (%d cells)\n", grid->cellCount);
        grid->cellCount = 0;
        return;
    }
    Com_Memset(cellCounts, 0, (size_t)grid->cellCount * sizeof(uint32_t));

    for (int i = 0; i < gridLightCount; ++i) {
        const rtSceneLight_t *light = &rt.sceneLights[i];

        if (light->type == RT_LIGHT_TYPE_DIRECTIONAL) {
            if (directionalCount < ARRAY_LEN(directionalIndices)) {
                directionalIndices[directionalCount++] = (uint32_t)i;
            } else if (rt_debug && rt_debug->integer >= 1) {
                ri.Printf(PRINT_WARNING, "RT_BuildLightGrid: exceeded directional capacity (%d)\n", ARRAY_LEN(directionalIndices));
            }
            continue;
        }

        float radius = RT_SafeRadius(light->radius);
        vec3_t lightMins;
        vec3_t lightMaxs;
        for (int axis = 0; axis < 3; ++axis) {
            lightMins[axis] = light->origin[axis] - radius;
            lightMaxs[axis] = light->origin[axis] + radius;
        }

        int minCell[3];
        int maxCell[3];
        for (int axis = 0; axis < 3; ++axis) {
            minCell[axis] = RT_LightGridClampIndex(lightMins[axis], grid->origin[axis], grid->invCellSize[axis], grid->dims[axis]);
            maxCell[axis] = RT_LightGridClampIndex(lightMaxs[axis], grid->origin[axis], grid->invCellSize[axis], grid->dims[axis]);
        }

        for (int z = minCell[2]; z <= maxCell[2]; ++z) {
            for (int y = minCell[1]; y <= maxCell[1]; ++y) {
                for (int x = minCell[0]; x <= maxCell[0]; ++x) {
                    int cellIndex = RT_LightGridFlattenIndex(grid, x, y, z);
                    cellCounts[cellIndex]++;
                }
            }
        }
    }

    grid->offsetCount = grid->cellCount + 1;
    grid->offsets = (uint32_t *)ri.Malloc((size_t)grid->offsetCount * sizeof(uint32_t));
    if (!grid->offsets) {
        ri.Printf(PRINT_WARNING, "RT_BuildLightGrid: failed to allocate %u offsets\n", grid->offsetCount);
        ri.Free(cellCounts);
        grid->cellCount = 0;
        return;
    }

    uint32_t prefix = 0;
    for (int cell = 0; cell < grid->cellCount; ++cell) {
        grid->offsets[cell] = prefix;
        prefix += cellCounts[cell];
    }
    grid->offsets[grid->cellCount] = prefix;

    uint32_t totalPerCell = prefix;
    grid->indexCount = directionalCount + totalPerCell;
    grid->directionalCount = directionalCount;

    if (grid->indexCount > 0) {
        grid->indices = (uint32_t *)ri.Malloc((size_t)grid->indexCount * sizeof(uint32_t));
        if (!grid->indices) {
            ri.Printf(PRINT_WARNING, "RT_BuildLightGrid: failed to allocate %u indices\n", grid->indexCount);
            ri.Free(cellCounts);
            ri.Free(grid->offsets);
            grid->offsets = NULL;
            grid->offsetCount = 0;
            grid->indexCount = 0;
            grid->directionalCount = 0;
            grid->cellCount = 0;
            return;
        }
        if (directionalCount > 0) {
            Com_Memcpy(grid->indices, directionalIndices, directionalCount * sizeof(uint32_t));
        }
    } else {
        grid->indexCount = 1;
        grid->indices = (uint32_t *)ri.Malloc(sizeof(uint32_t));
        if (!grid->indices) {
            ri.Printf(PRINT_WARNING, "RT_BuildLightGrid: failed to allocate fallback indices buffer\n");
            ri.Free(cellCounts);
            ri.Free(grid->offsets);
            grid->offsets = NULL;
            grid->offsetCount = 0;
            grid->indexCount = 0;
            grid->directionalCount = 0;
            grid->cellCount = 0;
            return;
        }
        grid->indices[0] = 0;
    }

    uint32_t *writeCursor = NULL;
    if (totalPerCell > 0) {
        writeCursor = (uint32_t *)ri.Malloc((size_t)grid->cellCount * sizeof(uint32_t));
        if (!writeCursor) {
            ri.Printf(PRINT_WARNING, "RT_BuildLightGrid: failed to allocate write cursor\n");
            ri.Free(cellCounts);
            if (grid->indices) {
                ri.Free(grid->indices);
                grid->indices = NULL;
            }
            ri.Free(grid->offsets);
            grid->offsets = NULL;
            grid->offsetCount = 0;
            grid->indexCount = 0;
            grid->directionalCount = 0;
            grid->cellCount = 0;
            return;
        }
        for (int cell = 0; cell < grid->cellCount; ++cell) {
            writeCursor[cell] = grid->offsets[cell];
        }
    }

    for (int i = 0; i < gridLightCount; ++i) {
        const rtSceneLight_t *light = &rt.sceneLights[i];
        if (light->type == RT_LIGHT_TYPE_DIRECTIONAL) {
            continue;
        }

        float radius = RT_SafeRadius(light->radius);
        vec3_t lightMins;
        vec3_t lightMaxs;
        for (int axis = 0; axis < 3; ++axis) {
            lightMins[axis] = light->origin[axis] - radius;
            lightMaxs[axis] = light->origin[axis] + radius;
        }

        int minCell[3];
        int maxCell[3];
        for (int axis = 0; axis < 3; ++axis) {
            minCell[axis] = RT_LightGridClampIndex(lightMins[axis], grid->origin[axis], grid->invCellSize[axis], grid->dims[axis]);
            maxCell[axis] = RT_LightGridClampIndex(lightMaxs[axis], grid->origin[axis], grid->invCellSize[axis], grid->dims[axis]);
        }

        for (int z = minCell[2]; z <= maxCell[2]; ++z) {
            for (int y = minCell[1]; y <= maxCell[1]; ++y) {
                for (int x = minCell[0]; x <= maxCell[0]; ++x) {
                    int cellIndex = RT_LightGridFlattenIndex(grid, x, y, z);
                    uint32_t writeIndex = writeCursor ? writeCursor[cellIndex]++ : 0;
                    if (grid->indices && directionalCount + writeIndex < grid->indexCount) {
                        grid->indices[directionalCount + writeIndex] = (uint32_t)i;
                    }
                }
            }
        }
    }

    if (writeCursor) {
        ri.Free(writeCursor);
    }

    if (rt_debug && rt_debug->integer >= 2) {
        float avgPerCell = (grid->cellCount > 0) ? ((float)totalPerCell / (float)grid->cellCount) : 0.0f;
        ri.Printf(PRINT_DEVELOPER,
            "RT: Light grid built dims=%dx%dx%d dir=%u localIndices=%u avgCell=%.2f cellSize=(%.1f,%.1f,%.1f)\n",
            grid->dims[0], grid->dims[1], grid->dims[2],
            grid->directionalCount, totalPerCell, avgPerCell,
            grid->cellSize[0], grid->cellSize[1], grid->cellSize[2]);
    }

    ri.Free(cellCounts);

    RT_ValidateLightGrid(grid);

    if (rt_debug && rt_debug->integer >= 1) {
        ri.Printf(PRINT_ALL,
                  "RT_ExtractStaticLights: built %d static lights (dir=%u cells=%d indexCount=%u)\n",
                  rt.numStaticLights,
                  grid->directionalCount,
                  grid->cellCount,
                  grid->indexCount);
    }
}

#ifdef USE_VULKAN
static void RT_DestroyLightGridBuffers(void) {
    if (!vk.device) {
        return;
    }

    if (rt.lightGrid.offsetBuffer != VK_NULL_HANDLE) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER, "RT_DestroyLightGridBuffers: destroying offset buffer %p\n",
                (void *)rt.lightGrid.offsetBuffer);
        }
        vkDestroyBuffer(vk.device, rt.lightGrid.offsetBuffer, NULL);
        rt.lightGrid.offsetBuffer = VK_NULL_HANDLE;
    }

    if (rt.lightGrid.offsetMemory != VK_NULL_HANDLE) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER, "RT_DestroyLightGridBuffers: freeing offset memory %p\n",
                (void *)rt.lightGrid.offsetMemory);
        }
        vkFreeMemory(vk.device, rt.lightGrid.offsetMemory, NULL);
        rt.lightGrid.offsetMemory = VK_NULL_HANDLE;
    }

    if (rt.lightGrid.indexBuffer != VK_NULL_HANDLE) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER, "RT_DestroyLightGridBuffers: destroying index buffer %p\n",
                (void *)rt.lightGrid.indexBuffer);
        }
        vkDestroyBuffer(vk.device, rt.lightGrid.indexBuffer, NULL);
        rt.lightGrid.indexBuffer = VK_NULL_HANDLE;
    }

    if (rt.lightGrid.indexMemory != VK_NULL_HANDLE) {
        if (r_rtx_debug && r_rtx_debug->integer >= 2) {
            ri.Printf(PRINT_DEVELOPER, "RT_DestroyLightGridBuffers: freeing index memory %p\n",
                (void *)rt.lightGrid.indexMemory);
        }
        vkFreeMemory(vk.device, rt.lightGrid.indexMemory, NULL);
        rt.lightGrid.indexMemory = VK_NULL_HANDLE;
    }

    rt.lightGrid.offsetBufferSize = 0;
    rt.lightGrid.indexBufferSize = 0;
}

VkBuffer RT_GetLightGridOffsetBuffer(void) {
    return rt.lightGrid.offsetBuffer;
}

VkDeviceSize RT_GetLightGridOffsetBufferSize(void) {
    return rt.lightGrid.offsetBufferSize;
}

VkBuffer RT_GetLightGridIndexBuffer(void) {
    return rt.lightGrid.indexBuffer;
}

VkDeviceSize RT_GetLightGridIndexBufferSize(void) {
    return rt.lightGrid.indexBufferSize;
}

static qboolean RT_EnsureLightGridBuffer(VkBuffer *buffer,
    VkDeviceMemory *memory,
    VkDeviceSize *currentSize,
    VkDeviceSize requiredSize,
    const char *label) {

    if (requiredSize == 0) {
        return qfalse;
    }

    if (*buffer != VK_NULL_HANDLE && *currentSize >= requiredSize) {
        return qtrue;
    }

    if (*buffer != VK_NULL_HANDLE || *memory != VK_NULL_HANDLE) {
        vkQueueWaitIdle(vk.queue);
    }

    if (*buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk.device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
    }
    if (*memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk.device, *memory, NULL);
        *memory = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = requiredSize,
        // Add device-address flag to satisfy vkGetBufferDeviceAddress validation.
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    if (vkCreateBuffer(vk.device, &bufferInfo, NULL, buffer) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RT_EnsureLightGridBuffer: failed to create %s buffer (%llu bytes)\n",
            label, (unsigned long long)requiredSize);
        *currentSize = 0;
        return qfalse;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk.device, *buffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };

    // Pad the backing allocation to mirror the scratch/instance buffers.
    // Validation/device-fault dumps showed WRITE_INVALID on small host-visible
    // buffers, so give the driver an extra aligned MiB of headroom.
    const VkDeviceSize pad = 1024 * 1024; // 1 MiB guard
    VkDeviceSize padAligned = (pad + memReqs.alignment - 1) & ~(memReqs.alignment - 1);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &allocFlags,
        .allocationSize = memReqs.size + padAligned,
        .memoryTypeIndex = vk_find_memory_type(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        ri.Printf(PRINT_DEVELOPER,
                  "RT_EnsureLightGridBuffer: %s alloc=%llu (raw=%llu pad=%llu align=%llu)\n",
                  label,
                  (unsigned long long)allocInfo.allocationSize,
                  (unsigned long long)memReqs.size,
                  (unsigned long long)padAligned,
                  (unsigned long long)memReqs.alignment);
    }

    if (vkAllocateMemory(vk.device, &allocInfo, NULL, memory) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RT_EnsureLightGridBuffer: failed to allocate %s memory (%llu bytes)\n",
            label, (unsigned long long)allocInfo.allocationSize);
        vkDestroyBuffer(vk.device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        *currentSize = 0;
        return qfalse;
    }

    if (vkBindBufferMemory(vk.device, *buffer, *memory, 0) != VK_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RT_EnsureLightGridBuffer: vkBindBufferMemory failed for %s\n", label);
        vkFreeMemory(vk.device, *memory, NULL);
        vkDestroyBuffer(vk.device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
        *currentSize = 0;
        return qfalse;
    }

    if (r_rtx_debug && r_rtx_debug->integer >= 2) {
        VkDeviceAddress addr = RTX_GetBufferDeviceAddressVK(*buffer);
        ri.Printf(PRINT_DEVELOPER,
                  "RT_EnsureLightGridBuffer: %s deviceAddr=0x%llx size=%llu\n",
                  label,
                  (unsigned long long)addr,
                  (unsigned long long)allocInfo.allocationSize);
    }

    *currentSize = requiredSize;
    return qtrue;
}

void RT_UpdateLightGridBuffers(void) {
    if (!vk.device) {
        return;
    }

    rtLightGrid_t *grid = &rt.lightGrid;
    if (!grid->dirty) {
        return;
    }

    if (!grid->indices || grid->indexCount == 0 || !grid->offsets || grid->offsetCount == 0) {
        RT_DestroyLightGridBuffers();
        grid->dirty = qfalse;
        return;
    }

    VkDeviceSize offsetsSize = (VkDeviceSize)grid->offsetCount * sizeof(uint32_t);
    VkDeviceSize indicesSize = (VkDeviceSize)grid->indexCount * sizeof(uint32_t);

    if (r_rtx_debug && r_rtx_debug->integer >= 1) {
        ri.Printf(PRINT_DEVELOPER,
                  "RT_UpdateLightGridBuffers: offsets=%u (bytes=%llu) indices=%u (bytes=%llu)\n",
                  grid->offsetCount, (unsigned long long)offsetsSize,
                  grid->indexCount, (unsigned long long)indicesSize);
    }

    // Pad buffers to tolerate small overruns from GPU-side address calculations.
    const VkDeviceSize guard = 1024 * 1024; // 1 MiB padding
    VkDeviceSize paddedOffsetsSize = offsetsSize + guard;
    VkDeviceSize paddedIndicesSize = indicesSize + guard;

    if (!RT_EnsureLightGridBuffer(&grid->offsetBuffer, &grid->offsetMemory,
        &grid->offsetBufferSize, paddedOffsetsSize, "light-grid offsets")) {
        grid->dirty = qfalse;
        return;
    }

    if (!RT_EnsureLightGridBuffer(&grid->indexBuffer, &grid->indexMemory,
        &grid->indexBufferSize, paddedIndicesSize, "light-grid indices")) {
        grid->dirty = qfalse;
        return;
    }

    // Grid uploads happen only when the static light set changes (world
    // load); drain the queue so no in-flight frame still reads the buffers.
    vkQueueWaitIdle(vk.queue);

    void *mapped = NULL;
    if (vkMapMemory(vk.device, grid->offsetMemory, 0, offsetsSize, 0, &mapped) == VK_SUCCESS && mapped) {
        Com_Memcpy(mapped, grid->offsets, offsetsSize);
        vkUnmapMemory(vk.device, grid->offsetMemory);
    } else {
        ri.Printf(PRINT_WARNING, "RT_UpdateLightGridBuffers: failed to map offset memory\n");
    }

    mapped = NULL;
    if (vkMapMemory(vk.device, grid->indexMemory, 0, indicesSize, 0, &mapped) == VK_SUCCESS && mapped) {
        Com_Memcpy(mapped, grid->indices, indicesSize);
        vkUnmapMemory(vk.device, grid->indexMemory);
    } else {
        ri.Printf(PRINT_WARNING, "RT_UpdateLightGridBuffers: failed to map index memory\n");
    }

    grid->dirty = qfalse;
}
#endif

/*
===============
RT_InitPathTracer

Initialize the path tracing system
===============
*/
void RT_InitPathTracer(void) {
    Com_Memset(&rt, 0, sizeof(rt));
    RT_SetBackendStatus("Software backend initialising");
    
    // Register CVARs
    rt_enable = ri.Cvar_Get("rt_enable", "1", CVAR_ARCHIVE);
    rt_mode = ri.Cvar_Get("rt_mode", "all", CVAR_ARCHIVE);
    rt_quality = ri.Cvar_Get("rt_quality", "2", CVAR_ARCHIVE);
    rt_bounces = ri.Cvar_Get("rt_bounces", "2", CVAR_ARCHIVE);
    rt_samples = ri.Cvar_Get("rt_samples", "1", CVAR_ARCHIVE);
    rt_denoise = ri.Cvar_Get("rt_denoise", "1", CVAR_ARCHIVE);
    rt_temporal = ri.Cvar_Get("rt_temporal", "1", CVAR_ARCHIVE);
    r_rt_backend = ri.Cvar_Get("r_rt_backend", "auto", CVAR_ARCHIVE);
    rt_probes = ri.Cvar_Get("rt_probes", "1", CVAR_ARCHIVE);
    rt_cache = ri.Cvar_Get("rt_cache", "1", CVAR_ARCHIVE);
    rt_debug = ri.Cvar_Get("rt_debug", "0", CVAR_CHEAT);
    rt_staticLights = ri.Cvar_Get("rt_staticLights", "1", CVAR_ARCHIVE);
    rt_gpuValidate = ri.Cvar_Get("rt_gpuValidate", "0", CVAR_ARCHIVE);
    rt_staticLightScale = ri.Cvar_Get("rt_staticLightScale", "1.6", CVAR_ARCHIVE);
    rt_staticLightRadiusScale = ri.Cvar_Get("rt_staticLightRadiusScale", "1.0", CVAR_ARCHIVE);
    rt_lightGridCellSize = ri.Cvar_Get("rt_lightGridCellSize", "192", CVAR_ARCHIVE);
    rt_skyLightScale = ri.Cvar_Get("rt_skyLightScale", "1.5", CVAR_ARCHIVE);
    rt_staticLightAutoScale = ri.Cvar_Get("rt_staticLightAutoScale", "1", CVAR_ARCHIVE);
    // Calibrated for the GPU PBR pipeline: point-light intensity I yields
    // roughly I * albedo/PI * attenuation on a lit surface, so an average of
    // 6 keeps interiors readable without saturating the tonemapper.
    rt_staticLightAutoTarget = ri.Cvar_Get("rt_staticLightAutoTarget", "8", CVAR_ARCHIVE);
    rt_skyAmbientFactor = ri.Cvar_Get("rt_skyAmbientFactor", "0.015", CVAR_ARCHIVE);
    rt_exposure = ri.Cvar_Get("rt_exposure", "1.4", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_exposure, "Linear exposure applied to the path-traced image before tonemapping.");
    rt_dlightIntensity = ri.Cvar_Get("rt_dlightIntensity", "12", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_dlightIntensity, "Intensity scale for game effect lights (muzzle flashes, rockets, explosions).");
    rt_volumetric = ri.Cvar_Get("rt_volumetric", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_volumetric, "Volumetric light scattering (sun shafts, fog glow from effect lights).");
    rt_volumetricDensity = ri.Cvar_Get("rt_volumetricDensity", "1.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_volumetricDensity, "Volumetric fog density scale.");
    rt_volumetricScatter = ri.Cvar_Get("rt_volumetricScatter", "1.0", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_volumetricScatter, "Volumetric in-scatter brightness scale (light carried by the fog itself).");
    rt_cloudCoverage = ri.Cvar_Get("rt_cloudCoverage", "0.32", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_cloudCoverage, "Dynamic sky cloud coverage (0 = clear, 1 = overcast).");
    rt_reflections = ri.Cvar_Get("rt_reflections", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_reflections, "Ray-traced specular/glossy/mirror reflections on metal and smooth surfaces.");
    rt_caustics = ri.Cvar_Get("rt_caustics", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_caustics, "Animated caustic lighting on underwater surfaces.");
    rt_volumetricFX = ri.Cvar_Get("rt_volumetricFX", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(rt_volumetricFX, "Replace weapon explosion/smoke sprites with path-traced volumetric fireballs and smoke.");
    rt_pbrMaps = ri.Cvar_Get("rt_pbrMaps", "1", CVAR_ARCHIVE | CVAR_LATCH);
    ri.Cvar_SetDescription(rt_pbrMaps, "Load generated PBR companion maps (_n/_r/_metal/_ao) for material textures (needs map reload).");
    r_rt_mode = ri.Cvar_Get("r_rt_mode", rt_mode->string, CVAR_ARCHIVE);
    if (Q_stricmp(r_rt_mode->string, rt_mode->string)) {
        ri.Cvar_Set("rt_mode", r_rt_mode->string);
    }
    
    ri.Cvar_SetDescription(rt_mode, "Path tracing mode: 'off', 'dynamic', or 'all'");
    ri.Cvar_SetDescription(r_rt_backend, "Ray tracing backend: 'auto', 'hardware', or 'software'");
    ri.Cvar_SetDescription(rt_gpuValidate, "Frame validation stride for CPU reference and backend parity checks (0 disables validation).");
    ri.Cvar_SetDescription(rt_staticLightScale, "Scalar applied to legacy BSP light intensities after tone mapping.");
    ri.Cvar_SetDescription(rt_staticLightRadiusScale, "Scalar applied to BSP light radii after inference.");
    ri.Cvar_SetDescription(rt_lightGridCellSize, "Target world-space size (in units) for light-grid cells.");
    ri.Cvar_SetDescription(rt_skyLightScale, "Scalar multiplier applied to the inferred skylight intensity.");
    ri.Cvar_SetDescription(rt_staticLightAutoScale, "Enable automatic rescaling of extracted static light intensities to reach the target average.");
    ri.Cvar_SetDescription(rt_staticLightAutoTarget, "Target average intensity for auto-scaled static lights (set to 0 to disable target clamping).");
    ri.Cvar_SetDescription(rt_skyAmbientFactor, "Multiplier applied to the inferred skylight intensity for ambient lighting contribution.");
    
#ifndef USE_VULKAN
    if (rt_enable->integer) {
        ri.Error(ERR_FATAL, "RTX path tracer requires Vulkan RT support; set rt_enable 0 to continue without it.");
    }
#else
    if (rt_enable->integer) {
        qboolean initOk = RTX_Init();
        if (!initOk) {
            if (rtx_enable && rtx_enable->integer) {
                ri.Error(ERR_FATAL, "RTX hardware backend not detected. Disable rtx_enable to continue.");
            }
        } else if (!RTX_IsAvailable()) {
            ri.Error(ERR_FATAL, "RTX hardware backend not detected. Disable rtx_enable to continue.");
        }
    }
#endif

    // Set default quality
    rt.quality = RT_QUALITY_MEDIUM;
    rt.mode = RT_MODE_DYNAMIC;
    rt.maxBounces = 2;
    rt.samplesPerPixel = 1;
    rt.enabled = qfalse;
    rt.useRTX = qfalse;
    
    // Parse mode CVAR
    if (!Q_stricmp(rt_mode->string, "all")) {
        rt.mode = RT_MODE_ALL;
    } else if (!Q_stricmp(rt_mode->string, "dynamic")) {
        rt.mode = RT_MODE_DYNAMIC;
    } else {
        rt.mode = RT_MODE_OFF;
    }
    
    // Allocate static light storage
    rt.maxStaticLights = RT_MAX_STATIC_LIGHTS;
    rt.staticLights = ri.Hunk_Alloc(sizeof(staticLight_t) * rt.maxStaticLights, h_low);
    rt.numStaticLights = 0;
    rt.numDynamicLights = 0;
    rt.numSceneLights = 0;
    rt.sceneLightHash = 0;
    rt.temporalWidth = 0;
    rt.temporalHeight = 0;
    rt.temporalEnabled = qtrue;
    
    // Initialize random seed
    g_seed = ri.Milliseconds();
    
    // Register console command
    ri.Cmd_AddCommand("rt_status", RT_Status_f);

#ifdef USE_VULKAN
    RT_InitSceneLightBuffer();
    RT_UpdateSceneLightBuffer();
#endif

    RT_InitDenoiser();

    RT_SelectBackend();
    
    ri.Printf(PRINT_ALL, "Path tracer initialized (mode: %s, backend: %s)\n",
        rt_mode->string, (rt.useRTX ? "RTX Hardware" : "Software"));
}

void RT_ShutdownBackend(void) {
#ifdef USE_VULKAN
    if (rtBackendActive) {
        ri.Printf(PRINT_ALL, "RTX hardware backend disabled\n");
    }
    RTX_Shutdown();
#endif
    rtBackendActive = qfalse;
    rt.useRTX = qfalse;
    RT_ResetBackendLogs();
    rt.sceneLightBufferDirty = qtrue;
    RT_SetBackendStatus("Software backend active");
}

/*
===============
RT_ShutdownPathTracer

Shutdown and free resources
===============
*/
void RT_ShutdownPathTracer(void) {
    RT_ShutdownBackend();

    rt.frameActive = qfalse;

#ifdef USE_VULKAN
    RT_DestroyLightGridBuffers();
    RT_DestroySceneLightBuffer();
#endif

    RT_DestroyLightGridHostData();

    if (rt.lightCache) {
        ri.Free(rt.lightCache);
        rt.lightCache = NULL;
    }
    
    if (rt.probes) {
        ri.Free(rt.probes);
        rt.probes = NULL;
    }
    
    if (rt.accumBuffer) {
        ri.Free(rt.accumBuffer);
        rt.accumBuffer = NULL;
    }

    if (rt.varianceBuffer) {
        ri.Free(rt.varianceBuffer);
        rt.varianceBuffer = NULL;
    }

    if (rt.sampleBuffer) {
        ri.Free(rt.sampleBuffer);
        rt.sampleBuffer = NULL;
    }

    if (rt.denoisedBuffer) {
        ri.Free(rt.denoisedBuffer);
        rt.denoisedBuffer = NULL;
    }
}

#ifdef USE_VULKAN
/*
================
RT_BackendFrameReady

True when the hardware backend will actually composite a path-traced image
this frame: backend active and a valid 3D world view was captured. Used by
vk_pathtracer_apply to avoid breaking the render pass for nothing.
================
*/
qboolean RT_BackendFrameReady(void) {
    if (!rt_enable || !rt_enable->integer) {
        return qfalse;
    }

    if (!rtBackendActive || !rt.useRTX) {
        return qfalse;
    }

    if (!RTX_IsAvailable()) {
        return qfalse;
    }

    return RTX_HasValidViewParms();
}

void RT_RecordBackendCommands(VkCommandBuffer cmd) {
    if (!cmd) {
        return;
    }

    if (!rt_enable || !rt_enable->integer) {
        return;
    }

    if (!rtBackendActive || !rt.useRTX) {
        return;
    }

    if (!RTX_IsAvailable()) {
        return;
    }

    RTX_RecordCommands(cmd);
}

void RT_ApplyBackendDebugOverlay(VkCommandBuffer cmd, VkImage colorImage) {
    if (!cmd) {
        return;
    }

    if (!rtBackendActive || !rt.useRTX) {
        return;
    }

    if (!rtx_debug || rtx_debug->integer <= 0) {
        return;
    }

    if (!RTX_IsAvailable()) {
        return;
    }

    RTX_ApplyDebugOverlayCompute(cmd, colorImage);
}
#endif

/*
===============
RT_BuildAccelerationStructure

Build BSP acceleration structure for ray tracing
Uses existing BSP tree with additional optimization
===============
*/
void RT_BuildAccelerationStructure(void) {
    if (!tr.world) {
        return;
    }
    
    // We'll use the existing BSP tree directly
    // No need to build a separate structure
    rt.bspTree = (rtBspNode_t *)tr.world->nodes;
    rt.numNodes = tr.world->numnodes;
    
    // Allocate static light array if needed
    if (!rt.staticLights) {
        rt.maxStaticLights = RT_MAX_STATIC_LIGHTS;
        rt.staticLights = ri.Hunk_Alloc(sizeof(staticLight_t) * rt.maxStaticLights, h_low);
    }
    
    // Extract static lights if mode is set to all
    const char *modeStr = rt_mode ? rt_mode->string : "dynamic";
    if (!Q_stricmp(modeStr, "all")) {
        RT_ExtractStaticLights();
    }
    
    // Initialize light cache
    RT_InitLightCache();
    
    // Initialize probe grid if enabled
    if (rt_probes && rt_probes->integer) {
        vec3_t worldMins, worldMaxs;
        VectorCopy(tr.world->nodes[0].mins, worldMins);
        VectorCopy(tr.world->nodes[0].maxs, worldMaxs);
        RT_InitProbeGrid(worldMins, worldMaxs);
    }
}

/*
===============
RT_RayTriangleIntersect

Möller-Trumbore ray-triangle intersection
Optimized for SSE when available
===============
*/
qboolean RT_RayTriangleIntersect(const ray_t *ray, const vec3_t v0, const vec3_t v1, const vec3_t v2, float *t, vec2_t *uv) {
    vec3_t edge1, edge2, h, s, q;
    float a, f, u, v;
    
    VectorSubtract(v1, v0, edge1);
    VectorSubtract(v2, v0, edge2);
    
    CrossProduct(ray->direction, edge2, h);
    a = DotProduct(edge1, h);
    
    if (a > -0.00001f && a < 0.00001f) {
        return qfalse;
    }
    
    f = 1.0f / a;
    VectorSubtract(ray->origin, v0, s);
    u = f * DotProduct(s, h);
    
    if (u < 0.0f || u > 1.0f) {
        return qfalse;
    }
    
    CrossProduct(s, edge1, q);
    v = f * DotProduct(ray->direction, q);
    
    if (v < 0.0f || u + v > 1.0f) {
        return qfalse;
    }
    
    float rayT = f * DotProduct(edge2, q);
    
    if (rayT > ray->tMin && rayT < ray->tMax) {
        *t = rayT;
        if (uv) {
            uv[0][0] = u;
            uv[0][1] = v;
        }
        return qtrue;
    }
    
    return qfalse;
}

/*
===============
RT_RayBoxIntersect

Fast ray-AABB intersection using slab method
===============
*/
qboolean RT_RayBoxIntersect(const ray_t *ray, const vec3_t mins, const vec3_t maxs, float *tMin, float *tMax) {
    float t1, t2;
    float tNear = ray->tMin;
    float tFar = ray->tMax;
    
    for (int i = 0; i < 3; i++) {
        if (fabs(ray->direction[i]) < 0.00001f) {
            if (ray->origin[i] < mins[i] || ray->origin[i] > maxs[i]) {
                return qfalse;
            }
        } else {
            t1 = (mins[i] - ray->origin[i]) / ray->direction[i];
            t2 = (maxs[i] - ray->origin[i]) / ray->direction[i];
            
            if (t1 > t2) {
                float temp = t1;
                t1 = t2;
                t2 = temp;
            }
            
            if (t1 > tNear) tNear = t1;
            if (t2 < tFar) tFar = t2;
            
            if (tNear > tFar || tFar < 0) {
                return qfalse;
            }
        }
    }
    
    if (tMin) *tMin = tNear;
    if (tMax) *tMax = tFar;
    
    return qtrue;
}

/*
===============
RT_TraceSurface

Test ray against a surface (triangle mesh or patch)
===============
*/
static qboolean RT_TraceSurface(const ray_t *ray, msurface_t *surf, hitInfo_t *hit) {
    if (!surf || !surf->data) {
        return qfalse;
    }
    
    surfaceType_t *surface = surf->data;
    
    switch (*surface) {
    case SF_FACE: {
        srfSurfaceFace_t *face = (srfSurfaceFace_t *)surface;
        
        // Test all triangles in the face
        int *indices = (int *)((byte *)face + face->ofsIndices);
        
        for (int i = 0; i < face->numIndices; i += 3) {
            vec3_t v0, v1, v2;
            VectorCopy(face->points[indices[i]], v0);
            VectorCopy(face->points[indices[i+1]], v1);
            VectorCopy(face->points[indices[i+2]], v2);
            
            float t;
            vec2_t uv;
            if (RT_RayTriangleIntersect(ray, v0, v1, v2, &t, &uv)) {
                if (t < hit->t) {
                    hit->t = t;
                    VectorMA(ray->origin, t, ray->direction, hit->point);
                    
                    // Calculate normal
                    vec3_t edge1, edge2;
                    VectorSubtract(v1, v0, edge1);
                    VectorSubtract(v2, v0, edge2);
                    CrossProduct(edge1, edge2, hit->normal);
                    VectorNormalize(hit->normal);
                    
                    hit->shader = surf->shader;
                    hit->uv[0] = uv[0];
                    hit->uv[1] = uv[1];
                    
                    return qtrue;
                }
            }
        }
        break;
    }
    
    case SF_TRIANGLES: {
        srfTriangles_t *tris = (srfTriangles_t *)surface;
        
        // Quick bounds check
        float tMin, tMax;
        if (!RT_RayBoxIntersect(ray, tris->bounds[0], tris->bounds[1], &tMin, &tMax)) {
            return qfalse;
        }
        
        // Test all triangles
        for (int i = 0; i < tris->numIndexes; i += 3) {
            vec3_t v0, v1, v2;
            VectorCopy(tris->verts[tris->indexes[i]].xyz, v0);
            VectorCopy(tris->verts[tris->indexes[i+1]].xyz, v1);
            VectorCopy(tris->verts[tris->indexes[i+2]].xyz, v2);
            
            float t;
            vec2_t uv;
            if (RT_RayTriangleIntersect(ray, v0, v1, v2, &t, &uv)) {
                if (t < hit->t) {
                    hit->t = t;
                    VectorMA(ray->origin, t, ray->direction, hit->point);
                    
                    // Interpolate normal from vertices
                    vec3_t n0, n1, n2;
                    VectorCopy(tris->verts[tris->indexes[i]].normal, n0);
                    VectorCopy(tris->verts[tris->indexes[i+1]].normal, n1);
                    VectorCopy(tris->verts[tris->indexes[i+2]].normal, n2);
                    
                    hit->normal[0] = n0[0] * (1-uv[0]-uv[1]) + n1[0] * uv[0] + n2[0] * uv[1];
                    hit->normal[1] = n0[1] * (1-uv[0]-uv[1]) + n1[1] * uv[0] + n2[1] * uv[1];
                    hit->normal[2] = n0[2] * (1-uv[0]-uv[1]) + n1[2] * uv[0] + n2[2] * uv[1];
                    VectorNormalize(hit->normal);
                    
                    hit->shader = surf->shader;
                    hit->uv[0] = uv[0];
                    hit->uv[1] = uv[1];
                    
                    return qtrue;
                }
            }
        }
        break;
    }
    
    case SF_GRID: {
        // Grid meshes would need special handling
        // For now, skip them in path tracing
        break;
    }
    }
    
    return qfalse;
}

/*
===============
RT_TraceBSPNode

Traverse BSP tree to find ray intersection
Optimized for cache coherency
===============
*/
static qboolean RT_TraceBSPNode(const ray_t *ray, const mnode_t *node, hitInfo_t *hit) {
    if (!node) {
        return qfalse;
    }

    // Quick reject against the node bounds
    float tMin, tMax;
    if (!RT_RayBoxIntersect(ray, node->mins, node->maxs, &tMin, &tMax)) {
        return qfalse;
    }

    qboolean hitFound = qfalse;

    if (node->contents == -1) {
        // Interior node: traverse children in front-to-back order
        const cplane_t *plane = node->plane;
        float d1 = DotProduct(ray->origin, plane->normal) - plane->dist;
        float d2 = DotProduct(ray->direction, plane->normal);

        if (fabsf(d2) < 1e-5f) {
            // Ray is nearly parallel to the plane – visit the side we're on
            int side = (d1 >= 0.0f) ? 0 : 1;
            hitFound = RT_TraceBSPNode(ray, node->children[side], hit);
        } else {
            float tPlane = -d1 / d2;

            int nearSide = (d1 >= 0.0f) ? 0 : 1;
            int farSide = 1 - nearSide;

            if (RT_TraceBSPNode(ray, node->children[nearSide], hit)) {
                hitFound = qtrue;
            }

            if (tPlane > 0.0f && tPlane < hit->t) {
                if (RT_TraceBSPNode(ray, node->children[farSide], hit)) {
                    hitFound = qtrue;
                }
            }
        }
    }

    // Test surfaces attached to this node/leaf
    for (int i = 0; i < node->nummarksurfaces; i++) {
        msurface_t *surf = node->firstmarksurface[i];
        if (RT_TraceSurface(ray, surf, hit)) {
            hitFound = qtrue;
        }
    }

    return hitFound;
}

/*
===============
RT_TraceRay

Main ray tracing function
===============
*/
qboolean RT_TraceRay(const ray_t *ray, hitInfo_t *hit) {
    if (!tr.world) {
        return qfalse;
    }
    
    // Increment ray counter for statistics
    rt.raysTraced++;
    
    // Initialize hit info
    hit->t = ray->tMax;
    hit->shader = NULL;
    
    // Use RTX hardware acceleration if available
    if (rt.useRTX && RTX_IsAvailable()) {
        RTX_AcceleratePathTracing(ray, hit);
        if (hit->shader) {
            return qtrue;
        }
    }
    
    // Fallback to software BSP traversal
    return RT_TraceBSPNode(ray, &tr.world->nodes[0], hit);
}

/*
===============
RT_TraceShadowRay

Fast shadow ray test - early exit on any hit
===============
*/
qboolean RT_TraceShadowRay(const vec3_t origin, const vec3_t target, float maxDist) {
    // Use RTX hardware shadow query if available
    if (RTX_IsAvailable()) {
        float visibility = 0.0f;
        RTX_ShadowRayQuery(origin, target, &visibility);
        return (visibility < 1.0f);
    }

    if (RTX_RayQuerySupported()) {
        rtShadowQuery_t query;
        VectorCopy(origin, query.origin);
        vec3_t dir;
        VectorSubtract(target, origin, dir);
        float dist = VectorNormalize(dir);
        if (dist <= 0.0f) {
            return qfalse;
        }
        VectorCopy(dir, query.direction);
        query.maxDistance = maxDist > 0.0f ? maxDist : dist;
        query.occluded = qfalse;
        if (RTX_DispatchShadowQueries(&query, 1)) {
            return query.occluded;
        }
    }
    
    // Fallback to software implementation
    vec3_t dir;
    VectorSubtract(target, origin, dir);
    VectorNormalize(dir);

    return RT_TraceShadowRaySoftware(origin, dir, maxDist);
}

/*
===============
RT_EvaluateBRDF

Evaluate Cook-Torrance BRDF for physically-based shading
Optimized with approximations for real-time performance
===============
*/
void RT_EvaluateBRDF(const vec3_t wi, const vec3_t wo, const vec3_t normal, 
                     const vec3_t albedo, float roughness, float metallic, vec3_t result) {
    // Simplified BRDF for performance
    float NdotL = DotProduct(normal, wi);
    float NdotV = DotProduct(normal, wo);
    
    if (NdotL <= 0 || NdotV <= 0) {
        VectorClear(result);
        return;
    }
    
    // Half vector
    vec3_t H;
    VectorAdd(wi, wo, H);
    VectorNormalize(H);
    
    float NdotH = DotProduct(normal, H);
    float VdotH = DotProduct(wo, H);
    
    // Fresnel (Schlick approximation)
    vec3_t F0;
    if (metallic > 0.5f) {
        VectorCopy(albedo, F0);
    } else {
        VectorSet(F0, 0.04f, 0.04f, 0.04f);
    }
    
    float fresnel = F0[0] + (1.0f - F0[0]) * pow(1.0f - VdotH, 5.0f);
    
    // Distribution (GGX)
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (alpha2 - 1.0f) + 1.0f;
    float D = alpha2 / (M_PI * denom * denom);
    
    // Geometry (Smith)
    float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    float G1L = NdotL / (NdotL * (1.0f - k) + k);
    float G1V = NdotV / (NdotV * (1.0f - k) + k);
    float G = G1L * G1V;
    
    // Combine terms
    float specular = (D * G * fresnel) / (4.0f * NdotL * NdotV + 0.001f);
    float diffuse = (1.0f - fresnel) * (1.0f - metallic) / M_PI;
    
    result[0] = albedo[0] * (diffuse + specular) * NdotL;
    result[1] = albedo[1] * (diffuse + specular) * NdotL;
    result[2] = albedo[2] * (diffuse + specular) * NdotL;
}

/*
===============
RT_SampleBRDF

Importance sample the BRDF for next ray direction
===============
*/
void RT_SampleBRDF(const vec3_t wo, const vec3_t normal, float roughness, 
                   vec3_t wi, float *pdf, vec3_t result) {
    // For now, use cosine-weighted hemisphere sampling
    // This is simple and works well for diffuse surfaces
    RT_CosineSampleHemisphere(normal, wi);
    
    // PDF for cosine-weighted sampling
    *pdf = DotProduct(wi, normal) / M_PI;
    
    // The result would be the sampled direction
    VectorCopy(wi, result);
}

/*
===============
RT_EvaluateDirectLighting

Calculate direct lighting from all light sources
Optimized with light culling and caching
===============
*/
void RT_EvaluateDirectLighting(const hitInfo_t *hit, const vec3_t wo, vec3_t result) {
    VectorClear(result);
    
    if (!hit->shader || rt.mode == RT_MODE_OFF) {
        return;
    }
    
    if (rt.numSceneLights <= 0) {
        return;
    }
    
    vec3_t albedo = {1, 1, 1};
    float roughness = 0.5f;
    float metallic = 0.0f;
    vec3_t shadowOrigin;
    VectorMA(hit->point, 0.001f, hit->normal, shadowOrigin);

    int totalLights = rt.numSceneLights;
    rtLightEval_t *evaluations = (rtLightEval_t *)ri.Hunk_AllocateTempMemory(totalLights * sizeof(rtLightEval_t));
    rtShadowQuery_t *shadowQueries = (rtShadowQuery_t *)ri.Hunk_AllocateTempMemory(totalLights * sizeof(rtShadowQuery_t));
    int evalCount = 0;
    int queryCount = 0;

    for (int i = 0; i < totalLights; i++) {
        const rtSceneLight_t *light = &rt.sceneLights[i];
        if (light->intensity <= 0.0f) {
            continue;
        }

        vec3_t lightDir;
        float distance = RT_DIRECTIONAL_MAX_DISTANCE;
        qboolean valid = qtrue;

        switch (light->type) {
        case RT_LIGHT_TYPE_POINT:
        case RT_LIGHT_TYPE_SPOT:
            VectorSubtract(light->origin, hit->point, lightDir);
            distance = VectorLength(lightDir);
            if (distance <= 0.0f || distance > light->radius) {
                valid = qfalse;
                break;
            }
            VectorScale(lightDir, 1.0f / distance, lightDir);

            if (light->type == RT_LIGHT_TYPE_SPOT) {
                float dot = DotProduct(lightDir, light->direction);
                if (dot < light->spotCos) {
                    valid = qfalse;
                    break;
                }
            }
            break;
        case RT_LIGHT_TYPE_DIRECTIONAL:
            VectorCopy(light->direction, lightDir);
            if (VectorNormalize(lightDir) <= 0.0f) {
                valid = qfalse;
            }
            else {
                VectorScale(lightDir, -1.0f, lightDir);
            }
            distance = RT_DIRECTIONAL_MAX_DISTANCE;
            break;
        default:
            valid = qfalse;
            break;
        }

        if (!valid) {
            continue;
        }

        rtLightEval_t *eval = &evaluations[evalCount];
        eval->light = light;
        VectorCopy(lightDir, eval->direction);
        eval->distance = distance;
        eval->queryIndex = -1;

        if (light->castsShadows) {
            rtShadowQuery_t *query = &shadowQueries[queryCount];
            VectorCopy(shadowOrigin, query->origin);
            VectorCopy(lightDir, query->direction);
            query->maxDistance = distance;
            query->occluded = qfalse;
            eval->queryIndex = queryCount;
            queryCount++;
        }

        evalCount++;
    }

    if (queryCount > 0) {
        qboolean gpuHandled = qfalse;
        if (RTX_RayQuerySupported()) {
            gpuHandled = RTX_DispatchShadowQueries(shadowQueries, queryCount);
        }

        if (!gpuHandled) {
            for (int i = 0; i < queryCount; i++) {
                shadowQueries[i].occluded = RT_TraceShadowRaySoftware(
                    shadowQueries[i].origin,
                    shadowQueries[i].direction,
                    shadowQueries[i].maxDistance);
            }
        }
    }

    for (int i = 0; i < evalCount; i++) {
        const rtLightEval_t *eval = &evaluations[i];
        const rtSceneLight_t *light = eval->light;

        if (eval->queryIndex >= 0 && shadowQueries[eval->queryIndex].occluded) {
            continue;
        }

        vec3_t brdf;
        RT_EvaluateBRDF(eval->direction, wo, hit->normal, albedo, roughness, metallic, brdf);

        float attenuation = 1.0f;
        if (light->type == RT_LIGHT_TYPE_POINT || light->type == RT_LIGHT_TYPE_SPOT) {
            attenuation = 1.0f - (eval->distance / light->radius);
            attenuation = attenuation * attenuation;
        }

        vec3_t lightContrib;
        VectorCopy(light->color, lightContrib);
        VectorScale(lightContrib, light->intensity * attenuation, lightContrib);

        result[0] += lightContrib[0] * brdf[0];
        result[1] += lightContrib[1] * brdf[1];
        result[2] += lightContrib[2] * brdf[2];
    }

    if (rt.skyAmbientIntensity > 0.0f) {
        vec3_t ambient;
        VectorCopy(rt.skyAmbientColor, ambient);
        VectorScale(ambient, rt.skyAmbientIntensity, ambient);
        result[0] += ambient[0] * albedo[0];
        result[1] += ambient[1] * albedo[1];
        result[2] += ambient[2] * albedo[2];
    }

    ri.Hunk_FreeTempMemory(shadowQueries);
    ri.Hunk_FreeTempMemory(evaluations);
}

/*
===============
RT_EvaluateIndirectLighting

Calculate indirect lighting using path tracing
Limited bounces for performance
===============
*/
void RT_EvaluateIndirectLighting(const hitInfo_t *hit, const vec3_t wo, int depth, vec3_t result) {
    VectorClear(result);
    
    // Russian roulette for path termination
    if (depth > 2) {
        float p = 0.5f; // Termination probability
        if (FastRandom() > p) {
            return;
        }
    }
    
    // Sample new direction
    vec3_t wi;
    float pdf;
    vec3_t sample;
    RT_SampleBRDF(wo, hit->normal, 0.5f, wi, &pdf, sample);
    
    // Trace secondary ray
    ray_t ray;
    VectorCopy(hit->point, ray.origin);
    VectorCopy(wi, ray.direction);
    ray.tMin = 0.001f;
    ray.tMax = 10000.0f;
    ray.depth = depth + 1;
    
    hitInfo_t nextHit;
    if (RT_TraceRay(&ray, &nextHit)) {
        // Recursive evaluation
        vec3_t Li;
        RT_TracePath(&ray, depth + 1, Li);
        
        // Apply BRDF and PDF
        vec3_t brdf;
        RT_EvaluateBRDF(wi, wo, hit->normal, (vec3_t){1,1,1}, 0.5f, 0.0f, brdf);
        
        float NdotL = DotProduct(hit->normal, wi);
        if (NdotL > 0 && pdf > 0.001f) {
            VectorScale(Li, NdotL / pdf, result);
        }
    }
}

/*
===============
RT_TracePath

Main path tracing function - traces a complete light path
===============
*/
void RT_TracePath(const ray_t *ray, int depth, vec3_t result) {
    VectorClear(result);
    
    if (depth > rt.maxBounces) {
        return;
    }
    
    hitInfo_t hit;
    if (!RT_TraceRay(ray, &hit)) {
        // Sky color or environment
        VectorSet(result, 0.5f, 0.7f, 1.0f);
        return;
    }
    
    // View direction (opposite of ray)
    vec3_t wo;
    VectorScale(ray->direction, -1, wo);
    
    // Direct lighting
    vec3_t direct;
    RT_EvaluateDirectLighting(&hit, wo, direct);
    
    // Indirect lighting (if quality allows)
    vec3_t indirect = {0, 0, 0};
    if (rt.quality >= RT_QUALITY_HIGH && depth < rt.maxBounces) {
        RT_EvaluateIndirectLighting(&hit, wo, depth, indirect);
    }
    
    // Combine
    VectorAdd(direct, indirect, result);
}

/*
===============
RT_CosineSampleHemisphere

Generate cosine-weighted sample on hemisphere
===============
*/
void RT_CosineSampleHemisphere(const vec3_t normal, vec3_t result) {
    // Generate random point on unit disk
    float r1 = FastRandom();
    float r2 = FastRandom();
    
    float theta = 2.0f * M_PI * r1;
    float r = sqrt(r2);
    
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(1.0f - r2);
    
    // Transform to world space
    vec3_t tangent, bitangent;
    
    // Build orthonormal basis
    if (fabs(normal[0]) < 0.9f) {
        VectorSet(tangent, 1, 0, 0);
    } else {
        VectorSet(tangent, 0, 1, 0);
    }
    
    CrossProduct(normal, tangent, bitangent);
    VectorNormalize(bitangent);
    CrossProduct(bitangent, normal, tangent);
    
    // Transform sample to world space
    result[0] = x * tangent[0] + y * bitangent[0] + z * normal[0];
    result[1] = x * tangent[1] + y * bitangent[1] + z * normal[1];
    result[2] = x * tangent[2] + y * bitangent[2] + z * normal[2];
}

/*
===============
RT_EvaluateStaticLighting

Calculate lighting from static light sources (extracted from BSP)
===============
*/
void RT_EvaluateStaticLighting(const hitInfo_t *hit, const vec3_t wo, vec3_t result) {
    VectorClear(result);
    
    if (!hit->shader || rt.numStaticLights == 0) {
        return;
    }
    
    // Get material properties from shader
    vec3_t albedo = {1, 1, 1};
    float roughness = 0.5f;
    float metallic = 0.0f;
    
    // Test all static lights
    for (int i = 0; i < rt.numStaticLights; i++) {
        staticLight_t *sl = &rt.staticLights[i];
        
        // Calculate light direction and distance
        vec3_t lightDir;
        VectorSubtract(sl->origin, hit->point, lightDir);
        float dist = VectorLength(lightDir);
        
        // Skip if out of range
        if (dist > sl->radius) {
            continue;
        }
        
        VectorNormalize(lightDir);
        
        // Check spotlight cone if applicable
        if (sl->type == 1) { // Spotlight
            float dot = DotProduct(lightDir, sl->direction);
            if (dot < cos(sl->spotAngle * M_PI / 180.0f)) {
                continue; // Outside cone
            }
        }
        
        // Shadow test if enabled
        if (sl->castShadows && RT_TraceShadowRay(hit->point, sl->origin, dist)) {
            continue; // In shadow
        }
        
        // Calculate BRDF
        vec3_t brdf;
        RT_EvaluateBRDF(lightDir, wo, hit->normal, albedo, roughness, metallic, brdf);
        
        // Apply light color and attenuation
        float atten = 1.0f - (dist / sl->radius);
        atten = atten * atten; // Quadratic falloff
        
        // Apply intensity and color
        vec3_t lightContrib;
        VectorScale(sl->color, sl->intensity * atten, lightContrib);
        
        // Multiply by BRDF
        result[0] += lightContrib[0] * brdf[0];
        result[1] += lightContrib[1] * brdf[1];
        result[2] += lightContrib[2] * brdf[2];
    }
}

/*
===============
RT_ExtractStaticLights

Extract static lights from BSP data
===============
*/
void RT_ExtractStaticLights(void) {
    if (!rt_staticLights->integer || !tr.world || !tr.world->entityString) {
        static qboolean warnedMissingEntities = qfalse;
        if (!warnedMissingEntities) {
            ri.Printf(PRINT_WARNING,
                      "RT_ExtractStaticLights: skipped (rt_staticLights=%d world=%p entityString=%p)\n",
                      rt_staticLights ? rt_staticLights->integer : 0,
                      (void*)tr.world,
                      tr.world ? (void*)tr.world->entityString : NULL);
            warnedMissingEntities = qtrue;
        }
        rt.numStaticLights = 0;
        return;
    }

    enum { MAX_LIGHT_TARGETS = 1024 };

    typedef struct {
        char    name[MAX_QPATH];
        vec3_t  origin;
    } targetRef_t;

    typedef struct {
        vec3_t      origin;
        qboolean    hasOrigin;
        vec3_t      color;
        float       intensity;
        float       radius;
        float       scale;
        float       spotAngle;
        char        target[MAX_QPATH];
        qboolean    hasTarget;
        vec3_t      angles;
        qboolean    hasAngles;
        qboolean    explicitSpot;
        qboolean    castsShadows;
        qboolean    lightJunior;
    } pendingLight_t;

    targetRef_t targets[MAX_LIGHT_TARGETS];
    int numTargets = 0;

    pendingLight_t pending[RT_MAX_STATIC_LIGHTS];
    int numPending = 0;

    const char *data = tr.world->entityString;
    const char *token;

    rt.numStaticLights = 0;

    while (1) {
        token = COM_ParseExt(&data, qtrue);
        if (!token[0]) {
            break;
        }

        if (token[0] != '{') {
            continue;
        }

        char classname[MAX_TOKEN_CHARS] = "";
        char targetname[MAX_QPATH];
        targetname[0] = '\0';

        pendingLight_t light;
        Com_Memset(&light, 0, sizeof(light));
        light.color[0] = light.color[1] = light.color[2] = 1.0f;
        light.intensity = 300.0f;
        light.radius = 0.0f;
        light.scale = 1.0f;
        light.spotAngle = 45.0f;
        light.castsShadows = qtrue;

        qboolean entityDone = qfalse;

        while (!entityDone) {
            token = COM_ParseExt(&data, qtrue);
            if (!token[0]) {
                entityDone = qtrue;
                break;
            }

            if (!Q_stricmp(token, "}")) {
                entityDone = qtrue;
                break;
            }

            char key[MAX_TOKEN_CHARS];
            Q_strncpyz(key, token, sizeof(key));

            token = COM_ParseExt(&data, qtrue);
            if (!token[0]) {
                entityDone = qtrue;
                break;
            }

            char value[MAX_TOKEN_CHARS];
            Q_strncpyz(value, token, sizeof(value));

            if (!Q_stricmp(key, "classname")) {
                Q_strncpyz(classname, value, sizeof(classname));
            } else if (!Q_stricmp(key, "origin")) {
                if (sscanf(value, "%f %f %f", &light.origin[0], &light.origin[1], &light.origin[2]) == 3) {
                    light.hasOrigin = qtrue;
                }
            } else if (!Q_stricmp(key, "_color") || !Q_stricmp(key, "color")) {
                float r, g, b;
                if (sscanf(value, "%f %f %f", &r, &g, &b) == 3) {
                    light.color[0] = r;
                    light.color[1] = g;
                    light.color[2] = b;
                }
            } else if (!Q_stricmp(key, "light") || !Q_stricmp(key, "_light")) {
                light.intensity = atof(value);
            } else if (!Q_stricmp(key, "scale") || !Q_stricmp(key, "_scale")) {
                light.scale = atof(value);
            } else if (!Q_stricmp(key, "radius") || !Q_stricmp(key, "_radius") || !Q_stricmp(key, "light_radius")) {
                light.radius = atof(value);
            } else if (!Q_stricmp(key, "target")) {
                light.hasTarget = qtrue;
                Q_strncpyz(light.target, value, sizeof(light.target));
            } else if (!Q_stricmp(key, "targetname")) {
                Q_strncpyz(targetname, value, sizeof(targetname));
            } else if (!Q_stricmp(key, "angle")) {
                float yaw = atof(value);
                light.hasAngles = qtrue;
                light.explicitSpot = qtrue;
                if (yaw == -1.0f) {
                    light.angles[PITCH] = -90.0f;
                    light.angles[YAW] = 0.0f;
                    light.angles[ROLL] = 0.0f;
                } else if (yaw == -2.0f) {
                    light.angles[PITCH] = 90.0f;
                    light.angles[YAW] = 0.0f;
                    light.angles[ROLL] = 0.0f;
                } else {
                    light.angles[PITCH] = 0.0f;
                    light.angles[YAW] = yaw;
                    light.angles[ROLL] = 0.0f;
                }
            } else if (!Q_stricmp(key, "angles")) {
                float pitch, yaw, roll;
                if (sscanf(value, "%f %f %f", &pitch, &yaw, &roll) == 3) {
                    light.angles[PITCH] = pitch;
                    light.angles[YAW] = yaw;
                    light.angles[ROLL] = roll;
                    light.hasAngles = qtrue;
                }
            } else if (!Q_stricmp(key, "spotangle") || !Q_stricmp(key, "_spotangle") || !Q_stricmp(key, "cone")) {
                light.spotAngle = atof(value);
                light.explicitSpot = qtrue;
            } else if (!Q_stricmp(key, "spawnflags")) {
                int flags = atoi(value);
                if (flags & 1) {
                    light.castsShadows = qfalse;
                }
                if (flags & 2) {
                    light.lightJunior = qtrue;
                }
            } else if (!Q_stricmp(key, "noshadows") || !Q_stricmp(key, "_noshadows")) {
                if (atoi(value) != 0) {
                    light.castsShadows = qfalse;
                }
            } else if (!Q_stricmp(key, "rt_castShadows")) {
                light.castsShadows = atoi(value) != 0;
            }
        }

        if (!classname[0]) {
            continue;
        }

        if (!Q_stricmp(classname, "target_position") ||
            !Q_stricmp(classname, "info_null") ||
            !Q_stricmp(classname, "info_notnull")) {
            if (targetname[0] && light.hasOrigin && numTargets < MAX_LIGHT_TARGETS) {
                Q_strncpyz(targets[numTargets].name, targetname, sizeof(targets[numTargets].name));
                VectorCopy(light.origin, targets[numTargets].origin);
                numTargets++;
            }
            continue;
        }

        if (Q_stricmp(classname, "light") &&
            Q_stricmp(classname, "light_spot") &&
            Q_stricmp(classname, "lightJunior")) {
            continue;
        }

        if (!light.hasOrigin) {
            continue;
        }

        if (light.scale <= 0.0f) {
            light.scale = 1.0f;
        }

        if (numPending < rt.maxStaticLights) {
            pending[numPending++] = light;
        } else if (rt_debug && rt_debug->integer) {
            ri.Printf(PRINT_WARNING, "RT_ExtractStaticLights: static light limit reached (%d)\n", rt.maxStaticLights);
            break;
        }
    }

    for (int i = 0; i < numPending && rt.numStaticLights < rt.maxStaticLights; i++) {
        pendingLight_t *src = &pending[i];
        staticLight_t *sl = &rt.staticLights[rt.numStaticLights++];

        VectorCopy(src->origin, sl->origin);
        VectorCopy(src->color, sl->color);

        float rawEnergy = src->intensity * src->scale;
        if (src->lightJunior) {
            rawEnergy *= 0.5f;
        }
        if (rawEnergy <= 0.0f) {
            float fallback = (fabsf(src->color[0]) + fabsf(src->color[1]) + fabsf(src->color[2])) * 150.0f;
            rawEnergy = (fallback > 0.0f) ? fallback : 75.0f;
        }
        sl->intensity = RT_TranslateStaticLightIntensity(rawEnergy, src->color);
        sl->radius = RT_TranslateStaticLightRadius(src->radius, rawEnergy);

        sl->type = RT_LIGHT_TYPE_POINT;
        sl->spotAngle = src->spotAngle > 0.0f ? src->spotAngle : 45.0f;
        sl->castShadows = src->castsShadows;
        VectorClear(sl->direction);

        qboolean haveDirection = qfalse;

        if (src->hasTarget) {
            for (int t = 0; t < numTargets; t++) {
                if (!Q_stricmp(src->target, targets[t].name)) {
                    VectorSubtract(targets[t].origin, sl->origin, sl->direction);
                    if (VectorNormalize(sl->direction) > 0.0f) {
                        haveDirection = qtrue;
                    }
                    break;
                }
            }
        }

        if (!haveDirection && src->hasAngles) {
            vec3_t forward;
            AngleVectors(src->angles, forward, NULL, NULL);
            VectorCopy(forward, sl->direction);
            if (VectorNormalize(sl->direction) > 0.0f) {
                haveDirection = qtrue;
            }
        }

        if (haveDirection || src->explicitSpot) {
            sl->type = RT_LIGHT_TYPE_SPOT;
            if (sl->spotAngle <= 0.0f) {
                sl->spotAngle = 45.0f;
            }
        } else {
            sl->spotAngle = 180.0f;
        }
    }

    if (rt_debug && rt_debug->integer >= 2) {
        ri.Printf(PRINT_ALL, "RT: extracted %d static lights (%d pending, %d targets)\n",
            rt.numStaticLights, numPending, numTargets);
    }

    RT_ApplyStaticLightAutoScale();

    if (rt_debug && rt_debug->integer >= 2 && rt.numStaticLights > 0) {
        float totalIntensity = 0.0f;
        float totalRadius = 0.0f;
        float minIntensity = FLT_MAX;
        float maxIntensity = 0.0f;
        float minRadius = FLT_MAX;
        float maxRadius = 0.0f;
        int spotCount = 0;
        for (int i = 0; i < rt.numStaticLights; ++i) {
            const staticLight_t *sl = &rt.staticLights[i];
            totalIntensity += sl->intensity;
            totalRadius += sl->radius;
            if (sl->intensity < minIntensity) {
                minIntensity = sl->intensity;
            }
            if (sl->intensity > maxIntensity) {
                maxIntensity = sl->intensity;
            }
            if (sl->radius < minRadius) {
                minRadius = sl->radius;
            }
            if (sl->radius > maxRadius) {
                maxRadius = sl->radius;
            }
            if (sl->type == RT_LIGHT_TYPE_SPOT) {
                spotCount++;
            }
        }
        if (minIntensity == FLT_MAX) {
            minIntensity = 0.0f;
        }
        if (minRadius == FLT_MAX) {
            minRadius = 0.0f;
        }
        float avgIntensity = totalIntensity / (float)rt.numStaticLights;
        float avgRadius = totalRadius / (float)rt.numStaticLights;
        ri.Printf(PRINT_ALL,
            "RT: static light stats | avgIntensity=%.3f min=%.3f max=%.3f | avgRadius=%.1f min=%.1f max=%.1f | spots=%d\n",
            avgIntensity, minIntensity, maxIntensity,
            avgRadius, minRadius, maxRadius, spotCount);
    }

    if (rt.mode != RT_MODE_OFF) {
        RT_RebuildSceneLights();
    }
    else {
#ifdef USE_VULKAN
        rt.sceneLightBufferDirty = qtrue;
        RT_UpdateSceneLightBuffer();
#endif
    }
}

/*
===============
RT_UniformSampleHemisphere

Generate uniform sample on hemisphere
===============
*/
void RT_UniformSampleHemisphere(const vec3_t normal, vec3_t result) {
    float r1 = FastRandom();
    float r2 = FastRandom();
    
    float theta = 2.0f * M_PI * r1;
    float phi = acos(r2);
    
    float x = sin(phi) * cos(theta);
    float y = sin(phi) * sin(theta);
    float z = cos(phi);
    
    // Transform to world space (same as cosine sampling)
    vec3_t tangent, bitangent;
    
    if (fabs(normal[0]) < 0.9f) {
        VectorSet(tangent, 1, 0, 0);
    } else {
        VectorSet(tangent, 0, 1, 0);
    }
    
    CrossProduct(normal, tangent, bitangent);
    VectorNormalize(bitangent);
    CrossProduct(bitangent, normal, tangent);
    
    result[0] = x * tangent[0] + y * bitangent[0] + z * normal[0];
    result[1] = x * tangent[1] + y * bitangent[1] + z * normal[1];
    result[2] = x * tangent[2] + y * bitangent[2] + z * normal[2];
}

/*
===============
Light Cache Functions
Fast spatial hash for temporal coherence
===============
*/
void RT_InitLightCache(void) {
    if (rt.lightCache) {
        ri.Free(rt.lightCache);
    }
    
    rt.cacheSize = RT_CACHE_SIZE;
    rt.lightCache = ri.Malloc(sizeof(lightCacheEntry_t) * rt.cacheSize);
    Com_Memset(rt.lightCache, 0, sizeof(lightCacheEntry_t) * rt.cacheSize);
}

void RT_UpdateLightCache(const vec3_t pos, const vec3_t normal, const vec3_t irradiance) {
    // Simple spatial hash
    unsigned int hash = (unsigned int)(pos[0] * 73.0f + pos[1] * 179.0f + pos[2] * 283.0f);
    hash = hash % rt.cacheSize;
    
    lightCacheEntry_t *entry = &rt.lightCache[hash];
    
    // Update or replace entry
    if (VectorDistance(entry->position, pos) < 10.0f) {
        // Blend with existing
        float blend = 0.1f;
        VectorLerp(entry->irradiance, irradiance, blend, entry->irradiance);
        entry->confidence = MIN(1.0f, entry->confidence + 0.1f);
    } else {
        // Replace
        VectorCopy(pos, entry->position);
        VectorCopy(normal, entry->normal);
        VectorCopy(irradiance, entry->irradiance);
        entry->confidence = 0.5f;
    }
    
    entry->frameUpdated = rt.currentFrame;
    entry->sampleCount++;
}

qboolean RT_QueryLightCache(const vec3_t pos, const vec3_t normal, vec3_t irradiance) {
    unsigned int hash = (unsigned int)(pos[0] * 73.0f + pos[1] * 179.0f + pos[2] * 283.0f);
    hash = hash % rt.cacheSize;
    
    lightCacheEntry_t *entry = &rt.lightCache[hash];
    
    if (VectorDistance(entry->position, pos) < 10.0f &&
        DotProduct(entry->normal, normal) > 0.9f &&
        entry->confidence > 0.3f) {
        VectorCopy(entry->irradiance, irradiance);
        return qtrue;
    }
    
    return qfalse;
}

/*
===============
Probe Grid Functions
Irradiance probes for global illumination
===============
*/
void RT_InitProbeGrid(const vec3_t mins, const vec3_t maxs) {
    VectorCopy(mins, rt.probeGridOrigin);
    VectorSubtract(maxs, mins, rt.probeGridSize);
    
    // Calculate probe spacing
    float spacing = rt.probeGridSize[0] / RT_PROBE_GRID_SIZE;
    
    rt.numProbes = RT_PROBE_GRID_SIZE * RT_PROBE_GRID_SIZE * RT_PROBE_GRID_SIZE;
    rt.probes = ri.Malloc(sizeof(irradianceProbe_t) * rt.numProbes);
    
    // Initialize probe positions
    int index = 0;
    for (int z = 0; z < RT_PROBE_GRID_SIZE; z++) {
        for (int y = 0; y < RT_PROBE_GRID_SIZE; y++) {
            for (int x = 0; x < RT_PROBE_GRID_SIZE; x++) {
                irradianceProbe_t *probe = &rt.probes[index++];
                
                probe->position[0] = rt.probeGridOrigin[0] + x * spacing;
                probe->position[1] = rt.probeGridOrigin[1] + y * spacing;
                probe->position[2] = rt.probeGridOrigin[2] + z * spacing;
                
                // Clear irradiance
                for (int i = 0; i < 6; i++) {
                    VectorClear(probe->irradiance[i]);
                    probe->visibility[i] = 1.0f;
                }
                
                probe->lastUpdate = 0;
                probe->dynamic = qfalse;
            }
        }
    }
}

void RT_UpdateProbe(int probeIndex) {
    if (probeIndex < 0 || probeIndex >= rt.numProbes) {
        return;
    }
    
    irradianceProbe_t *probe = &rt.probes[probeIndex];
    
    // Sample irradiance in 6 directions (cube faces)
    vec3_t directions[6] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1}
    };
    
    for (int i = 0; i < 6; i++) {
        ray_t ray;
        VectorCopy(probe->position, ray.origin);
        VectorCopy(directions[i], ray.direction);
        ray.tMin = 0.1f;
        ray.tMax = 1000.0f;
        ray.depth = 0;
        
        vec3_t irradiance;
        RT_TracePath(&ray, 0, irradiance);
        
        // Update probe irradiance with temporal filtering
        float blend = 0.1f;
        VectorLerp(probe->irradiance[i], irradiance, blend, probe->irradiance[i]);
    }
    
    probe->lastUpdate = rt.currentFrame;
}

void RT_SampleProbeGrid(const vec3_t pos, const vec3_t normal, vec3_t result) {
    VectorClear(result);

    if (!rt.probes) {
        return;
    }

    // Record demand so the per-frame maintenance loop keeps this grid fresh
    rtProbeLastSampleFrame = rt.currentFrame;
    
    // Find nearest probes
    vec3_t gridPos;
    VectorSubtract(pos, rt.probeGridOrigin, gridPos);
    
    float spacing = rt.probeGridSize[0] / RT_PROBE_GRID_SIZE;
    int x = (int)(gridPos[0] / spacing);
    int y = (int)(gridPos[1] / spacing);
    int z = (int)(gridPos[2] / spacing);
    
    // Clamp to grid bounds
    x = Com_Clamp(0, RT_PROBE_GRID_SIZE - 2, x);
    y = Com_Clamp(0, RT_PROBE_GRID_SIZE - 2, y);
    z = Com_Clamp(0, RT_PROBE_GRID_SIZE - 2, z);
    
    // Trilinear interpolation of 8 nearest probes
    float fx = (gridPos[0] / spacing) - x;
    float fy = (gridPos[1] / spacing) - y;
    float fz = (gridPos[2] / spacing) - z;
    
    for (int dz = 0; dz <= 1; dz++) {
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int index = (z + dz) * RT_PROBE_GRID_SIZE * RT_PROBE_GRID_SIZE +
                           (y + dy) * RT_PROBE_GRID_SIZE + (x + dx);
                
                if (index >= rt.numProbes) continue;
                
                irradianceProbe_t *probe = &rt.probes[index];
                
                // Weight based on position
                float weight = (dx ? fx : 1-fx) * (dy ? fy : 1-fy) * (dz ? fz : 1-fz);
                
                // Sample probe in direction of normal
                vec3_t probeIrradiance = {0, 0, 0};
                for (int i = 0; i < 6; i++) {
                    vec3_t directions[6] = {
                        {1, 0, 0}, {-1, 0, 0},
                        {0, 1, 0}, {0, -1, 0},
                        {0, 0, 1}, {0, 0, -1}
                    };
                    
                    float dot = DotProduct(normal, directions[i]);
                    if (dot > 0) {
                        VectorMA(probeIrradiance, dot, probe->irradiance[i], probeIrradiance);
                    }
                }
                
                VectorMA(result, weight, probeIrradiance, result);
            }
        }
    }
}

/*
===============
RT_RenderPathTracedLighting

Main rendering function - integrates with existing renderer
This is called per frame to add path traced lighting
===============
*/
void RT_RenderPathTracedLighting(void) {
    if (!rt.enabled || !rt_enable || !rt_enable->integer) {
        return;
    }
    
    if (!tr.world) {
        return;
    }
    
    // Check if path tracing is disabled
    if (rt.mode == RT_MODE_OFF) {
        return;
    }
    
    // Update frame counter
    rt.currentFrame++;
    
    // Handle different lighting modes
    switch (rt.mode) {
    case RT_MODE_DYNAMIC:
        // Only path trace dynamic lights
        // Base lighting comes from lightmaps
        break;
        
    case RT_MODE_ALL:
        // Path trace all lighting (static + dynamic)
        // This replaces lightmap lighting
        // Ensure static lights are extracted
        if (rt.numStaticLights == 0 && rt_staticLights->integer) {
            RT_ExtractStaticLights();
        }
        break;
        
    default:
        break;
    }
    
    // Refresh irradiance probes on demand. Each update traces 6 CPU paths,
    // so the per-frame budget must stay small (the previous full-grid sweep
    // of numProbes/16 = 2048 probes cost seconds per frame), and idle grids
    // that nothing sampled recently are skipped entirely.
    if (rt_probes && rt_probes->integer && rt.numProbes > 0 &&
        rt.currentFrame - rtProbeLastSampleFrame < RT_PROBE_DEMAND_WINDOW) {
        const int probesPerFrame = 16;
        for (int i = 0; i < probesPerFrame; i++) {
            int index = (rt.currentFrame * probesPerFrame + i) % rt.numProbes;
            RT_UpdateProbe(index);
        }
    }
    
    // Render debug visualization if enabled
    if (rt_debug && rt_debug->integer) {
        RT_RenderDebugVisualization();
        RT_DrawLightProbes();
        RT_DebugStats();
    }
    
    // The path tracer is now ready to be used by the main renderer
    // Surfaces will query RT_EvaluateDirectLighting based on the mode
}

/*
===============
Utility Functions
===============
*/
void RT_HammersleySequence(int i, int n, vec2_t result) {
    result[0] = (float)i / (float)n;
    result[1] = RT_RadicalInverse(i);
}

float RT_RadicalInverse(unsigned int bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f; // / 0x100000000
}

void RT_GenerateRay(int x, int y, int sample, ray_t *ray) {
    // Generate camera ray for pixel (x, y) with jitter for anti-aliasing
    // This would use the view parameters from tr.refdef
    
    // For now, just return a default ray
    VectorCopy(tr.refdef.vieworg, ray->origin);
    VectorCopy(tr.refdef.viewaxis[0], ray->direction);
    ray->tMin = 0.1f;
    ray->tMax = 10000.0f;
    ray->depth = 0;
}

void RT_GetAccumulatedColor(int x, int y, vec3_t result) {
    if (!rt.accumBuffer || !rt.sampleBuffer ||
        x < 0 || y < 0 ||
        x >= rt.temporalWidth || y >= rt.temporalHeight) {
        VectorClear(result);
        return;
    }

    int pixelIndex = y * rt.temporalWidth + x;
    int base = pixelIndex * 3;
    int samples = rt.sampleBuffer[pixelIndex];

    if (samples <= 0) {
        VectorClear(result);
        return;
    }

    result[0] = rt.accumBuffer[base + 0];
    result[1] = rt.accumBuffer[base + 1];
    result[2] = rt.accumBuffer[base + 2];
}

void RT_BuildCameraRay(int x, int y, int width, int height, ray_t *ray) {
    if (!ray || width <= 0 || height <= 0) {
        return;
    }

    const viewParms_t *vp = &backEnd.viewParms;
    vec3_t forward, right, up;

    VectorCopy(vp->or.axis[0], forward);
    VectorCopy(vp->or.axis[1], right);
    VectorCopy(vp->or.axis[2], up);

    float ndcX = ((2.0f * ((float)x + 0.5f)) / (float)width) - 1.0f;
    float ndcY = 1.0f - ((2.0f * ((float)y + 0.5f)) / (float)height);

    float tanHalfFov = tanf(DEG2RAD(vp->fovX * 0.5f));
    float aspectRatio = (height > 0) ? ((float)width / (float)height) : 1.0f;

    ndcX *= tanHalfFov * aspectRatio;
    ndcY *= tanHalfFov;

    VectorCopy(vp->or.origin, ray->origin);

    ray->direction[0] = forward[0] + ndcX * right[0] + ndcY * up[0];
    ray->direction[1] = forward[1] + ndcX * right[1] + ndcY * up[1];
    ray->direction[2] = forward[2] + ndcX * right[2] + ndcY * up[2];
    VectorNormalize(ray->direction);

    ray->tMin = 0.001f;
    ray->tMax = 10000.0f;
    ray->depth = 0;
    ray->ior = 1.0f;
}

void RT_AccumulateSample(int x, int y, const vec3_t color) {
    if (!rt.accumBuffer || !rt.varianceBuffer || !rt.sampleBuffer ||
        x < 0 || y < 0 ||
        x >= rt.temporalWidth || y >= rt.temporalHeight) {
        return;
    }

    int pixelIndex = y * rt.temporalWidth + x;
    int base = pixelIndex * 3;

    if (!rt.temporalEnabled) {
        rt.accumBuffer[base + 0] = color[0];
        rt.accumBuffer[base + 1] = color[1];
        rt.accumBuffer[base + 2] = color[2];
        rt.varianceBuffer[base + 0] = 0.0f;
        rt.varianceBuffer[base + 1] = 0.0f;
        rt.varianceBuffer[base + 2] = 0.0f;
        rt.sampleBuffer[pixelIndex] = 1;
        return;
    }

    int samples = ++rt.sampleBuffer[pixelIndex];

    for (int c = 0; c < 3; c++) {
        float mean = rt.accumBuffer[base + c];
        float delta = color[c] - mean;
        mean += delta / samples;
        float delta2 = color[c] - mean;

        rt.accumBuffer[base + c] = mean;
        rt.varianceBuffer[base + c] += delta * delta2;
    }
}

void RT_ProcessGpuFrame(const float *rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) {
        return;
    }

    if (width != glConfig.vidWidth || height != glConfig.vidHeight) {
        static qboolean warned = qfalse;
        if (!warned) {
            ri.Printf(PRINT_DEVELOPER, "RT_ProcessGpuFrame: Skipping validation/temporal integration due to resolution mismatch (%dx%d vs %dx%d)\n",
                      width, height, glConfig.vidWidth, glConfig.vidHeight);
            warned = qtrue;
        }
        return;
    }

    RT_InitTemporalBuffers();

    if (!rt.accumBuffer || !rt.varianceBuffer || !rt.sampleBuffer) {
        return;
    }

    if (rt.temporalWidth != width || rt.temporalHeight != height) {
        RT_ResetAccumulation();
        if (rt.temporalWidth != width || rt.temporalHeight != height) {
            return;
        }
    }

    const qboolean validate = (rt_gpuValidate && rt_gpuValidate->integer > 0);
    const int validationStride = validate ? MAX(1, rt_gpuValidate->integer) : 0;

    size_t pixelCount = (size_t)width * (size_t)height;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = (size_t)y * width + x;
            size_t base = idx * 4;

            vec3_t sample = {
                rgba[base + 0],
                rgba[base + 1],
                rgba[base + 2]
            };

            for (int c = 0; c < 3; ++c) {
                if (!isfinite(sample[c]) || fabsf(sample[c]) > 1e6f) {
                    sample[c] = 0.0f;
                }
            }

            RT_AccumulateSample(x, y, sample);
        }
    }

    if (rt_denoise && rt_denoise->integer && rt.denoisedBuffer) {
        RT_DenoiseFrame(rt.accumBuffer, rt.denoisedBuffer, width, height);
    } else if (rt.denoisedBuffer) {
        size_t bytes = pixelCount * 3 * sizeof(float);
        Com_Memcpy(rt.denoisedBuffer, rt.accumBuffer, bytes);
    }

    // rt.currentFrame advances once per frame in RT_RenderPathTracedLighting;
    // advancing it here as well double-counted frames whenever readback ran.

    if (validate && validationStride > 0) {
        double sumSq = 0.0;
        double maxErr = 0.0;
        int samples = 0;
        vec3_t cpuColor;

        for (int y = 0; y < height; y += validationStride) {
            for (int x = 0; x < width; x += validationStride) {
                ray_t ray;
                RT_BuildCameraRay(x, y, width, height, &ray);
                RT_TracePath(&ray, 0, cpuColor);

                size_t idx = (size_t)y * width + x;
                size_t base = idx * 4;
                float gpuColor[3] = {
                    rgba[base + 0],
                    rgba[base + 1],
                    rgba[base + 2]
                };

                for (int c = 0; c < 3; ++c) {
                    if (!isfinite(gpuColor[c]) || fabsf(gpuColor[c]) > 1e6f) {
                        gpuColor[c] = 0.0f;
                    }
                    double diff = (double)cpuColor[c] - (double)gpuColor[c];
                    sumSq += diff * diff;
                    double absDiff = fabs(diff);
                    if (absDiff > maxErr) {
                        maxErr = absDiff;
                    }
                }
                samples++;
            }
        }

        if (samples > 0) {
            rt.validationRMSE = sqrt(sumSq / (double)(samples * 3));
            rt.validationMaxError = maxErr;
            rt.validationSamples = samples;
        } else {
            rt.validationRMSE = 0.0;
            rt.validationMaxError = 0.0;
            rt.validationSamples = 0;
        }
    } else {
        rt.validationRMSE = 0.0;
        rt.validationMaxError = 0.0;
        rt.validationSamples = 0;
    }

    qboolean backendValidated = (validate && validationStride > 0 && rt.validationSamples > 0);
    RT_RecordBackendValidation(rgba, width, height, backendValidated);
}

/*
===============
RT_Status_f

Console command to display path tracing status
===============
*/
void RT_Status_f(void) {
    const char *modeStr = "Unknown";
    const char *qualityStr = "Unknown";
    
    switch (rt.mode) {
    case RT_MODE_OFF:
        modeStr = "Off";
        break;
    case RT_MODE_DYNAMIC:
        modeStr = "Dynamic Lights Only";
        break;
    case RT_MODE_ALL:
        modeStr = "All Lighting (Static + Dynamic)";
        break;
    }
    
    switch (rt.quality) {
    case RT_QUALITY_OFF:
        qualityStr = "Off";
        break;
    case RT_QUALITY_LOW:
        qualityStr = "Low";
        break;
    case RT_QUALITY_MEDIUM:
        qualityStr = "Medium";
        break;
    case RT_QUALITY_HIGH:
        qualityStr = "High";
        break;
    case RT_QUALITY_ULTRA:
        qualityStr = "Ultra";
        break;
    }
    
    ri.Printf(PRINT_ALL, "\n==== Path Tracing Status ====\n");
    ri.Printf(PRINT_ALL, "Enabled: %s\n", rt.enabled ? "Yes" : "No");
    ri.Printf(PRINT_ALL, "Backend: %s\n", RT_GetBackendStatus());
    ri.Printf(PRINT_ALL, "RTX Active: %s\n", (rt.useRTX && RTX_IsAvailable()) ? "Yes" : "No");
    ri.Printf(PRINT_ALL, "Scene Lights: %d (dynamic %d, static %d)\n",
              rt.numSceneLights, rt.numDynamicLights, rt.numStaticLights);
    ri.Printf(PRINT_ALL, "Light Buffer: %s\n",
              rt.sceneLightBufferDirty ? "Pending upload" : "Synced");
    ri.Printf(PRINT_ALL, "Mode: %s\n", modeStr);
    ri.Printf(PRINT_ALL, "Quality: %s\n", qualityStr);
    ri.Printf(PRINT_ALL, "Max Bounces: %d\n", rt.maxBounces);
    ri.Printf(PRINT_ALL, "Samples Per Pixel: %d\n", rt.samplesPerPixel);
    ri.Printf(PRINT_ALL, "Backend: %s\n", (rt.useRTX && RTX_IsAvailable()) ? "RTX Hardware" : "Software");
    ri.Printf(PRINT_ALL, "RTX Available: %s\n", RTX_IsAvailable() ? "Yes" : "No");
    ri.Printf(PRINT_ALL, "Static Lights: %d / %d\n", rt.numStaticLights, rt.maxStaticLights);
    ri.Printf(PRINT_ALL, "Frame: %d\n", rt.currentFrame);
    ri.Printf(PRINT_ALL, "Temporal Accumulation: %s (%dx%d)\n",
              rt.temporalEnabled ? "On" : "Off", rt.temporalWidth, rt.temporalHeight);
    ri.Printf(PRINT_ALL, "\nCVARs:\n");
    ri.Printf(PRINT_ALL, "  rt_enable: %d\n", rt_enable ? rt_enable->integer : 0);
    ri.Printf(PRINT_ALL, "  rt_mode: %s\n", rt_mode ? rt_mode->string : "not set");
    ri.Printf(PRINT_ALL, "  rt_quality: %d\n", rt_quality ? rt_quality->integer : 0);
    ri.Printf(PRINT_ALL, "  rt_bounces: %d\n", rt_bounces ? rt_bounces->integer : 0);
    ri.Printf(PRINT_ALL, "  rt_samples: %d\n", rt_samples ? rt_samples->integer : 0);
    ri.Printf(PRINT_ALL, "  rt_temporal: %d\n", rt_temporal ? rt_temporal->integer : 0);
    ri.Printf(PRINT_ALL, "  rt_backend: %s\n", r_rt_backend ? r_rt_backend->string : "auto");
    ri.Printf(PRINT_ALL, "  rt_staticLights: %d\n", rt_staticLights ? rt_staticLights->integer : 0);
    ri.Printf(PRINT_ALL, "  rt_debug: %d\n", rt_debug ? rt_debug->integer : 0);
    ri.Printf(PRINT_ALL, "  rt_staticLightScale: %.2f\n",
              rt_staticLightScale ? rt_staticLightScale->value : 0.0f);
    ri.Printf(PRINT_ALL, "  rt_staticLightRadiusScale: %.2f\n",
              rt_staticLightRadiusScale ? rt_staticLightRadiusScale->value : 0.0f);
    ri.Printf(PRINT_ALL, "  rt_lightGridCellSize: %.1f\n",
              rt_lightGridCellSize ? rt_lightGridCellSize->value : 0.0f);
    ri.Printf(PRINT_ALL, "  rt_skyLightScale: %.2f\n",
              rt_skyLightScale ? rt_skyLightScale->value : 0.0f);

    if (rt_gpuValidate && rt_gpuValidate->integer > 0) {
        int stride = MAX(1, rt_gpuValidate->integer);
#ifdef USE_VULKAN
        qboolean hardwareActive = (rt.useRTX && RTX_IsAvailable());
#else
        qboolean hardwareActive = qfalse;
#endif
        const char *backendLabel = hardwareActive ? "RTX HW" : "Compute";

        ri.Printf(PRINT_ALL, "\nValidation (stride=%d, active=%s)\n", stride, backendLabel);
        ri.Printf(PRINT_ALL, "  Last frame RMSE: %.5f  Max: %.5f  Samples: %d\n",
                  (float)rt.validationRMSE,
                  (float)rt.validationMaxError,
                  rt.validationSamples);

        const rtBackendValidation_t *compute = &rt.backendValidation[RT_BACKEND_INDEX_COMPUTE];
        const rtBackendValidation_t *hardware = &rt.backendValidation[RT_BACKEND_INDEX_HARDWARE];

        if (compute->hash) {
            ri.Printf(PRINT_ALL,
                      "  Compute backend: hash=%08X map=%s RMSE=%.5f Max=%.5f Samples=%d\n",
                      compute->hash,
                      compute->map[0] ? compute->map : "unknown",
                      (float)compute->rmse,
                      (float)compute->maxError,
                      compute->samples);
        }

        if (hardware->hash) {
            ri.Printf(PRINT_ALL,
                      "  RTX backend:     hash=%08X map=%s RMSE=%.5f Max=%.5f Samples=%d\n",
                      hardware->hash,
                      hardware->map[0] ? hardware->map : "unknown",
                      (float)hardware->rmse,
                      (float)hardware->maxError,
                      hardware->samples);
        }

        if (rt.backendParityMap[0] && hardware->valid && compute->valid) {
            ri.Printf(PRINT_ALL,
                      "  ΔRMSE=%.5f  ΔMax=%.5f (map=%s)\n",
                      (float)rt.backendRMSEDelta,
                      (float)rt.backendMaxErrorDelta,
                      rt.backendParityMap);
        }
    } else {
        ri.Printf(PRINT_ALL, "\nValidation: disabled\n");
    }

    ri.Printf(PRINT_ALL, "=============================\n");
}

/*
===============
Temporal accumulation helpers
===============
*/
void RT_InitTemporalBuffers(void) {
    const int width = glConfig.vidWidth;
    const int height = glConfig.vidHeight;

    if (width <= 0 || height <= 0) {
        return;
    }

    if (rt.temporalWidth == width && rt.temporalHeight == height &&
        rt.accumBuffer && rt.varianceBuffer && rt.sampleBuffer && rt.denoisedBuffer) {
        return;
    }

    if (rt.accumBuffer) {
        ri.Free(rt.accumBuffer);
        rt.accumBuffer = NULL;
    }
    if (rt.varianceBuffer) {
        ri.Free(rt.varianceBuffer);
        rt.varianceBuffer = NULL;
    }
    if (rt.sampleBuffer) {
        ri.Free(rt.sampleBuffer);
        rt.sampleBuffer = NULL;
    }
    if (rt.denoisedBuffer) {
        ri.Free(rt.denoisedBuffer);
        rt.denoisedBuffer = NULL;
    }

    size_t pixelCount = (size_t)width * height;
    size_t colorBytes = pixelCount * 3 * sizeof(float);
    size_t sampleBytes = pixelCount * sizeof(int);

    rt.accumBuffer = ri.Malloc(colorBytes);
    rt.varianceBuffer = ri.Malloc(colorBytes);
    rt.denoisedBuffer = ri.Malloc(colorBytes);
    rt.sampleBuffer = ri.Malloc(sampleBytes);

    if (!rt.accumBuffer || !rt.varianceBuffer || !rt.denoisedBuffer || !rt.sampleBuffer) {
        ri.Printf(PRINT_WARNING, "RT_InitTemporalBuffers: failed to allocate %dx%d buffers\n", width, height);

        if (rt.accumBuffer) { ri.Free(rt.accumBuffer); rt.accumBuffer = NULL; }
        if (rt.varianceBuffer) { ri.Free(rt.varianceBuffer); rt.varianceBuffer = NULL; }
        if (rt.denoisedBuffer) { ri.Free(rt.denoisedBuffer); rt.denoisedBuffer = NULL; }
        if (rt.sampleBuffer) { ri.Free(rt.sampleBuffer); rt.sampleBuffer = NULL; }
        rt.temporalWidth = rt.temporalHeight = 0;
        return;
    }

    rt.temporalWidth = width;
    rt.temporalHeight = height;

    RT_ResetAccumulation();
}

void RT_ResetAccumulation(void) {
    int width = glConfig.vidWidth;
    int height = glConfig.vidHeight;

    if (!rt.accumBuffer || !rt.sampleBuffer || width <= 0 || height <= 0) {
        rt.temporalWidth = width > 0 ? width : 0;
        rt.temporalHeight = height > 0 ? height : 0;
        rt.currentFrame = 0;
        rt.validationRMSE = 0.0;
        rt.validationMaxError = 0.0;
        rt.validationSamples = 0;
        return;
    }

    size_t pixelCount = (size_t)width * height;

    if (rt.accumBuffer) {
        Com_Memset(rt.accumBuffer, 0, pixelCount * 3 * sizeof(float));
    }
    if (rt.varianceBuffer) {
        Com_Memset(rt.varianceBuffer, 0, pixelCount * 3 * sizeof(float));
    }
    if (rt.sampleBuffer) {
        Com_Memset(rt.sampleBuffer, 0, pixelCount * sizeof(int));
    }
    if (rt.denoisedBuffer) {
        Com_Memset(rt.denoisedBuffer, 0, pixelCount * 3 * sizeof(float));
    }

    rt.temporalWidth = width;
    rt.temporalHeight = height;
    rt.currentFrame = 0;
    rt.validationRMSE = 0.0;
    rt.validationMaxError = 0.0;
    rt.validationSamples = 0;
    RT_ResetScreenProgress();
}
/*
===============
RT_BeginFrame

Prepare path tracer for new frame
===============
*/
void RT_BeginFrame(void) {
    if (rt.frameActive) {
        return;
    }

    RT_SelectBackend();
    RT_SyncModeAlias();

    if (!rt_enable || !rt_enable->integer) {
        rt.enabled = qfalse;
        rt.frameActive = qfalse;
        return;
    }
    
    rt.enabled = qtrue;
    
    // Parse rt_mode CVAR
    const char *modeStr = rt_mode->string;
    if (!Q_stricmp(modeStr, "off")) {
        rt.mode = RT_MODE_OFF;
    } else if (!Q_stricmp(modeStr, "dynamic")) {
        rt.mode = RT_MODE_DYNAMIC;
    } else if (!Q_stricmp(modeStr, "all")) {
        rt.mode = RT_MODE_ALL;
    } else {
        // Default to dynamic if invalid
        rt.mode = RT_MODE_DYNAMIC;
    }
    
    // Extract static lights if needed and not already done
    static void *lastWorld = NULL;
    if (rt.mode == RT_MODE_ALL && tr.world) {
        if (tr.world != lastWorld) {
            // New level loaded, extract static lights
            if (!rt.staticLights) {
                rt.maxStaticLights = RT_MAX_STATIC_LIGHTS;
                rt.staticLights = ri.Hunk_Alloc(sizeof(staticLight_t) * rt.maxStaticLights, h_low);
            }
            RT_ExtractStaticLights();
            lastWorld = tr.world;
        }
    }
    
    // Update quality settings
    rt.quality = (rtQuality_t)rt_quality->integer;
    rt.maxBounces = rt_bounces->integer;
    rt.samplesPerPixel = rt_samples->integer;

    RT_InitTemporalBuffers();
    rt.validationRMSE = 0.0;
    rt.validationMaxError = 0.0;
    rt.validationSamples = 0;

    static qboolean firstFrame = qtrue;
    static rtMode_t lastMode = RT_MODE_DYNAMIC;
    static int lastSamples = -1;
    static int lastBounces = -1;
    static rtQuality_t lastQualitySetting = RT_QUALITY_MEDIUM;
    static qboolean lastTemporalEnabled = qtrue;

    rt.temporalEnabled = (rt_temporal && rt_temporal->integer) ? qtrue : qfalse;

    if (lastTemporalEnabled != rt.temporalEnabled) {
        RT_ResetAccumulation();
        lastTemporalEnabled = rt.temporalEnabled;
    }

    if (firstFrame || lastMode != rt.mode) {
        RT_ResetAccumulation();
        lastMode = rt.mode;
        firstFrame = qfalse;
    }

    if (lastSamples != rt.samplesPerPixel || lastBounces != rt.maxBounces || lastQualitySetting != rt.quality) {
        RT_ResetAccumulation();
        lastSamples = rt.samplesPerPixel;
        lastBounces = rt.maxBounces;
        lastQualitySetting = rt.quality;
    }
    
    // Reset frame statistics
    rt.raysTraced = 0;
    rt.triangleTests = 0;
    rt.boxTests = 0;
    rt.frameActive = qtrue;
}
/*
===============
RT_EndFrame

End of frame statistics and debug output
===============
*/
void RT_EndFrame(void) {
    if (!rt.frameActive) {
        return;
    }

    if (rt_debug && rt_debug->integer && rt.enabled) {
        const char *modeStr = "Unknown";
        switch (rt.mode) {
        case RT_MODE_OFF:
            modeStr = "Off";
            break;
        case RT_MODE_DYNAMIC:
            modeStr = "Dynamic Only";
            break;
        case RT_MODE_ALL:
            modeStr = "All Lighting";
            break;
        }
        
        ri.Printf(PRINT_ALL, "Path Tracing: Mode=%s, Static Lights=%d, Rays=%d\n",
                  modeStr, rt.numStaticLights, rt.raysTraced);
    }

    if (rt.enabled && rt_gpuValidate && rt_gpuValidate->integer > 0 &&
        rt.validationSamples > 0) {
        int stride = MAX(1, rt_gpuValidate->integer);
#ifdef USE_VULKAN
        qboolean hardwareActive = (rt.useRTX && RTX_IsAvailable());
#else
        qboolean hardwareActive = qfalse;
#endif
        const char *backendLabel = hardwareActive ? "RTX HW" : "Compute";
        ri.Printf(PRINT_ALL,
                  "rt_gpuValidate (%s): stride=%d RMSE=%.5f max=%.5f (%d samples)\n",
                  backendLabel,
                  stride,
                  (float)rt.validationRMSE,
                  (float)rt.validationMaxError,
                  rt.validationSamples);
    }

    if (rt.backendParityMap[0] &&
        rt.backendValidation[RT_BACKEND_INDEX_COMPUTE].valid &&
        rt.backendValidation[RT_BACKEND_INDEX_HARDWARE].valid &&
        rt.backendParityFrame == rt.currentFrame) {
        ri.Printf(PRINT_DEVELOPER,
                  "rt_gpuValidate parity %s: ΔRMSE=%.5f ΔMax=%.5f (RTX=%08X, Compute=%08X)\n",
                  rt.backendParityMap,
                  (float)rt.backendRMSEDelta,
                  (float)rt.backendMaxErrorDelta,
                  rt.backendValidation[RT_BACKEND_INDEX_HARDWARE].hash,
                  rt.backendValidation[RT_BACKEND_INDEX_COMPUTE].hash);
    }

    rt.frameActive = qfalse;
}

void RT_ResetSkyLighting(void) {
    VectorClear(rtSkyDirectionAccum);
    VectorClear(rtSkyColorAccum);
    rtSkyWeightAccum = 0.0f;
    VectorClear(rt.skyAmbientColor);
    VectorClear(rt.skyDomeColor);
    rt.skyAmbientIntensity = 0.0f;
}

void RT_AddSkyLightingContribution(const vec3_t direction, const vec3_t color, float weight) {
    if (weight <= 0.0f) {
        return;
    }

    vec3_t dirNormalized;
    VectorCopy(direction, dirNormalized);
    if (VectorNormalize(dirNormalized) <= 0.0f) {
        return;
    }

    vec3_t colorClamped;
    colorClamped[0] = color[0] < 0.0f ? 0.0f : color[0];
    colorClamped[1] = color[1] < 0.0f ? 0.0f : color[1];
    colorClamped[2] = color[2] < 0.0f ? 0.0f : color[2];

    VectorMA(rtSkyDirectionAccum, weight, dirNormalized, rtSkyDirectionAccum);
    VectorMA(rtSkyColorAccum, weight, colorClamped, rtSkyColorAccum);
    rtSkyWeightAccum += weight;
}

void RT_AddEmissiveStaticLight(const vec3_t origin, const vec3_t color, float intensity, float radius) {
    if (!rt_staticLights || !rt_staticLights->integer) {
        return;
    }

    if (!rt.staticLights) {
        if (rt.maxStaticLights <= 0) {
            rt.maxStaticLights = RT_MAX_STATIC_LIGHTS;
        }
        rt.staticLights = ri.Hunk_Alloc(sizeof(staticLight_t) * rt.maxStaticLights, h_low);
        rt.numStaticLights = 0;
    }

    if (rt.numStaticLights >= rt.maxStaticLights) {
        ri.Printf(PRINT_WARNING, "RT_AddEmissiveStaticLight: static light limit reached (%d)\n", rt.maxStaticLights);
        return;
    }

    staticLight_t *light = &rt.staticLights[rt.numStaticLights++];
    VectorCopy(origin, light->origin);
    VectorCopy(color, light->color);
    light->intensity = intensity;
    light->radius = RT_SafeRadius(radius);
    light->type = 0; // point light
    VectorClear(light->direction);
    light->spotAngle = 0.0f;
    light->castShadows = qfalse;

    RT_AddSkyLightingContribution(light->direction, light->color, intensity);
#ifdef USE_VULKAN
    rt.sceneLightBufferDirty = qtrue;
#endif
}

static qboolean RT_BuildDynamicFromRenderLight(const renderLight_t *light, rtDynamicLight_t *out) {
    if (!light || !out) {
        return qfalse;
    }

    vec3_t color;
    VectorCopy(light->color, color);
    float intensity = (light->intensity > 0.0f) ? light->intensity : 1.0f;
    float brightness = color[0] + color[1] + color[2];

    if (brightness <= 0.0f) {
        return qfalse;
    }

    out->type = RT_LIGHT_TYPE_POINT;
    VectorCopy(light->origin, out->origin);
    VectorCopy(color, out->color);
    VectorClear(out->direction);
    out->radius = RT_SafeRadius((light->cutoffDistance > 0.0f) ? light->cutoffDistance : light->radius);
    out->intensity = intensity;
    out->spotCos = -1.0f;
    out->castsShadows = (light->flags & LIGHTFLAG_NOSHADOWS) ? qfalse : qtrue;
    out->isStatic = light->isStatic ? qtrue : qfalse;
    out->additive = qfalse;

    switch (light->type) {
    case RL_OMNI:
        out->type = RT_LIGHT_TYPE_POINT;
        break;
    case RL_PROJ:
        out->type = RT_LIGHT_TYPE_SPOT;
        VectorSubtract(light->target, light->origin, out->direction);
        if (VectorNormalize(out->direction) <= 0.0f) {
            VectorSet(out->direction, 0.0f, 0.0f, -1.0f);
        }
        out->spotCos = RT_ComputeSpotCosFromFov(light->fovX);
        break;
    case RL_DIRECTIONAL:
        out->type = RT_LIGHT_TYPE_DIRECTIONAL;
        VectorCopy(light->target, out->direction);
        if (VectorNormalize(out->direction) <= 0.0f) {
            VectorSet(out->direction, 0.0f, 0.0f, -1.0f);
        }
        out->radius = RT_DIRECTIONAL_MAX_DISTANCE;
        out->spotCos = -1.0f;
        out->isStatic = qtrue;
        break;
    case RL_AMBIENT:
        out->type = RT_LIGHT_TYPE_POINT;
        out->castsShadows = qfalse;
        out->isStatic = qtrue;
        if (out->radius < 2048.0f) {
            out->radius = 2048.0f;
        }
        break;
    case RL_FOG:
    default:
        return qfalse;
    }

    out->radius = RT_SafeRadius(out->radius);
    return qtrue;
}

static qboolean RT_BuildDynamicFromLegacyDlight(const dlight_t *dlight, rtDynamicLight_t *out) {
    if (!dlight || !out) {
        return qfalse;
    }

    float brightness = dlight->color[0] + dlight->color[1] + dlight->color[2];
    if (brightness <= 0.0f) {
        return qfalse;
    }

    out->type = RT_LIGHT_TYPE_POINT;
    VectorCopy(dlight->origin, out->origin);
    VectorCopy(dlight->color, out->color);
    VectorClear(out->direction);
    out->radius = RT_SafeRadius(dlight->radius);
    // Game dlight colors are normalized 0..1; scale into the same energy
    // range the calibrated static lights use or effect lights are invisible.
    out->intensity = (brightness / 3.0f) *
        ((rt_dlightIntensity && rt_dlightIntensity->value > 0.0f) ? rt_dlightIntensity->value : 12.0f);
    if (out->intensity <= 0.0f) {
        out->intensity = 1.0f;
    }
    out->spotCos = -1.0f;
    out->castsShadows = dlight->additive ? qfalse : qtrue;
    out->isStatic = qfalse;
    out->additive = dlight->additive ? qtrue : qfalse;

    return qtrue;
}

static void RT_RebuildSceneLights(void) {
    if (rt.mode == RT_MODE_OFF) {
        if (rt.numSceneLights != 0 || rt.sceneLightHash != 0) {
            rt.numSceneLights = 0;
            if (rt.sceneLightHash != 0) {
                rt.sceneLightHash = 0;
                RT_ResetAccumulation();
            }
        }
        RT_BuildLightGrid();
#ifdef USE_VULKAN
        rt.sceneLightBufferDirty = qtrue;
        RT_UpdateSceneLightBuffer();
        RT_UpdateLightGridBuffers();
#endif
        return;
    }

    // Statics (and the injected skylight) form a stable prefix of the scene
    // light array: the GPU light grid indexes only this prefix, so it stays
    // valid across dynamic-light churn and only rebuilds on world changes.
    // Dynamic lights are appended after the prefix and iterated linearly by
    // the shaders (there are only ever a handful of them).
    int combined = 0;

    if (rt.mode == RT_MODE_ALL) {
        for (int i = 0; i < rt.numStaticLights && combined < RT_MAX_SCENE_LIGHTS; i++) {
            const staticLight_t *sl = &rt.staticLights[i];
            rtSceneLight_t *dst = &rt.sceneLights[combined++];

            dst->type = (sl->type == 1) ? RT_LIGHT_TYPE_SPOT : RT_LIGHT_TYPE_POINT;
            VectorCopy(sl->origin, dst->origin);
            VectorCopy(sl->color, dst->color);
            dst->radius = RT_SafeRadius(sl->radius);
            dst->intensity = sl->intensity;
            if (dst->intensity <= 0.0f) {
                float fallback = fabsf(dst->color[0]) + fabsf(dst->color[1]) + fabsf(dst->color[2]);
                if (fallback > 0.0f) {
                    dst->intensity = fallback / 3.0f;
                }
            }
            dst->castsShadows = sl->castShadows;
            dst->isStatic = qtrue;

            if (dst->type == RT_LIGHT_TYPE_SPOT) {
                VectorCopy(sl->direction, dst->direction);
                if (VectorNormalize(dst->direction) <= 0.0f) {
                    VectorSet(dst->direction, 0.0f, 0.0f, -1.0f);
                }
                dst->spotCos = RT_ComputeSpotCosFromFov(sl->spotAngle);
            } else {
                VectorClear(dst->direction);
                dst->spotCos = -1.0f;
            }
        }
    }

    if (combined < RT_MAX_SCENE_LIGHTS) {
        vec3_t skyDirection;
        vec3_t skyColor;
        float skyIntensity = 0.0f;
        if (RT_ComputeSkyLight(skyDirection, skyColor, &skyIntensity)) {
            rtSceneLight_t *dst = &rt.sceneLights[combined++];
            dst->type = RT_LIGHT_TYPE_DIRECTIONAL;
            VectorClear(dst->origin);
            VectorCopy(skyColor, dst->color);
            dst->radius = RT_DIRECTIONAL_MAX_DISTANCE;
            dst->intensity = skyIntensity;
            VectorCopy(skyDirection, dst->direction);
            if (VectorNormalize(dst->direction) <= 0.0f) {
                VectorSet(dst->direction, 0.0f, 0.0f, -1.0f);
            }
            dst->spotCos = -1.0f;
            dst->castsShadows = qfalse;
            dst->isStatic = qtrue;
            if (rt_debug && rt_debug->integer >= 2) {
                ri.Printf(PRINT_ALL,
                    "RT: Injected skylight dir=(%.2f,%.2f,%.2f) color=(%.2f,%.2f,%.2f) intensity=%.3f\n",
                    skyDirection[0], skyDirection[1], skyDirection[2],
                    skyColor[0], skyColor[1], skyColor[2],
                    skyIntensity);
            }
        }
        else {
            rt.skyAmbientIntensity = 0.0f;
            VectorClear(rt.skyAmbientColor);
        }
    }

    rt.staticSceneLightCount = combined;

    for (int i = 0; i < rt.numDynamicLights && combined < RT_MAX_SCENE_LIGHTS; i++) {
        const rtDynamicLight_t *src = &rt.dynamicLights[i];
        rtSceneLight_t *dst = &rt.sceneLights[combined++];

        dst->type = src->type;
        VectorCopy(src->origin, dst->origin);
        VectorCopy(src->color, dst->color);
        VectorCopy(src->direction, dst->direction);
        dst->radius = RT_SafeRadius(src->radius);
        dst->intensity = src->intensity;
        dst->spotCos = Com_Clamp(-1.0f, 1.0f, src->spotCos);
        if (dst->intensity <= 0.0f) {
            float fallback = fabsf(dst->color[0]) + fabsf(dst->color[1]) + fabsf(dst->color[2]);
            if (fallback > 0.0f) {
                dst->intensity = fallback / 3.0f;
            }
        }
        dst->castsShadows = src->castsShadows;
        dst->isStatic = src->isStatic;

        if (dst->type == RT_LIGHT_TYPE_DIRECTIONAL) {
            if (VectorNormalize(dst->direction) <= 0.0f) {
                VectorSet(dst->direction, 0.0f, 0.0f, -1.0f);
            }
            dst->radius = RT_DIRECTIONAL_MAX_DISTANCE;
        } else if (dst->type == RT_LIGHT_TYPE_SPOT) {
            if (VectorNormalize(dst->direction) <= 0.0f) {
                VectorSet(dst->direction, 0.0f, 0.0f, -1.0f);
            }
        } else {
            VectorClear(dst->direction);
        }
    }

    rt.numSceneLights = combined;
    static int sceneLightLogCount = 0;
    if (sceneLightLogCount < 5 && rt.numSceneLights > 0) {
        float totalIntensity = 0.0f;
        for (int i = 0; i < rt.numSceneLights; ++i) {
            totalIntensity += rt.sceneLights[i].intensity;
        }
        const char *modeLabel = "unknown";
        switch (rt.mode) {
        case RT_MODE_OFF: modeLabel = "off"; break;
        case RT_MODE_DYNAMIC: modeLabel = "dynamic"; break;
        case RT_MODE_ALL: modeLabel = "all"; break;
        }
        ri.Printf(PRINT_ALL,
            "RT_Debug: scene lights rebuilt (mode=%s count=%d avgIntensity=%.3f skylight=%s)\n",
            modeLabel,
            rt.numSceneLights,
            (rt.numSceneLights > 0) ? totalIntensity / (float)rt.numSceneLights : 0.0f,
            (combined > 0 && rt.sceneLights[combined - 1].type == RT_LIGHT_TYPE_DIRECTIONAL) ? "yes" : "no");
        sceneLightLogCount++;
    }
	if (rt_debug && rt_debug->integer >= 2 && rt.numSceneLights > 0) {
		float totalIntensity = 0.0f;
		float minIntensity = FLT_MAX;
		float maxIntensity = 0.0f;
		int directionalCount = 0;
		int pointCount = 0;
		int spotCount = 0;
		int staticCount = 0;
		for (int i = 0; i < rt.numSceneLights; ++i) {
			rtSceneLight_t *light = &rt.sceneLights[i];
			totalIntensity += light->intensity;
			if (light->intensity < minIntensity) {
				minIntensity = light->intensity;
			}
			if (light->intensity > maxIntensity) {
				maxIntensity = light->intensity;
			}
			switch (light->type) {
			case RT_LIGHT_TYPE_DIRECTIONAL:
				directionalCount++;
				break;
			case RT_LIGHT_TYPE_SPOT:
				spotCount++;
				break;
			default:
				pointCount++;
				break;
			}
			if (light->isStatic) {
				staticCount++;
			}
		}
		float avgIntensity = totalIntensity / (float)rt.numSceneLights;
		if (minIntensity == FLT_MAX) {
			minIntensity = 0.0f;
		}
		ri.Printf(PRINT_ALL, "RT: scene lights=%d static=%d dynamic=%d | avgIntensity=%.3f min=%.3f max=%.3f | types point=%d spot=%d dir=%d\n",
				 rt.numSceneLights, staticCount, rt.numSceneLights - staticCount,
				 avgIntensity, minIntensity, maxIntensity, pointCount, spotCount, directionalCount);
	}


    // The grid only indexes the static prefix, so dynamic-light churn during
    // gameplay never forces a grid rebuild/re-upload.
    {
        static uint32_t staticLightHash = 0u;
        uint32_t newStaticHash = RT_ComputeSceneLightHash(rt.sceneLights, rt.staticSceneLightCount);
        if (newStaticHash != staticLightHash || rt.lightGrid.cellCount == 0) {
            staticLightHash = newStaticHash;
            RT_BuildLightGrid();
        }
    }

    uint32_t newHash = RT_ComputeSceneLightHash(rt.sceneLights, rt.numSceneLights);
    if (newHash != rt.sceneLightHash) {
        rt.sceneLightHash = newHash;
        RT_ResetAccumulation();
    }
#ifdef USE_VULKAN
    rt.sceneLightBufferDirty = qtrue;
    RT_UpdateSceneLightBuffer();
    RT_UpdateLightGridBuffers();
#endif
}

void RT_UpdateDynamicLights(void) {
    rt.numDynamicLights = 0;

    // Age and expire the volumetric weapon effects alongside the lights
    RT_VolumeFX_FrameUpdate();

    if (rt.mode == RT_MODE_OFF) {
        RT_RebuildSceneLights();
        return;
    }

    R_UpdateLightSystem();

    // Only genuinely dynamic render lights (movers etc.) — the light
    // system's visible set is dominated by static world lights that are
    // already in the scene light prefix; appending them again double-lights
    // the map.
    if (tr_lightSystem.numVisibleLights > 0) {
        int limit = MIN(tr_lightSystem.numVisibleLights, RT_MAX_LIGHTS);
        for (int i = 0; i < limit && rt.numDynamicLights < RT_MAX_LIGHTS; i++) {
            renderLight_t *light = tr_lightSystem.visibleLights[i];
            if (!light || light->isStatic) {
                continue;
            }

            if (light->viewCount && light->viewCount != tr.viewCount) {
                continue;
            }

            if (RT_BuildDynamicFromRenderLight(light, &rt.dynamicLights[rt.numDynamicLights])) {
                rt.numDynamicLights++;
            }
        }
    }

    // Per-frame effect dlights from the game (muzzle flashes, rocket glow,
    // explosions) are a separate population and must always be ingested.
    if (tr.refdef.num_dlights > 0) {
        int legacyCount = MIN(tr.refdef.num_dlights, RT_MAX_LIGHTS);
        for (int i = 0; i < legacyCount && rt.numDynamicLights < RT_MAX_LIGHTS; i++) {
            if (RT_BuildDynamicFromLegacyDlight(&tr.refdef.dlights[i], &rt.dynamicLights[rt.numDynamicLights])) {
                rt.numDynamicLights++;
            }
        }
    }

    RT_RebuildSceneLights();
}

static ID_INLINE float RT_Luminance(const float *rgb) {
    return 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
}

void RT_InitDenoiser(void) {
    rt.denoiseSigma = 0.25f;
    rt.denoiseThreshold = 0.5f;
}

void RT_ApplyTemporalFilter(float *current, float *history, float *output, int width, int height) {
    if (!current || !output || width <= 0 || height <= 0) {
        return;
    }

    size_t pixelCount = (size_t)width * height;
    const float minAlpha = 0.05f;

    for (size_t i = 0; i < pixelCount; i++) {
        int base = (int)(i * 3);
        int samples = (rt.sampleBuffer) ? rt.sampleBuffer[i] : 0;

        if (samples <= 0) {
            if (history) {
                output[base + 0] = history[base + 0];
                output[base + 1] = history[base + 1];
                output[base + 2] = history[base + 2];
            } else {
                output[base + 0] = current[base + 0];
                output[base + 1] = current[base + 1];
                output[base + 2] = current[base + 2];
            }
            continue;
        }

        float alpha = 1.0f / (float)samples;
        alpha = Com_Clamp(minAlpha, 1.0f, alpha);

        for (int c = 0; c < 3; c++) {
            float prev = history ? history[base + c] : current[base + c];
            float curr = current[base + c];
            output[base + c] = prev + alpha * (curr - prev);
        }
    }
}

void RT_ApplySpatialFilter(float *input, float *output, int width, int height) {
    if (!input || !output || width <= 0 || height <= 0) {
        return;
    }

    const int radius = 1;
    const float sigmaSpatial = 1.0f;
    const float sigmaColor = 0.25f;
    const float varianceInfluence = 0.5f;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int index = y * width + x;
            int base = index * 3;

            if (!rt.sampleBuffer || rt.sampleBuffer[index] <= 0) {
                output[base + 0] = input[base + 0];
                output[base + 1] = input[base + 1];
                output[base + 2] = input[base + 2];
                continue;
            }

            float centerColor[3] = {
                input[base + 0],
                input[base + 1],
                input[base + 2]
            };

            float centerLuma = RT_Luminance(centerColor);
            float centerVariance = 0.0f;

            if (rt.varianceBuffer && rt.sampleBuffer[index] > 1) {
                float varianceSum = rt.varianceBuffer[base + 0] +
                                    rt.varianceBuffer[base + 1] +
                                    rt.varianceBuffer[base + 2];
                centerVariance = varianceSum / (3.0f * (rt.sampleBuffer[index] - 1));
            }

            vec3_t accum = {0.0f, 0.0f, 0.0f};
            float weightSum = 0.0f;

            for (int dy = -radius; dy <= radius; dy++) {
                int ny = y + dy;
                if (ny < 0) ny = 0;
                if (ny >= height) ny = height - 1;

                for (int dx = -radius; dx <= radius; dx++) {
                    int nx = x + dx;
                    if (nx < 0) nx = 0;
                    if (nx >= width) nx = width - 1;

                    int nIndex = ny * width + nx;
                    int nBase = nIndex * 3;

                    if (!rt.sampleBuffer || rt.sampleBuffer[nIndex] <= 0) {
                        continue;
                    }

                    float neighborColor[3] = {
                        input[nBase + 0],
                        input[nBase + 1],
                        input[nBase + 2]
                    };
                    float neighborLuma = RT_Luminance(neighborColor);

                    float neighborVariance = 0.0f;
                    if (rt.varianceBuffer && rt.sampleBuffer[nIndex] > 1) {
                        float nVarianceSum = rt.varianceBuffer[nBase + 0] +
                                             rt.varianceBuffer[nBase + 1] +
                                             rt.varianceBuffer[nBase + 2];
                        neighborVariance = nVarianceSum / (3.0f * (rt.sampleBuffer[nIndex] - 1));
                    }

                    float spatialDist2 = (float)(dx * dx + dy * dy);
                    float wSpatial = expf(-spatialDist2 / (2.0f * sigmaSpatial * sigmaSpatial));

                    float colorDiff = neighborLuma - centerLuma;
                    float wColor = expf(-(colorDiff * colorDiff) / (2.0f * sigmaColor * sigmaColor + 1e-6f));

                    float varianceTerm = centerVariance + neighborVariance + 1e-6f;
                    float wVariance = 1.0f / (1.0f + varianceTerm * varianceInfluence);

                    float weight = wSpatial * wColor * wVariance;

                    accum[0] += neighborColor[0] * weight;
                    accum[1] += neighborColor[1] * weight;
                    accum[2] += neighborColor[2] * weight;
                    weightSum += weight;
                }
            }

            if (weightSum > 1e-5f) {
                output[base + 0] = accum[0] / weightSum;
                output[base + 1] = accum[1] / weightSum;
                output[base + 2] = accum[2] / weightSum;
            } else {
                output[base + 0] = centerColor[0];
                output[base + 1] = centerColor[1];
                output[base + 2] = centerColor[2];
            }
        }
    }
}

void RT_DenoiseFrame(float *input, float *output, int width, int height) {
    if (!input || !output || width <= 0 || height <= 0) {
        return;
    }

    size_t pixelCount = (size_t)width * height;
    size_t bytes = pixelCount * 3 * sizeof(float);

    int denoiseLevel = rt_denoise ? rt_denoise->integer : 0;
    if (denoiseLevel <= 0 || !rt.temporalEnabled) {
        if (output != input) {
            Com_Memcpy(output, input, bytes);
        }
        return;
    }

    float *historyCopy = NULL;
    if (output) {
        historyCopy = (float *)ri.Hunk_AllocateTempMemory(bytes);
        if (historyCopy) {
            Com_Memcpy(historyCopy, output, bytes);
        }
    }

    float *temp = (float *)ri.Hunk_AllocateTempMemory(bytes);
    if (!temp) {
        if (historyCopy) {
            ri.Hunk_FreeTempMemory(historyCopy);
        }
        if (output != input) {
            Com_Memcpy(output, input, bytes);
        }
        return;
    }

    RT_ApplyTemporalFilter(input, historyCopy, temp, width, height);

    if (denoiseLevel > 1) {
        RT_ApplySpatialFilter(temp, output, width, height);
    } else {
        Com_Memcpy(output, temp, bytes);
    }

    ri.Hunk_FreeTempMemory(temp);
    if (historyCopy) {
        ri.Hunk_FreeTempMemory(historyCopy);
    }
}

void RT_ClearLightCache(void) {}
/*
===============
RT_DrawDebugRays

Visualize path traced rays for debugging
===============
*/
void RT_DrawDebugRays(void) {
    if (!rt_debug || !rt_debug->integer || !rt.enabled) {
        return;
    }
    
    // Draw static light positions if in ALL mode
    if (rt.mode == RT_MODE_ALL) {
        for (int i = 0; i < MIN(rt.numStaticLights, 50); i++) {
            staticLight_t *sl = &rt.staticLights[i];
            
            // Draw light as a small sphere or marker
            // This would integrate with the debug rendering system
            // For now just log the info
            if (i < 5) { // Only log first 5 to avoid spam
                ri.Printf(PRINT_ALL, "Static Light %d: pos=(%.1f,%.1f,%.1f) color=(%.2f,%.2f,%.2f) intensity=%.1f radius=%.1f\n",
                          i, sl->origin[0], sl->origin[1], sl->origin[2],
                          sl->color[0], sl->color[1], sl->color[2],
                          sl->intensity, sl->radius);
            }
        }
    }
}
/*
================
RT_ComputeLightingAtPoint

Computes lighting at a specific point using path tracing
================
*/
void RT_ComputeLightingAtPoint(const vec3_t point, vec3_t result) {
    int numSamples = 8;  // Number of hemisphere samples
    
    VectorClear(result);
    
    if (!rt.enabled || rt.mode == RT_MODE_OFF) {
        return;
    }
    
    // Sample lighting from multiple directions
    for (int i = 0; i < numSamples; i++) {
        ray_t ray;
        hitInfo_t hit;
        vec3_t sampleDir;
        
        // Generate random direction on hemisphere
        float theta = 2.0f * M_PI * (i + random()) / numSamples;
        float phi = acos(1.0f - 2.0f * random());
        
        sampleDir[0] = sin(phi) * cos(theta);
        sampleDir[1] = sin(phi) * sin(theta);
        sampleDir[2] = cos(phi);
        
        // Create ray from point
        VectorCopy(point, ray.origin);
        VectorCopy(sampleDir, ray.direction);
        ray.tMin = 0.001f;
        ray.tMax = 1000.0f;
        ray.depth = 0;
        
        // Trace ray and accumulate lighting
        if (RT_TraceRay(&ray, &hit)) {
            vec3_t lighting;
            VectorClear(lighting);
            
            // Evaluate direct lighting at hit point
            RT_EvaluateDirectLighting(&hit, sampleDir, lighting);
            
            // Accumulate with cosine weighting
            float cosTheta = DotProduct(hit.normal, sampleDir);
            if (cosTheta > 0) {
                VectorMA(result, cosTheta / numSamples, lighting, result);
            }
        }
    }
    
    // Add ambient term
    VectorMA(result, 0.1f, colorWhite, result);
}

void RT_DrawProbeGrid(void) {}
void RT_DrawLightCache(void) {}
qboolean RT_RayBSPIntersect(const ray_t *ray, rtBspNode_t *node, hitInfo_t *hit) { return qfalse; }


