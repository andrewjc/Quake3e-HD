/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

RTX Denoiser Implementation
Supports NVIDIA OptiX, Intel Open Image Denoise, and AMD FidelityFX
===========================================================================
*/

#include "rt_rtx.h"
#include "rt_pathtracer.h"
#include "../tr_local.h"
#include <math.h>

// ============================================================================
// NVIDIA OptiX Denoiser
// ============================================================================

#ifdef USE_OPTIX
#include <optix.h>
#include <optix_stubs.h>

typedef struct optixDenoiser_s {
    OptixDeviceContext      context;
    OptixDenoiser           denoiser;
    OptixDenoiserParams     params;
    OptixDenoiserSizes      sizes;
    
    // Buffers
    CUdeviceptr             stateBuffer;
    CUdeviceptr             scratchBuffer;
    CUdeviceptr             intensity;
    CUdeviceptr             avgColor;
    
    // Input/output layers
    OptixImage2D            inputLayer;
    OptixImage2D            outputLayer;
    OptixImage2D            albedoLayer;
    OptixImage2D            normalLayer;
    OptixImage2D            flowLayer;
    
    size_t                  stateSize;
    size_t                  scratchSize;
    
    qboolean                useAlbedo;
    qboolean                useNormals;
    qboolean                useTemporal;
} optixDenoiser_t;

static optixDenoiser_t optixDenoise;

/*
================
RTX_InitOptiXDenoiser

Initialize NVIDIA OptiX denoiser
================
*/
static qboolean RTX_InitOptiXDenoiser(int width, int height) {
    OptixResult result;
    
    // Initialize OptiX
    result = optixInit();
    if (result != OPTIX_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to initialize OptiX (%s)\n",
                  optixGetErrorString(result));
        return qfalse;
    }
    
    // Create OptiX context
    CUcontext cuContext = 0;  // Use current CUDA context
    OptixDeviceContextOptions options = {
        .logCallbackFunction = NULL,
        .logCallbackLevel = 4
    };
    
    result = optixDeviceContextCreate(cuContext, &options, &optixDenoise.context);
    if (result != OPTIX_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create OptiX context (%s)\n",
                  optixGetErrorString(result));
        return qfalse;
    }
    
    // Create denoiser
    OptixDenoiserOptions denoiserOptions = {
        .guideAlbedo = 1,  // Use albedo guide
        .guideNormal = 1   // Use normal guide
    };
    
    OptixDenoiserModelKind model = OPTIX_DENOISER_MODEL_KIND_HDR;
    
    result = optixDenoiserCreate(optixDenoise.context, model, &denoiserOptions,
                                  &optixDenoise.denoiser);
    if (result != OPTIX_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create OptiX denoiser (%s)\n",
                  optixGetErrorString(result));
        optixDeviceContextDestroy(optixDenoise.context);
        return qfalse;
    }
    
    // Setup denoiser
    result = optixDenoiserComputeMemoryResources(optixDenoise.denoiser,
                                                  width, height,
                                                  &optixDenoise.sizes);
    if (result != OPTIX_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to compute denoiser memory (%s)\n",
                  optixGetErrorString(result));
        optixDenoiserDestroy(optixDenoise.denoiser);
        optixDeviceContextDestroy(optixDenoise.context);
        return qfalse;
    }
    
    // Allocate denoiser state
    optixDenoise.stateSize = optixDenoise.sizes.stateSizeInBytes;
    cudaMalloc((void**)&optixDenoise.stateBuffer, optixDenoise.stateSize);
    
    // Allocate scratch buffer
    optixDenoise.scratchSize = optixDenoise.sizes.withoutOverlapScratchSizeInBytes;
    cudaMalloc((void**)&optixDenoise.scratchBuffer, optixDenoise.scratchSize);
    
    // Allocate intensity buffer
    cudaMalloc((void**)&optixDenoise.intensity, sizeof(float));
    
    // Setup denoiser state
    CUstream stream = 0;  // Use default stream
    result = optixDenoiserSetup(optixDenoise.denoiser, stream,
                                width, height,
                                optixDenoise.stateBuffer, optixDenoise.stateSize,
                                optixDenoise.scratchBuffer, optixDenoise.scratchSize);
    
    if (result != OPTIX_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to setup OptiX denoiser (%s)\n",
                  optixGetErrorString(result));
        cudaFree((void*)optixDenoise.stateBuffer);
        cudaFree((void*)optixDenoise.scratchBuffer);
        cudaFree((void*)optixDenoise.intensity);
        optixDenoiserDestroy(optixDenoise.denoiser);
        optixDeviceContextDestroy(optixDenoise.context);
        return qfalse;
    }
    
    // Setup image descriptors
    optixDenoise.inputLayer.data = 0;  // Will be set per frame
    optixDenoise.inputLayer.width = width;
    optixDenoise.inputLayer.height = height;
    optixDenoise.inputLayer.rowStrideInBytes = width * sizeof(float) * 4;
    optixDenoise.inputLayer.pixelStrideInBytes = sizeof(float) * 4;
    optixDenoise.inputLayer.format = OPTIX_PIXEL_FORMAT_FLOAT4;
    
    optixDenoise.outputLayer = optixDenoise.inputLayer;
    optixDenoise.albedoLayer = optixDenoise.inputLayer;
    optixDenoise.normalLayer = optixDenoise.inputLayer;
    
    optixDenoise.useAlbedo = qtrue;
    optixDenoise.useNormals = qtrue;
    optixDenoise.useTemporal = qfalse;
    
    ri.Printf(PRINT_ALL, "RTX: OptiX denoiser initialized\n");
    ri.Printf(PRINT_ALL, "  State size: %.2f MB\n", optixDenoise.stateSize / (1024.0f * 1024.0f));
    ri.Printf(PRINT_ALL, "  Scratch size: %.2f MB\n", optixDenoise.scratchSize / (1024.0f * 1024.0f));
    
    return qtrue;
}

