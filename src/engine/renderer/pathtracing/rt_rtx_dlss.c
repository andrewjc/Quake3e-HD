/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

NVIDIA DLSS (Deep Learning Super Sampling) Implementation
Provides AI-powered upscaling for improved performance
===========================================================================
*/

#include "rt_rtx.h"
#include "../core/tr_local.h"
#include <math.h>

#ifdef USE_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

typedef struct dlssState_s {
    // NGX SDK
    NVSDK_NGX_Handle            *ngxHandle;
    NVSDK_NGX_Parameter         *ngxParams;
    NVSDK_NGX_Feature_Handle    *dlssFeature;
    
    // Feature info
    qboolean                    available;
    uint32_t                    optimalWidth;
    uint32_t                    optimalHeight;
    uint32_t                    minWidth;
    uint32_t                    minHeight;
    uint32_t                    maxWidth;
    uint32_t                    maxHeight;
    float                       sharpness;
    
    // Current settings
    dlssMode_t                  mode;
    uint32_t                    renderWidth;
    uint32_t                    renderHeight;
    uint32_t                    displayWidth;
    uint32_t                    displayHeight;
    
    // Resources
    void                        *colorInput;
    void                        *colorOutput;
    void                        *depth;
    void                        *motionVectors;
    void                        *exposure;
    
    // Performance
    float                       lastFrameTime;
    uint32_t                    frameCount;
} dlssState_t;

static dlssState_t dlss;

// DLSS quality presets
static const struct {
    dlssMode_t mode;
    float scaleFactor;
    const char *name;
    NVSDK_NGX_PerfQuality_Value ngxQuality;
} dlssModes[] = {
    { DLSS_MODE_OFF,               1.00f, "Off",                NVSDK_NGX_PerfQuality_Value_Off },
    { DLSS_MODE_ULTRA_PERFORMANCE, 0.33f, "Ultra Performance",  NVSDK_NGX_PerfQuality_Value_UltraPerformance },
    { DLSS_MODE_PERFORMANCE,       0.50f, "Performance",        NVSDK_NGX_PerfQuality_Value_MaxPerf },
    { DLSS_MODE_BALANCED,          0.58f, "Balanced",           NVSDK_NGX_PerfQuality_Value_Balanced },
    { DLSS_MODE_QUALITY,           0.67f, "Quality",            NVSDK_NGX_PerfQuality_Value_MaxQuality }
};

/*
================
RTX_GetDLSSRenderResolution

Calculate render resolution for DLSS mode
================
*/
static void RTX_GetDLSSRenderResolution(dlssMode_t mode, uint32_t displayWidth, uint32_t displayHeight,
                                        uint32_t *renderWidth, uint32_t *renderHeight) {
    float scale = 1.0f;
    
    for (int i = 0; i < ARRAY_LEN(dlssModes); i++) {
        if (dlssModes[i].mode == mode) {
            scale = dlssModes[i].scaleFactor;
            break;
        }
    }
    
    *renderWidth = (uint32_t)(displayWidth * scale);
    *renderHeight = (uint32_t)(displayHeight * scale);
    
    // Align to 8 pixels for better performance
    *renderWidth = (*renderWidth + 7) & ~7;
    *renderHeight = (*renderHeight + 7) & ~7;
    
    // Clamp to DLSS limits
    if (dlss.available) {
        *renderWidth = MAX(dlss.minWidth, MIN(*renderWidth, dlss.maxWidth));
        *renderHeight = MAX(dlss.minHeight, MIN(*renderHeight, dlss.maxHeight));
    }
}

