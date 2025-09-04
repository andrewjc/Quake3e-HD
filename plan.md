# Quake3e-HD RTX Implementation - Detailed Execution Plan

## Overview
This plan provides step-by-step instructions to complete the RTX ray tracing implementation. The core RTX components (shaders, acceleration structures) are complete. We need to create the pipeline, compile shaders, and integrate with the render loop.

## Phase 1: Shader Compilation Setup (Day 1)

### Step 1.1: Install glslangValidator
```bash
# Windows - Download from https://github.com/KhronosGroup/glslang/releases
# Or use Vulkan SDK which includes it
# Verify installation:
glslangValidator --version
```

### Step 1.2: Create Shader Compilation Script
Create `compile_rtx_shaders.bat` in project root:
```batch
@echo off
set SHADER_DIR=src\engine\renderer\shaders\rtx
set OUTPUT_DIR=baseq3\shaders\rtx

if not exist %OUTPUT_DIR% mkdir %OUTPUT_DIR%

echo Compiling RTX shaders...
glslangValidator -V %SHADER_DIR%\raygen.rgen -o %OUTPUT_DIR%\raygen.spv --target-env vulkan1.2 --target-env spirv1.4
glslangValidator -V %SHADER_DIR%\miss.rmiss -o %OUTPUT_DIR%\miss.spv --target-env vulkan1.2 --target-env spirv1.4
glslangValidator -V %SHADER_DIR%\shadow.rmiss -o %OUTPUT_DIR%\shadow.spv --target-env vulkan1.2 --target-env spirv1.4
glslangValidator -V %SHADER_DIR%\closesthit.rchit -o %OUTPUT_DIR%\closesthit.spv --target-env vulkan1.2 --target-env spirv1.4

echo RTX shaders compiled successfully!
```

### Step 1.3: Add to Visual Studio Project
In `src\project\msvc2017\quake3e.vcxproj`, add before `</Project>`:
```xml
<ItemGroup>
  <None Include="..\..\engine\renderer\shaders\rtx\raygen.rgen" />
  <None Include="..\..\engine\renderer\shaders\rtx\miss.rmiss" />
  <None Include="..\..\engine\renderer\shaders\rtx\shadow.rmiss" />
  <None Include="..\..\engine\renderer\shaders\rtx\closesthit.rchit" />
</ItemGroup>

<Target Name="CompileRTXShaders" BeforeTargets="Build">
  <Exec Command="$(ProjectDir)..\..\..\compile_rtx_shaders.bat" />
</Target>
```

## Phase 2: Create RT Pipeline Infrastructure (Day 1-2)

### Step 2.1: Create rt_rtx_pipeline.c
New file: `src/engine/renderer/pathtracing/rt_rtx_pipeline.c`

This file will contain:
1. `RTX_LoadShaderModule()` - Load compiled SPIR-V
2. `RTX_CreateRTPipeline()` - Create ray tracing pipeline
3. `RTX_CreateShaderBindingTable()` - Setup SBT
4. `RTX_CreateDescriptorSetLayout()` - Define resource bindings
5. `RTX_AllocateDescriptorSets()` - Allocate and update sets

Key structures needed:
```c
typedef struct {
    VkShaderModule raygenShader;
    VkShaderModule missShader;
    VkShaderModule shadowMissShader;
    VkShaderModule closestHitShader;
} rtxShaders_t;

typedef struct {
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
} rtxPipelineInfo_t;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkStridedDeviceAddressRegionKHR raygenRegion;
    VkStridedDeviceAddressRegionKHR missRegion;
    VkStridedDeviceAddressRegionKHR hitRegion;
    VkStridedDeviceAddressRegionKHR callableRegion;
} rtxSBT_t;
```

### Step 2.2: Implement Shader Loading
```c
static VkShaderModule RTX_LoadShaderModule(const char *filename) {
    // 1. Read SPIR-V file from disk
    // 2. Create VkShaderModule
    // 3. Return module handle
}
```

### Step 2.3: Implement Pipeline Creation
```c
qboolean RTX_CreateRTPipeline(void) {
    // 1. Load all shader modules
    // 2. Create shader stages
    // 3. Setup shader groups (raygen, miss, hit)
    // 4. Create pipeline layout with descriptor sets
    // 5. Create ray tracing pipeline
}
```