/*
================
RTX_DenoiseOptiX

Denoise frame using OptiX
================
*/
static void RTX_DenoiseOptiX(void *input, void *albedo, void *normal, void *output) {
    if (!optixDenoise.denoiser) {
        return;
    }
    
    float startTime = ri.Milliseconds();
    
    // Set input/output buffers
    optixDenoise.inputLayer.data = (CUdeviceptr)input;
    optixDenoise.outputLayer.data = (CUdeviceptr)output;
    
    OptixDenoiserGuideLayer guideLayer = {0};
    
    if (optixDenoise.useAlbedo && albedo) {
        optixDenoise.albedoLayer.data = (CUdeviceptr)albedo;
        guideLayer.albedo = optixDenoise.albedoLayer;
    }
    
    if (optixDenoise.useNormals && normal) {
        optixDenoise.normalLayer.data = (CUdeviceptr)normal;
        guideLayer.normal = optixDenoise.normalLayer;
    }
    
    // Compute intensity
    CUstream stream = 0;
    OptixResult result = optixDenoiserComputeIntensity(
        optixDenoise.denoiser, stream,
        &optixDenoise.inputLayer,
        optixDenoise.intensity,
        optixDenoise.scratchBuffer,
        optixDenoise.scratchSize);
    
    if (result != OPTIX_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: OptiX intensity computation failed\n");
        return;
    }
    
    // Setup denoiser parameters
    optixDenoise.params.denoiseAlpha = 0;  // Don't denoise alpha channel
    optixDenoise.params.hdrIntensity = optixDenoise.intensity;
    optixDenoise.params.blendFactor = rtx.denoiser.blendFactor;
    
    // Run denoiser
    OptixDenoiserLayer inputLayer = {
        .input = optixDenoise.inputLayer,
        .previousOutput = optixDenoise.useTemporal ? optixDenoise.outputLayer : (OptixImage2D){0}
    };
    
    OptixDenoiserLayer outputLayer = {
        .output = optixDenoise.outputLayer
    };
    
    result = optixDenoiserInvoke(
        optixDenoise.denoiser, stream,
        &optixDenoise.params,
        optixDenoise.stateBuffer, optixDenoise.stateSize,
        &guideLayer,
        &inputLayer, 1,  // Single input layer
        0, 0,  // No AOV layers
        &outputLayer,
        optixDenoise.scratchBuffer, optixDenoise.scratchSize);
    
    if (result != OPTIX_SUCCESS) {
        ri.Printf(PRINT_WARNING, "RTX: OptiX denoiser invocation failed\n");
        return;
    }
    
    // Synchronize
    cudaStreamSynchronize(stream);
    
    rtx.denoiseTime = ri.Milliseconds() - startTime;
}