/*
================
RTX_InitDLSS

Initialize NVIDIA DLSS
================
*/
qboolean RTX_InitDLSS(void) {
    NVSDK_NGX_Result result;
    
    // Check for NVIDIA GPU
    if (rtx.gpuType != RTX_GPU_NVIDIA) {
        ri.Printf(PRINT_WARNING, "RTX: DLSS requires NVIDIA GPU\n");
        return qfalse;
    }
    
    // Initialize NGX SDK
    wchar_t logPath[MAX_PATH];
    GetTempPathW(MAX_PATH, logPath);
    wcscat(logPath, L"Quake3e_DLSS");
    
    // Application ID for Quake3e
    const uint32_t appId = 0x51336B33;  // "Q3k3"
    
    // Initialize NGX
    result = NVSDK_NGX_D3D12_Init(appId, logPath, rtx.device, 
                                   &dlss.ngxParams, NVSDK_NGX_Version_API);
    
    if (NVSDK_NGX_FAILED(result)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to initialize NGX SDK (0x%08x)\n", result);
        return qfalse;
    }
    
    // Check DLSS availability
    int dlssAvailable = 0;
    result = NVSDK_NGX_Parameter_GetI(dlss.ngxParams, NVSDK_NGX_Parameter_SuperSampling_Available, 
                                       &dlssAvailable);
    
    if (!dlssAvailable || NVSDK_NGX_FAILED(result)) {
        ri.Printf(PRINT_WARNING, "RTX: DLSS not available on this GPU\n");
        NVSDK_NGX_D3D12_Shutdown();
        return qfalse;
    }
    
    // Get DLSS capabilities
    result = NGX_DLSS_GET_OPTIMAL_SETTINGS(dlss.ngxParams,
                                           glConfig.vidWidth, glConfig.vidHeight,
                                           NVSDK_NGX_PerfQuality_Value_Balanced,
                                           &dlss.optimalWidth, &dlss.optimalHeight,
                                           &dlss.maxWidth, &dlss.maxHeight,
                                           &dlss.minWidth, &dlss.minHeight,
                                           &dlss.sharpness);
    
    if (NVSDK_NGX_FAILED(result)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to get DLSS optimal settings\n");
        NVSDK_NGX_D3D12_Shutdown();
        return qfalse;
    }
    
    dlss.available = qtrue;
    dlss.displayWidth = glConfig.vidWidth;
    dlss.displayHeight = glConfig.vidHeight;
    dlss.mode = DLSS_MODE_BALANCED;  // Default to balanced
    dlss.frameCount = 0;
    
    // Calculate initial render resolution
    RTX_GetDLSSRenderResolution(dlss.mode, dlss.displayWidth, dlss.displayHeight,
                                &dlss.renderWidth, &dlss.renderHeight);
    
    // Create DLSS feature
    NVSDK_NGX_DLSS_Create_Params dlssCreateParams = {0};
    dlssCreateParams.Feature.InWidth = dlss.renderWidth;
    dlssCreateParams.Feature.InHeight = dlss.renderHeight;
    dlssCreateParams.Feature.InTargetWidth = dlss.displayWidth;
    dlssCreateParams.Feature.InTargetHeight = dlss.displayHeight;
    dlssCreateParams.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;
    dlssCreateParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Create_Flags_None;
    
    result = NGX_D3D12_CREATE_DLSS_EXT(rtx.commandList, 1, 1, &dlss.dlssFeature,
                                        dlss.ngxParams, &dlssCreateParams);
    
    if (NVSDK_NGX_FAILED(result)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create DLSS feature (0x%08x)\n", result);
        NVSDK_NGX_D3D12_Shutdown();
        dlss.available = qfalse;
        return qfalse;
    }
    
    ri.Printf(PRINT_ALL, "RTX: DLSS initialized successfully\n");
    ri.Printf(PRINT_ALL, "  Display resolution: %dx%d\n", dlss.displayWidth, dlss.displayHeight);
    ri.Printf(PRINT_ALL, "  Render resolution: %dx%d\n", dlss.renderWidth, dlss.renderHeight);
    ri.Printf(PRINT_ALL, "  Quality mode: %s\n", dlssModes[dlss.mode].name);
    ri.Printf(PRINT_ALL, "  Optimal resolution: %dx%d\n", dlss.optimalWidth, dlss.optimalHeight);
    ri.Printf(PRINT_ALL, "  Min resolution: %dx%d\n", dlss.minWidth, dlss.minHeight);
    ri.Printf(PRINT_ALL, "  Max resolution: %dx%d\n", dlss.maxWidth, dlss.maxHeight);
    
    return qtrue;
}