### Step 2.4: Implement SBT Creation
```c
qboolean RTX_CreateShaderBindingTable(void) {
    // 1. Get RT pipeline properties for SBT sizing
    // 2. Calculate aligned sizes for each shader group
    // 3. Allocate SBT buffer
    // 4. Get shader group handles from pipeline
    // 5. Copy handles to SBT buffer at correct offsets
    // 6. Calculate strided device address regions
}
```

## Phase 3: Descriptor Set Management (Day 2)

### Step 3.1: Define Descriptor Set Layout
Create bindings for:
- Binding 0: TLAS (acceleration structure)
- Binding 1: Output color image (storage image)
- Binding 2: Albedo image (storage image)
- Binding 3: Normal image (storage image)
- Binding 4: Motion vector image (storage image)
- Binding 5: Depth image (storage image)
- Binding 6: Camera uniform buffer
- Binding 7: Render settings uniform buffer
- Binding 8: Environment map (sampler2D)
- Binding 9: Environment data uniform buffer
- Binding 10: Instance data buffer
- Binding 11: Material buffer
- Binding 12: Texture array (sampler2D array)
- Binding 13: Lightmap array (sampler2D array)
- Binding 14: Light buffer

### Step 3.2: Create Descriptor Pool
```c
static VkDescriptorPool RTX_CreateDescriptorPool(void) {
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 }
    };
    // Create pool with these sizes
}
```

### Step 3.3: Update Descriptor Sets
```c
void RTX_UpdateDescriptorSets(void) {
    // 1. Update TLAS binding
    // 2. Update output image bindings
    // 3. Update uniform buffer bindings
    // 4. Update texture array bindings
    // 5. Update storage buffer bindings
}
```

## Phase 4: Resource Creation (Day 2-3)

### Step 4.1: Create Uniform Buffers
```c
typedef struct {
    mat4 viewInverse;
    mat4 projInverse;
    vec3 position;
    float time;
    vec3 forward;
    float fov;
    vec3 right;
    float nearPlane;
    vec3 up;
    float farPlane;
    vec2 jitter;
    vec2 previousJitter;
    mat4 previousViewProjection;
    uint32_t frameCount;
    uint32_t enablePathTracing;
    uint32_t maxBounces;
    uint32_t samplesPerPixel;
} CameraUBO;

typedef struct {
    uint32_t enableShadows;
    uint32_t enableReflections;
    uint32_t enableGI;
    uint32_t enableAO;
    float shadowBias;
    float reflectionRoughnessCutoff;
    float giIntensity;
    float aoRadius;
    uint32_t debugMode;
    uint32_t enableDenoiser;
    uint32_t enableDLSS;
    uint32_t enableMotionBlur;
} RenderSettingsUBO;
```

### Step 4.2: Create Storage Buffers
```c
// Material buffer - convert Quake3 materials to PBR
typedef struct {
    vec4 albedo;
    vec4 specular;
    vec4 emission;
    float roughness;
    float metallic;
    float normalScale;
    float occlusionStrength;
    uint32_t albedoTexture;
    uint32_t normalTexture;
    uint32_t roughnessTexture;
    uint32_t metallicTexture;
    uint32_t emissionTexture;
    uint32_t occlusionTexture;
    uint32_t lightmapTexture;
    uint32_t flags;
} MaterialData;
```

### Step 4.3: Setup Texture Arrays
```c
void RTX_SetupTextureArrays(void) {
    // 1. Gather all loaded textures
    // 2. Create texture array descriptor
    // 3. Transition textures to shader read layout
}
```

## Phase 5: Integration with Render Loop (Day 3)

### Step 5.1: Modify tr_backend.c
Find the main rendering function and add:
```c
void RB_ExecuteRenderCommands(const void *data) {
    // ... existing code ...
    
    if (rtx_enable && rtx_enable->integer && RTX_IsAvailable()) {
        // RTX ray tracing path
        RTX_BeginFrame();
        
        // Update camera UBO
        RTX_UpdateCamera(&backEnd.viewParms);
        
        // Update scene BLAS/TLAS if needed
        if (sceneChanged) {
            RTX_UpdateScene();
        }
        
        // Dispatch rays
        RTX_TraceScene(glConfig.vidWidth, glConfig.vidHeight);
        
        // Copy RTX output to swapchain
        RTX_CopyToBackbuffer();
        
        RTX_EndFrame();
    } else {
        // Standard rasterization path
        // ... existing code ...
    }
}
```