/*
================
RTX_ShutdownOptiXDenoiser

Cleanup OptiX denoiser
================
*/
static void RTX_ShutdownOptiXDenoiser(void) {
    if (optixDenoise.stateBuffer) {
        cudaFree((void*)optixDenoise.stateBuffer);
    }
    if (optixDenoise.scratchBuffer) {
        cudaFree((void*)optixDenoise.scratchBuffer);
    }
    if (optixDenoise.intensity) {
        cudaFree((void*)optixDenoise.intensity);
    }
    if (optixDenoise.denoiser) {
        optixDenoiserDestroy(optixDenoise.denoiser);
    }
    if (optixDenoise.context) {
        optixDeviceContextDestroy(optixDenoise.context);
    }
    
    Com_Memset(&optixDenoise, 0, sizeof(optixDenoise));
}

#endif // USE_OPTIX

// ============================================================================
// Intel Open Image Denoise
// ============================================================================

#ifdef USE_OIDN
#include <OpenImageDenoise/oidn.h>

typedef struct oidnDenoiser_s {
    OIDNDevice              device;
    OIDNFilter              filter;
    OIDNBuffer              colorBuffer;
    OIDNBuffer              albedoBuffer;
    OIDNBuffer              normalBuffer;
    OIDNBuffer              outputBuffer;
    
    int                     width;
    int                     height;
    qboolean                hdr;
    qboolean                useAlbedo;
    qboolean                useNormals;
} oidnDenoiser_t;

static oidnDenoiser_t oidnDenoise;

