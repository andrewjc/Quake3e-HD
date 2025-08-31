# Phase 3: Material System Integration Guide

## Overview
Phase 3 introduces a modern material system that replaces the shader system while maintaining full backward compatibility with Quake 3 .shader files.

## Components Implemented

### Core Files
1. **tr_material.h** - Material system structures and API definitions
2. **tr_material.c** - Core material parsing and management
3. **tr_shader_compat.c** - Backward compatibility layer for existing shaders
4. **tr_expression.c** - Dynamic expression evaluation system
5. **tr_material_opt.c** - Material optimization routines

### Build System Updates
- **tr_local.h** - Added `#include "tr_material.h"` 
- **quake3e.vcxproj** - Added all material system source files
- **Makefile** - Added material system object files to Q3RENDVOBJ

## Integration Points

### 1. Initialization
```c
// In R_Init() after shader system init
R_InitMaterialSystem();
```

### 2. Shader Loading Replacement
```c
// Old code:
shader_t *shader = R_FindShader(name, lightmapIndex, qtrue);

// New code with material system:
material_t *material = Material_Find(name);
if (!material) {
    material = Material_CreateDefault(name, lightmapIndex);
}
// For compatibility, get shader wrapper:
shader_t *shader = material->legacyShader;
```

### 3. Material File Loading
```c
// In R_LoadShaders() or equivalent:
void R_LoadMaterials(void) {
    char **shaderFiles;
    int numShaderFiles;
    int i;
    
    // Get list of shader files
    shaderFiles = ri.FS_ListFiles("scripts", ".shader", &numShaderFiles);
    
    for (i = 0; i < numShaderFiles; i++) {
        char *buffer;
        char *p;
        char *token;
        
        ri.FS_ReadFile(va("scripts/%s", shaderFiles[i]), (void**)&buffer);
        if (!buffer) continue;
        
        p = buffer;
        while (1) {
            token = COM_ParseExt(&p, qtrue);
            if (!*token) break;
            
            // Parse as material first, fall back to shader
            material_t *mat = Material_Parse(token, &p);
            if (!mat) {
                // Fall back to legacy shader parsing
                shader_t *shader = R_ParseShader(token, &p);
                if (shader) {
                    Material_FromShader(shader);
                }
            }
        }
        
        ri.FS_FreeFile(buffer);
    }
    
    ri.FS_FreeFileList(shaderFiles);
}
```

### 4. Rendering Integration
```c
// In RB_BeginSurface():
void RB_BeginSurface(shader_t *shader, int fogNum) {
    material_t *material = shader->material;
    
    if (material) {
        // Use material path
        tess.material = material;
        
        // Evaluate dynamic expressions
        Material_EvaluateExpressions(material, tess.shaderTime);
        
        // Setup for material rendering
        RB_IterateMaterialStages(material);
    } else {
        // Legacy shader path
        tess.shader = shader;
        RB_IterateShaderStages();
    }
}
```

### 5. Stage Iteration
```c
// In RB_IterateMaterialStages():
void RB_IterateMaterialStages(material_t *material) {
    int stage;
    
    // Check for optimized paths
    if (material->isStaticMaterial && material->hasLightmap) {
        // Fast path for static lightmapped surfaces
        RB_RenderLightmappedMaterial(material);
        return;
    }
    
    // Iterate through stages
    for (stage = 0; stage < material->numStages; stage++) {
        materialStage_t *pStage = &material->stages[stage];
        
        if (!pStage->active) {
            continue;
        }
        
        RB_SetupMaterialStage(pStage);
        
        // Draw the stage
        R_DrawElements(tess.numIndexes, tess.indexes);
    }
}
```

## Material Definition Format

### Basic Material
```shader
materials/test/diffuse
{
    // Material-specific parameters
    specular 0.5 0.5 0.5 32
    receivesShadows
    
    // Traditional stage definition (compatible)
    {
        map textures/test/diffuse.tga
        rgbGen lightingDiffuse
    }
    
    // Normal map stage (new)
    {
        stage normalMap
        map textures/test/normal.tga
    }
    
    // Specular map stage (new)
    {
        stage specularMap
        map textures/test/specular.tga
    }
}
```

### Dynamic Material with Expressions
```shader
materials/test/animated
{
    // Define expression registers
    expressions
    {
        // REG_SCRATCH0 = sin(time * 2)
        scratch0 = sin(time * 2)
        
        // REG_SCRATCH1 = clamp(scratch0, 0.2, 1.0)
        scratch1 = clamp(scratch0, 0.2, 1.0)
    }
    
    {
        map textures/test/glow.tga
        // Use expression result for RGB
        rgbGen expression scratch1
        blendFunc GL_ONE GL_ONE
    }
}
```

## Performance Considerations

### Material Optimization
The material system automatically:
1. Merges compatible stages
2. Detects standard lightmap patterns
3. Precomputes static values
4. Selects optimal render paths

### Memory Management
- Materials use memory pools for efficient allocation
- Expression evaluation is cached per frame
- Static materials skip dynamic evaluation

## Debug Commands

```c
// Register in R_Register():
ri.Cmd_AddCommand("materiallist", Material_ListMaterials_f);
ri.Cmd_AddCommand("materialinfo", Material_Info_f);

// CVars for material parameters
r_materialParm0 = ri.Cvar_Get("r_materialParm0", "0", CVAR_CHEAT);
r_materialParm1 = ri.Cvar_Get("r_materialParm1", "0", CVAR_CHEAT);
r_materialParm2 = ri.Cvar_Get("r_materialParm2", "0", CVAR_CHEAT);
r_materialParm3 = ri.Cvar_Get("r_materialParm3", "0", CVAR_CHEAT);
```

## Migration Path

### Phase 1: Compatibility Mode
1. Enable material system initialization
2. Convert shaders to materials on load
3. Use shader wrappers for rendering

### Phase 2: Hybrid Mode
1. Parse new material definitions
2. Fall back to shader parsing for legacy content
3. Gradually update content to use material features

### Phase 3: Full Material System
1. Convert all shaders to materials
2. Remove legacy shader code paths
3. Utilize advanced material features

## Testing Checklist

- [ ] Material system initializes correctly
- [ ] Legacy shaders load and render properly
- [ ] New material definitions parse correctly
- [ ] Dynamic expressions evaluate properly
- [ ] Material optimization reduces stage count
- [ ] Performance meets or exceeds shader system
- [ ] Debug commands work correctly
- [ ] No memory leaks in material system

## Common Issues and Solutions

### Issue: Materials not loading
**Solution**: Ensure R_InitMaterialSystem() is called before loading materials

### Issue: Legacy shaders not working
**Solution**: Check Material_FromShader() conversion and shader wrapper creation

### Issue: Expression evaluation errors
**Solution**: Verify expression syntax and register initialization

### Issue: Performance regression
**Solution**: Enable material optimization and check for proper stage merging

## Next Steps

1. **Phase 4**: Implement advanced lighting models (PBR, IBL)
2. **Phase 5**: Add material editor UI
3. **Phase 6**: Integrate with asset pipeline

## Notes

- The material system maintains full backward compatibility
- All existing shader files continue to work unchanged
- New features are opt-in through material definitions
- Performance optimizations are automatic