### Step 5.2: Add RTX Scene Updates
```c
void RTX_UpdateScene(void) {
    // 1. Clear existing instances
    rtx.tlas.numInstances = 0;
    
    // 2. For each visible surface
    for (int i = 0; i < tr.refdef.num_dlights; i++) {
        // Create or update BLAS for surface geometry
        // Add instance to TLAS
    }
    
    // 3. For each entity
    for (int i = 0; i < tr.refdef.num_entities; i++) {
        // Create or update BLAS for entity model
        // Add instance to TLAS with entity transform
    }
    
    // 4. Rebuild TLAS
    RTX_BuildAccelerationStructureVK();
}
```

### Step 5.3: Implement RTX Dispatch
Update `RTX_DispatchRaysVK()` in rt_rtx_impl.c:
```c
void RTX_DispatchRaysVK(const rtxDispatchRays_t *params) {
    // 1. Begin command buffer
    // 2. Bind RT pipeline
    // 3. Bind descriptor sets
    // 4. Setup push constants if needed
    // 5. Dispatch rays with SBT regions
    // 6. Add barrier for output image
    // 7. End command buffer
    // 8. Submit to queue
}
```

## Phase 6: Material System Integration (Day 3-4)

### Step 6.1: Convert Quake3 Shaders to PBR
```c
MaterialData RTX_ConvertQ3ShaderToPBR(shader_t *shader) {
    MaterialData mat;
    
    // Base color from diffuse
    mat.albedo = shader->stages[0]->bundle[0].image[0]->averageColor;
    
    // Estimate PBR parameters
    if (shader->surfaceFlags & SURF_METALLIC) {
        mat.metallic = 1.0f;
        mat.roughness = 0.1f;
    } else if (shader->surfaceFlags & SURF_GLASS) {
        mat.metallic = 0.0f;
        mat.roughness = 0.0f;
    } else {
        mat.metallic = 0.0f;
        mat.roughness = 0.5f;
    }
    
    // Texture indices
    mat.albedoTexture = shader->stages[0]->bundle[0].image[0]->index;
    mat.normalTexture = shader->normalMap ? shader->normalMap->index : 0;
    
    return mat;
}
```

### Step 6.2: Build Material Buffer
```c
void RTX_BuildMaterialBuffer(void) {
    MaterialData *materials = malloc(sizeof(MaterialData) * tr.numShaders);
    
    for (int i = 0; i < tr.numShaders; i++) {
        materials[i] = RTX_ConvertQ3ShaderToPBR(tr.shaders[i]);
    }
    
    // Upload to GPU buffer
    RTX_UpdateBuffer(materialBuffer, materials, sizeof(MaterialData) * tr.numShaders);
    
    free(materials);
}
```

## Phase 7: Testing and Debugging (Day 4)

### Step 7.1: Create Test Configuration
Create `test_rtx.cfg`:
```
// Basic RTX test
rtx_enable 1
rtx_quality 2
rtx_denoise 0  // Start without denoiser
rtx_dlss 0     // Start without DLSS
rtx_gi_bounces 1
rtx_debug 0

// Load a simple map
map q3dm1

// Position camera
setviewpos 100 100 100 0 0
```

### Step 7.2: Debug Visualization Modes
Implement debug modes in raygen shader:
- Mode 1: Show normals
- Mode 2: Show albedo
- Mode 3: Show depth
- Mode 4: Show hit/miss
- Mode 5: Show instance IDs
- Mode 6: Show primitive IDs

### Step 7.3: Validation Layers
Enable Vulkan validation:
```c
// In vk_initialize()
const char *validationLayers[] = {
    "VK_LAYER_KHRONOS_validation"
};

// Add to instance creation
instanceInfo.enabledLayerCount = 1;
instanceInfo.ppEnabledLayerNames = validationLayers;
```

## Phase 8: Optional Features (Day 5)

### Step 8.1: Enable Denoiser
1. Add to project settings:
   - Define: `USE_OPTIX` or `USE_OIDN`
   - Link: OptiX or OpenImageDenoise library
   
2. Download and install:
   - OptiX: https://developer.nvidia.com/optix
   - OIDN: https://www.openimagedenoise.org/

3. Update CMakeLists.txt or vcxproj

### Step 8.2: Enable DLSS
1. Get NVIDIA NGX SDK
2. Add to project:
   - Define: `USE_DLSS`
   - Link: nvsdk_ngx.lib
3. Copy NGX binaries to output directory