/*
================
RTX_InitOIDNDenoiser

Initialize Intel Open Image Denoise
================
*/
static qboolean RTX_InitOIDNDenoiser(int width, int height) {
    // Create OIDN device
    oidnDenoise.device = oidnNewDevice(OIDN_DEVICE_TYPE_DEFAULT);
    if (!oidnDenoise.device) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create OIDN device\n");
        return qfalse;
    }
    
    // Set device properties
    oidnSetDevice1b(oidnDenoise.device, "setAffinity", qtrue);
    oidnCommitDevice(oidnDenoise.device);
    
    // Check for errors
    const char *errorMessage;
    if (oidnGetDeviceError(oidnDenoise.device, &errorMessage) != OIDN_ERROR_NONE) {
        ri.Printf(PRINT_WARNING, "RTX: OIDN device error: %s\n", errorMessage);
        oidnReleaseDevice(oidnDenoise.device);
        return qfalse;
    }
    
    // Create filter
    oidnDenoise.filter = oidnNewFilter(oidnDenoise.device, "RT");  // Ray tracing filter
    if (!oidnDenoise.filter) {
        ri.Printf(PRINT_WARNING, "RTX: Failed to create OIDN filter\n");
        oidnReleaseDevice(oidnDenoise.device);
        return qfalse;
    }
    
    // Create buffers
    size_t bufferSize = width * height * 3 * sizeof(float);
    
    oidnDenoise.colorBuffer = oidnNewBuffer(oidnDenoise.device, bufferSize);
    oidnDenoise.outputBuffer = oidnNewBuffer(oidnDenoise.device, bufferSize);
    
    if (rtx_denoise->integer > 1) {
        oidnDenoise.albedoBuffer = oidnNewBuffer(oidnDenoise.device, bufferSize);
        oidnDenoise.normalBuffer = oidnNewBuffer(oidnDenoise.device, bufferSize);
        oidnDenoise.useAlbedo = qtrue;
        oidnDenoise.useNormals = qtrue;
    }
    
    // Set filter images
    oidnSetFilterImage(oidnDenoise.filter, "color", oidnDenoise.colorBuffer,
                       OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
    oidnSetFilterImage(oidnDenoise.filter, "output", oidnDenoise.outputBuffer,
                       OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
    
    if (oidnDenoise.useAlbedo) {
        oidnSetFilterImage(oidnDenoise.filter, "albedo", oidnDenoise.albedoBuffer,
                          OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
    }
    
    if (oidnDenoise.useNormals) {
        oidnSetFilterImage(oidnDenoise.filter, "normal", oidnDenoise.normalBuffer,
                          OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
    }
    
    // Set filter parameters
    oidnSetFilter1b(oidnDenoise.filter, "hdr", qtrue);  // HDR mode
    oidnSetFilter1b(oidnDenoise.filter, "cleanAux", qtrue);  // Denoise auxiliary images
    oidnSetFilter1f(oidnDenoise.filter, "inputScale", 1.0f);  // Input scale
    
    // Commit filter
    oidnCommitFilter(oidnDenoise.filter);
    
    // Check for errors
    if (oidnGetDeviceError(oidnDenoise.device, &errorMessage) != OIDN_ERROR_NONE) {
        ri.Printf(PRINT_WARNING, "RTX: OIDN filter error: %s\n", errorMessage);
        RTX_ShutdownOIDNDenoiser();
        return qfalse;
    }
    
    oidnDenoise.width = width;
    oidnDenoise.height = height;
    oidnDenoise.hdr = qtrue;
    
    // Get device info
    const char *deviceName = NULL;
    oidnGetDeviceString(oidnDenoise.device, "name", &deviceName);
    
    ri.Printf(PRINT_ALL, "RTX: Intel OIDN initialized\n");
    ri.Printf(PRINT_ALL, "  Device: %s\n", deviceName ? deviceName : "Unknown");
    ri.Printf(PRINT_ALL, "  Auxiliary buffers: %s\n",
              oidnDenoise.useAlbedo ? "Albedo + Normals" : "None");
    
    return qtrue;
}

/*
================
RTX_DenoiseOIDN

Denoise frame using Intel OIDN
================
*/
static void RTX_DenoiseOIDN(void *input, void *albedo, void *normal, void *output) {
    if (!oidnDenoise.filter) {
        return;
    }
    
    float startTime = ri.Milliseconds();
    
    // Upload input data
    void *colorPtr = oidnGetBufferData(oidnDenoise.colorBuffer);
    Com_Memcpy(colorPtr, input, oidnDenoise.width * oidnDenoise.height * 3 * sizeof(float));
    
    if (oidnDenoise.useAlbedo && albedo) {
        void *albedoPtr = oidnGetBufferData(oidnDenoise.albedoBuffer);
        Com_Memcpy(albedoPtr, albedo, oidnDenoise.width * oidnDenoise.height * 3 * sizeof(float));
    }
    
    if (oidnDenoise.useNormals && normal) {
        void *normalPtr = oidnGetBufferData(oidnDenoise.normalBuffer);
        Com_Memcpy(normalPtr, normal, oidnDenoise.width * oidnDenoise.height * 3 * sizeof(float));
    }
    
    // Execute filter
    oidnExecuteFilter(oidnDenoise.filter);
    
    // Check for errors
    const char *errorMessage;
    if (oidnGetDeviceError(oidnDenoise.device, &errorMessage) != OIDN_ERROR_NONE) {
        ri.Printf(PRINT_WARNING, "RTX: OIDN execution error: %s\n", errorMessage);
        return;
    }
    
    // Download output data
    void *outputPtr = oidnGetBufferData(oidnDenoise.outputBuffer);
    Com_Memcpy(output, outputPtr, oidnDenoise.width * oidnDenoise.height * 3 * sizeof(float));
    
    rtx.denoiseTime = ri.Milliseconds() - startTime;
}

/*
================
RTX_ShutdownOIDNDenoiser

Cleanup Intel OIDN
================
*/
static void RTX_ShutdownOIDNDenoiser(void) {
    if (oidnDenoise.filter) {
        oidnReleaseFilter(oidnDenoise.filter);
    }
    if (oidnDenoise.colorBuffer) {
        oidnReleaseBuffer(oidnDenoise.colorBuffer);
    }
    if (oidnDenoise.albedoBuffer) {
        oidnReleaseBuffer(oidnDenoise.albedoBuffer);
    }
    if (oidnDenoise.normalBuffer) {
        oidnReleaseBuffer(oidnDenoise.normalBuffer);
    }
    if (oidnDenoise.outputBuffer) {
        oidnReleaseBuffer(oidnDenoise.outputBuffer);
    }
    if (oidnDenoise.device) {
        oidnReleaseDevice(oidnDenoise.device);
    }
    
    Com_Memset(&oidnDenoise, 0, sizeof(oidnDenoise));
}

#endif // USE_OIDN

// ============================================================================
// Public Denoiser Interface
// ============================================================================

/*
================
RTX_InitDenoiser

Initialize hardware denoiser
================
*/
qboolean RTX_InitDenoiser(int width, int height) {
    rtx.denoiser.enabled = qfalse;
    rtx.denoiser.width = width;
    rtx.denoiser.height = height;
    rtx.denoiser.blendFactor = 0.95f;
    
    // Try NVIDIA OptiX first (best quality for RTX cards)
    if (rtx.gpuType == RTX_GPU_NVIDIA) {
#ifdef USE_OPTIX
        if (RTX_InitOptiXDenoiser(width, height)) {
            rtx.denoiser.enabled = qtrue;
            rtx.denoiser.context = (void*)1;  // Mark as OptiX
            ri.Printf(PRINT_ALL, "RTX: Using NVIDIA OptiX denoiser\n");
            return qtrue;
        }
#endif
    }
    
    // Try Intel OIDN (works on all hardware)
#ifdef USE_OIDN
    if (RTX_InitOIDNDenoiser(width, height)) {
        rtx.denoiser.enabled = qtrue;
        rtx.denoiser.context = (void*)2;  // Mark as OIDN
        ri.Printf(PRINT_ALL, "RTX: Using Intel Open Image Denoise\n");
        return qtrue;
    }
#endif
    
    ri.Printf(PRINT_WARNING, "RTX: No denoiser available\n");
    return qfalse;
}

/*
================
RTX_DenoiseFrame

Apply denoising to rendered frame
================
*/
void RTX_DenoiseFrame(void *input, void *output) {
    if (!rtx.denoiser.enabled) {
        // Just copy input to output if no denoiser
        Com_Memcpy(output, input, rtx.denoiser.width * rtx.denoiser.height * 4 * sizeof(float));
        return;
    }
    
    // Get auxiliary buffers if available
    void *albedo = rtx.denoiser.albedoBuffer;
    void *normal = rtx.denoiser.normalBuffer;
    
#ifdef USE_OPTIX
    if (rtx.denoiser.context == (void*)1) {
        RTX_DenoiseOptiX(input, albedo, normal, output);
        return;
    }
#endif
    
#ifdef USE_OIDN
    if (rtx.denoiser.context == (void*)2) {
        RTX_DenoiseOIDN(input, albedo, normal, output);
        return;
    }
#endif
}

/*
================
RTX_ShutdownDenoiser

Cleanup denoiser resources
================
*/
void RTX_ShutdownDenoiser(void) {
    if (!rtx.denoiser.enabled) {
        return;
    }
    
#ifdef USE_OPTIX
    if (rtx.denoiser.context == (void*)1) {
        RTX_ShutdownOptiXDenoiser();
    }
#endif
    
#ifdef USE_OIDN
    if (rtx.denoiser.context == (void*)2) {
        RTX_ShutdownOIDNDenoiser();
    }
#endif
    
    rtx.denoiser.enabled = qfalse;
    rtx.denoiser.context = NULL;
}