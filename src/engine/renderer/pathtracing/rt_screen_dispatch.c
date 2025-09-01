/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Full-Screen Ray Tracing Dispatch
Renders complete frames using path tracing
===========================================================================
*/

#include "rt_pathtracer.h"
#include "../tr_local.h"

// Screen ray tracing state
typedef struct {
    float *colorBuffer;      // RGB float buffer
    float *depthBuffer;      // Depth buffer
    float *normalBuffer;     // World space normals
    float *albedoBuffer;     // Base color for denoising
    int width;
    int height;
    int currentSample;      // For progressive rendering
    qboolean initialized;
} screenRT_t;

static screenRT_t screenRT;

/*
================
RT_AllocateScreenBuffers

Allocate buffers for full-screen ray tracing
================
*/
void RT_AllocateScreenBuffers(int width, int height) {
    if (screenRT.initialized && screenRT.width == width && screenRT.height == height) {
        return;
    }
    
    // Free old buffers
    if (screenRT.colorBuffer) ri.Free(screenRT.colorBuffer);
    if (screenRT.depthBuffer) ri.Free(screenRT.depthBuffer);
    if (screenRT.normalBuffer) ri.Free(screenRT.normalBuffer);
    if (screenRT.albedoBuffer) ri.Free(screenRT.albedoBuffer);
    
    // Allocate new buffers
    int pixelCount = width * height;
    screenRT.colorBuffer = ri.Malloc(pixelCount * 3 * sizeof(float));
    screenRT.depthBuffer = ri.Malloc(pixelCount * sizeof(float));
    screenRT.normalBuffer = ri.Malloc(pixelCount * 3 * sizeof(float));
    screenRT.albedoBuffer = ri.Malloc(pixelCount * 3 * sizeof(float));
    
    screenRT.width = width;
    screenRT.height = height;
    screenRT.initialized = qtrue;
    screenRT.currentSample = 0;
    
    ri.Printf(PRINT_ALL, "RT: Allocated screen buffers %dx%d\n", width, height);
}

/*
================
RT_GeneratePrimaryRay

Generate ray from camera through pixel
================
*/
static void RT_GeneratePrimaryRay(int x, int y, ray_t *ray) {
    // Get camera vectors from backend
    vec3_t forward, right, up;
    VectorCopy(backEnd.viewParms.or.axis[0], forward);
    VectorCopy(backEnd.viewParms.or.axis[1], right);
    VectorCopy(backEnd.viewParms.or.axis[2], up);
    
    // Convert pixel to NDC (-1 to 1)
    float ndcX = (2.0f * x / screenRT.width) - 1.0f;
    float ndcY = 1.0f - (2.0f * y / screenRT.height);
    
    // Apply FOV
    float tanHalfFov = tan(DEG2RAD(backEnd.viewParms.fovX * 0.5f));
    float aspectRatio = (float)screenRT.width / screenRT.height;
    
    ndcX *= tanHalfFov * aspectRatio;
    ndcY *= tanHalfFov;
    
    // Generate ray direction
    VectorCopy(backEnd.viewParms.or.origin, ray->origin);
    
    ray->direction[0] = forward[0] + ndcX * right[0] + ndcY * up[0];
    ray->direction[1] = forward[1] + ndcX * right[1] + ndcY * up[1];
    ray->direction[2] = forward[2] + ndcX * right[2] + ndcY * up[2];
    VectorNormalize(ray->direction);
    
    ray->tMin = 0.01f;
    ray->tMax = 10000.0f;
}

/*
================
RT_RenderScreenPixel

Render a single pixel using path tracing
================
*/
static void RT_RenderScreenPixel(int x, int y) {
    int pixelIndex = y * screenRT.width + x;
    
    // Generate primary ray
    ray_t ray;
    RT_GeneratePrimaryRay(x, y, &ray);
    
    // Trace path
    vec3_t color;
    RT_TracePath(&ray, 0, color);
    
    // Store results
    screenRT.colorBuffer[pixelIndex * 3 + 0] = color[0];
    screenRT.colorBuffer[pixelIndex * 3 + 1] = color[1];
    screenRT.colorBuffer[pixelIndex * 3 + 2] = color[2];
    
    // Trace for additional buffers (for denoising)
    hitInfo_t hit;
    if (RT_TraceRay(&ray, &hit)) {
        screenRT.depthBuffer[pixelIndex] = hit.t;
        VectorCopy(hit.normal, &screenRT.normalBuffer[pixelIndex * 3]);
        VectorCopy(hit.shader->baseColor, &screenRT.albedoBuffer[pixelIndex * 3]);
    } else {
        screenRT.depthBuffer[pixelIndex] = ray.tMax;
        VectorClear(&screenRT.normalBuffer[pixelIndex * 3]);
        VectorSet(&screenRT.albedoBuffer[pixelIndex * 3], 0.5f, 0.7f, 1.0f); // Sky color
    }
}