/*
================
RTX_SetDLSSMode

Set DLSS quality mode
================
*/
void RTX_SetDLSSMode(dlssMode_t mode) {
    if (!dlss.available || mode == dlss.mode) {
        return;
    }
    
    dlss.mode = mode;
    
    // Calculate new render resolution
    RTX_GetDLSSRenderResolution(mode, dlss.displayWidth, dlss.displayHeight,
                                &dlss.renderWidth, &dlss.renderHeight);
    
    // Find NGX quality value
    NVSDK_NGX_PerfQuality_Value ngxQuality = NVSDK_NGX_PerfQuality_Value_Balanced;
    for (int i = 0; i < ARRAY_LEN(dlssModes); i++) {
        if (dlssModes[i].mode == mode) {
            ngxQuality = dlssModes[i].ngxQuality;
            break;
        }
    }
    
    if (mode == DLSS_MODE_OFF) {
        // Disable DLSS
        if (dlss.dlssFeature) {
            NVSDK_NGX_D3D12_ReleaseFeature(dlss.dlssFeature);
            dlss.dlssFeature = NULL;
        }
        ri.Printf(PRINT_ALL, "RTX: DLSS disabled\n");
        return;
    }
    
    // Recreate DLSS feature with new settings
    if (dlss.dlssFeature) {
        NVSDK_NGX_D3D12_ReleaseFeature(dlss.dlssFeature);
    }
    
    NVSDK_NGX_DLSS_Create_Params dlssCreateParams = {0};
    dlssCreateParams.Feature.InWidth = dlss.renderWidth;
    dlssCreateParams.Feature.InHeight = dlss.renderHeight;
    dlssCreateParams.Feature.InTargetWidth = dlss.displayWidth;
    dlssCreateParams.Feature.InTargetHeight = dlss.displayHeight;
    dlssCreateParams.Feature.InPerfQualityValue = ngxQuality;
    dlssCreateParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Create_Flags_None;
    
    NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(rtx.commandList, 1, 1, 
                                                         &dlss.dlssFeature,
                                                         dlss.ngxParams, &dlssCreateParams);
    
    if (NVSDK_NGX_FAILED(result)) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to recreate DLSS feature\n");
        dlss.dlssFeature = NULL;
        dlss.mode = DLSS_MODE_OFF;
        return;
    }
    
    ri.Printf(PRINT_ALL, "RTX: DLSS mode changed to %s\n", dlssModes[mode].name);
    ri.Printf(PRINT_ALL, "  New render resolution: %dx%d\n", dlss.renderWidth, dlss.renderHeight);
}

/*
================
RTX_UpscaleWithDLSS

Perform DLSS upscaling
================
*/
void RTX_UpscaleWithDLSS(void *input, void *output, int inputWidth, int inputHeight) {
    if (!dlss.available || !dlss.dlssFeature || dlss.mode == DLSS_MODE_OFF) {
        // Just copy input to output if DLSS is disabled
        Com_Memcpy(output, input, inputWidth * inputHeight * 4 * sizeof(float));
        return;
    }
    
    float startTime = ri.Milliseconds();
    
    // Setup DLSS evaluation parameters
    NVSDK_NGX_D3D12_DLSS_Eval_Params evalParams = {0};
    
    // Input color (low resolution)
    evalParams.Feature.pInColor = (ID3D12Resource*)input;
    evalParams.Feature.InSharpness = dlss.sharpness;
    
    // Output color (high resolution)
    evalParams.Feature.pInOutput = (ID3D12Resource*)output;
    
    // Motion vectors (required for temporal upsampling)
    if (dlss.motionVectors) {
        evalParams.pInMotionVectors = (ID3D12Resource*)dlss.motionVectors;
        evalParams.InMVScaleX = 1.0f;
        evalParams.InMVScaleY = 1.0f;
    }
    
    // Depth buffer (improves quality)
    if (dlss.depth) {
        evalParams.pInDepth = (ID3D12Resource*)dlss.depth;
        evalParams.InDepthInverted = 0;  // Depth values: 0=near, 1=far
    }
    
    // Exposure texture (for HDR)
    if (dlss.exposure) {
        evalParams.pInExposureTexture = (ID3D12Resource*)dlss.exposure;
        evalParams.InPreExposure = 1.0f;
    }
    
    // Camera jitter offset (for TAA integration)
    evalParams.InJitterOffsetX = 0.0f;  // Would come from TAA system
    evalParams.InJitterOffsetY = 0.0f;
    
    // Reset flag (set on resolution change or scene change)
    evalParams.InReset = (dlss.frameCount == 0) ? 1 : 0;
    
    // Frame time delta (for frame pacing)
    float currentTime = ri.Milliseconds();
    evalParams.InFrameTimeDeltaInMsec = currentTime - dlss.lastFrameTime;
    dlss.lastFrameTime = currentTime;
    
    // Render subrect (we render the full frame)
    evalParams.InRenderSubrectDimensions.Width = dlss.renderWidth;
    evalParams.InRenderSubrectDimensions.Height = dlss.renderHeight;
    evalParams.InRenderSubrectDimensions.X = 0;
    evalParams.InRenderSubrectDimensions.Y = 0;
    
    // Execute DLSS
    NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(rtx.commandList, dlss.dlssFeature,
                                                           dlss.ngxParams, &evalParams);
    
    if (NVSDK_NGX_FAILED(result)) {
        ri.Printf(PRINT_WARNING, "RTX: DLSS evaluation failed (0x%08x)\n", result);
        // Fall back to simple copy
        Com_Memcpy(output, input, inputWidth * inputHeight * 4 * sizeof(float));
        return;
    }
    
    dlss.frameCount++;
    
    // Record DLSS time
    float dlssTime = ri.Milliseconds() - startTime;
    
    if (rtx_debug->integer) {
        ri.Printf(PRINT_ALL, "DLSS: Frame %d upscaled in %.2fms\n", dlss.frameCount, dlssTime);
    }
}