### Step 8.3: Motion Vectors
Implement in vertex shader and pass to RT:
```glsl
// In vertex shader
prevWorldPos = previousMVP * vec4(position, 1.0);
currWorldPos = currentMVP * vec4(position, 1.0);

// In raygen shader
vec2 prevScreen = (prevWorldPos.xy / prevWorldPos.w) * 0.5 + 0.5;
vec2 currScreen = (currWorldPos.xy / currWorldPos.w) * 0.5 + 0.5;
motionVector = currScreen - prevScreen;
```

## Build Commands

### Windows (Visual Studio)
```batch
# Debug build
msbuild quake3e.sln /p:Configuration=Debug /p:Platform=x64

# Release build with RTX
msbuild quake3e.sln /p:Configuration=Release /p:Platform=x64 /p:DefineConstants="USE_RTX"

# Release with all features
msbuild quake3e.sln /p:Configuration=Release /p:Platform=x64 /p:DefineConstants="USE_RTX;USE_OPTIX;USE_DLSS"
```

### Linux (Make)
```bash
# Basic RTX
make USE_RTX=1

# With denoiser
make USE_RTX=1 USE_OIDN=1

# Full features
make USE_RTX=1 USE_OIDN=1 USE_DLSS=1
```

## Validation Checklist

### Pre-Launch Checks
- [ ] Shader files compiled to SPIR-V
- [ ] SPIR-V files in correct directory
- [ ] RTX extensions loaded
- [ ] GPU supports ray tracing

### Runtime Checks
- [ ] BLAS creation succeeds
- [ ] TLAS build completes
- [ ] Pipeline creation succeeds
- [ ] SBT properly formatted
- [ ] Descriptor sets bound
- [ ] Ray dispatch executes

### Visual Checks
- [ ] Image output not black
- [ ] Shadows visible
- [ ] Materials look correct
- [ ] No visual artifacts

## Performance Targets

### Initial Goals (No Optimization)
- 1080p @ 30 FPS on RTX 3060
- 1080p @ 60 FPS on RTX 4070
- AS Build < 5ms per frame
- Ray Trace < 16ms per frame

### Optimized Goals
- 1080p @ 60 FPS on RTX 3060
- 1440p @ 60 FPS on RTX 4070
- 4K @ 60 FPS on RTX 4090 with DLSS
- AS Build < 2ms per frame
- Ray Trace < 10ms per frame

## Common Issues and Solutions

### Issue: Shader compilation fails
**Solution**: Ensure glslangValidator is in PATH and using correct target environment

### Issue: Black screen with RTX enabled
**Solution**: Check descriptor set bindings and ensure TLAS is built

### Issue: Crash on RTX enable
**Solution**: Verify all RT extensions loaded and function pointers valid

### Issue: Poor performance
**Solution**: 
1. Reduce rtx_quality setting
2. Enable DLSS
3. Reduce rtx_gi_bounces
4. Check AS rebuild frequency

### Issue: Visual artifacts
**Solution**:
1. Check normal transformation in shader
2. Verify material buffer data
3. Ensure proper barrier synchronization

## Timeline Summary

### Day 1
- Morning: Setup shader compilation
- Afternoon: Start pipeline creation

### Day 2  
- Morning: Complete pipeline and SBT
- Afternoon: Descriptor set management

### Day 3
- Morning: Resource creation
- Afternoon: Render loop integration

### Day 4
- Morning: Material system
- Afternoon: Testing and debugging

### Day 5
- Morning: Fix issues found in testing
- Afternoon: Optional features (denoiser/DLSS)

## Success Criteria

The RTX implementation is complete when:
1. ✅ RTX can be enabled without crashes
2. ✅ Scene renders with ray tracing
3. ✅ Shadows and lighting work correctly
4. ✅ Performance meets minimum targets
5. ✅ All debug modes functional
6. ✅ Can switch between RTX and raster
7. ✅ No memory leaks or validation errors

## Next Steps After Completion

1. **Optimization Phase**
   - Profile with NSight/RenderDoc
   - Optimize BLAS update frequency
   - Implement BLAS compaction
   - Add LOD system

2. **Feature Enhancement**
   - Add more material types
   - Implement area lights
   - Add volumetric effects
   - Implement RT-based particles

3. **Quality Improvements**
   - Tune denoiser settings
   - Improve motion vectors
   - Add temporal accumulation
   - Implement variable rate shading