/*
================
RT_RenderFullScreen

Main full-screen ray tracing function
This replaces the entire rasterization pipeline
================
*/
void RT_RenderFullScreen(void) {
    if (!rt_enable || !rt_enable->integer) {
        return;
    }
    
    // Ensure buffers are allocated
    RT_AllocateScreenBuffers(glConfig.vidWidth, glConfig.vidHeight);
    
    // Progressive rendering - render in tiles for interactivity
    int tileSize = 32;
    int tilesX = (screenRT.width + tileSize - 1) / tileSize;
    int tilesY = (screenRT.height + tileSize - 1) / tileSize;
    int totalTiles = tilesX * tilesY;
    
    // Render a subset of tiles per frame for responsiveness
    int tilesPerFrame = MAX(1, totalTiles / 16);
    int startTile = (screenRT.currentSample * tilesPerFrame) % totalTiles;
    
    for (int i = 0; i < tilesPerFrame && startTile + i < totalTiles; i++) {
        int tileIndex = startTile + i;
        int tileX = tileIndex % tilesX;
        int tileY = tileIndex / tilesX;
        
        int startX = tileX * tileSize;
        int startY = tileY * tileSize;
        int endX = MIN(startX + tileSize, screenRT.width);
        int endY = MIN(startY + tileSize, screenRT.height);
        
        // Render tile
        for (int y = startY; y < endY; y++) {
            for (int x = startX; x < endX; x++) {
                RT_RenderScreenPixel(x, y);
            }
        }
    }
    
    screenRT.currentSample++;
    
    // Apply denoising if available
    if (rtx_denoise && rtx_denoise->integer && RTX_IsAvailable()) {
        RTX_DenoiseImage(screenRT.colorBuffer, screenRT.albedoBuffer, 
                        screenRT.normalBuffer, screenRT.width, screenRT.height);
    }
}

/*
================
RT_CopyToFramebuffer

Copy ray traced results to OpenGL/Vulkan framebuffer
================
*/
void RT_CopyToFramebuffer(void) {
    if (!screenRT.initialized || !screenRT.colorBuffer) {
        return;
    }
    
#ifdef USE_VULKAN
    // Vulkan path - copy to swapchain image
    vk_upload_screen_texture(screenRT.colorBuffer, screenRT.width, screenRT.height);
#else
    // OpenGL path - draw fullscreen quad with RT results
    GL_Bind(tr.screenImageRT);
    
    // Upload RT buffer to texture
    qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, 
                  screenRT.width, screenRT.height, 0,
                  GL_RGB, GL_FLOAT, screenRT.colorBuffer);
    
    // Draw fullscreen quad
    GL_State(GLS_DEPTHTEST_DISABLE);
    
    qglBegin(GL_QUADS);
    qglTexCoord2f(0, 0);
    qglVertex2f(-1, -1);
    qglTexCoord2f(1, 0);
    qglVertex2f(1, -1);
    qglTexCoord2f(1, 1);
    qglVertex2f(1, 1);
    qglTexCoord2f(0, 1);
    qglVertex2f(-1, 1);
    qglEnd();
#endif
}

/*
================
RT_ScreenDispatchCommands

Process RT commands for screen rendering
================
*/
void RT_ScreenDispatchCommands(void) {
    // Check if we should replace rasterization
    if (!rt_enable || !rt_enable->integer) {
        return;
    }
    
    if (Q_stricmp(rt_mode->string, "replace") == 0) {
        // Full replacement mode - skip rasterization entirely
        RT_RenderFullScreen();
        RT_CopyToFramebuffer();
    } else if (Q_stricmp(rt_mode->string, "hybrid") == 0) {
        // Hybrid mode - rasterize geometry, ray trace lighting
        // This is handled by the existing point-query system
    }
}

/*
================
RT_FreeScreenBuffers

Free screen RT buffers
================
*/
void RT_FreeScreenBuffers(void) {
    if (screenRT.colorBuffer) ri.Free(screenRT.colorBuffer);
    if (screenRT.depthBuffer) ri.Free(screenRT.depthBuffer);
    if (screenRT.normalBuffer) ri.Free(screenRT.normalBuffer);
    if (screenRT.albedoBuffer) ri.Free(screenRT.albedoBuffer);
    
    Com_Memset(&screenRT, 0, sizeof(screenRT));
}