/*
================
RTX_GetDLSSRenderScale

Get current DLSS render scale factor
================
*/
float RTX_GetDLSSRenderScale(void) {
    if (!dlss.available || dlss.mode == DLSS_MODE_OFF) {
        return 1.0f;
    }
    
    for (int i = 0; i < ARRAY_LEN(dlssModes); i++) {
        if (dlssModes[i].mode == dlss.mode) {
            return dlssModes[i].scaleFactor;
        }
    }
    
    return 1.0f;
}

/*
================
RTX_GetDLSSStatus

Get DLSS status string for display
================
*/
const char *RTX_GetDLSSStatus(void) {
    static char status[256];
    
    if (!dlss.available) {
        return "DLSS: Not Available";
    }
    
    if (dlss.mode == DLSS_MODE_OFF) {
        return "DLSS: Off";
    }
    
    const char *modeName = "Unknown";
    for (int i = 0; i < ARRAY_LEN(dlssModes); i++) {
        if (dlssModes[i].mode == dlss.mode) {
            modeName = dlssModes[i].name;
            break;
        }
    }
    
    Com_sprintf(status, sizeof(status), "DLSS: %s (%dx%d -> %dx%d)",
                modeName, dlss.renderWidth, dlss.renderHeight,
                dlss.displayWidth, dlss.displayHeight);
    
    return status;
}

/*
================
RTX_UpdateDLSSMotionVectors

Update motion vectors for DLSS temporal upsampling
================
*/
void RTX_UpdateDLSSMotionVectors(void *mvBuffer) {
    dlss.motionVectors = mvBuffer;
}

/*
================
RTX_UpdateDLSSDepth

Update depth buffer for DLSS
================
*/
void RTX_UpdateDLSSDepth(void *depthBuffer) {
    dlss.depth = depthBuffer;
}

/*
================
RTX_UpdateDLSSExposure

Update exposure for HDR with DLSS
================
*/
void RTX_UpdateDLSSExposure(void *exposureBuffer) {
    dlss.exposure = exposureBuffer;
}

/*
================
RTX_ShutdownDLSS

Cleanup DLSS resources
================
*/
void RTX_ShutdownDLSS(void) {
    if (!dlss.available) {
        return;
    }
    
    // Release DLSS feature
    if (dlss.dlssFeature) {
        NVSDK_NGX_D3D12_ReleaseFeature(dlss.dlssFeature);
        dlss.dlssFeature = NULL;
    }
    
    // Shutdown NGX SDK
    NVSDK_NGX_D3D12_Shutdown();
    
    Com_Memset(&dlss, 0, sizeof(dlss));
    
    ri.Printf(PRINT_ALL, "RTX: DLSS shutdown complete\n");
}

#else // !USE_DLSS

// Stub implementations when DLSS is not available

qboolean RTX_InitDLSS(void) {
    ri.Printf(PRINT_WARNING, "RTX: DLSS support not compiled in\n");
    return qfalse;
}

void RTX_SetDLSSMode(dlssMode_t mode) {
    // No-op
}

void RTX_UpscaleWithDLSS(void *input, void *output, int inputWidth, int inputHeight) {
    // Just copy input to output
    Com_Memcpy(output, input, inputWidth * inputHeight * 4 * sizeof(float));
}

float RTX_GetDLSSRenderScale(void) {
    return 1.0f;
}

const char *RTX_GetDLSSStatus(void) {
    return "DLSS: Not Available";
}

void RTX_UpdateDLSSMotionVectors(void *mvBuffer) {
    // No-op
}

void RTX_UpdateDLSSDepth(void *depthBuffer) {
    // No-op
}

void RTX_UpdateDLSSExposure(void *exposureBuffer) {
    // No-op
}

void RTX_ShutdownDLSS(void) {
    // No-op
}

#endif // USE_DLSS