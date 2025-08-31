# Phase 3: New C-based Material System

## Executive Summary

Phase 3 implements a modern material system inspired by DOOM 3 while maintaining full backward compatibility with Quake 3's .shader files. This phase replaces the existing shader_t structure with a more flexible material_t system that supports multiple rendering stages, dynamic expressions, and prepares the foundation for advanced lighting in later phases.

## Current State Analysis

### Existing Shader System

```c
// Current shader structure (tr_local.h)
typedef struct shader_s {
    char        name[MAX_QPATH];
    int         lightmapIndex;
    int         index;
    int         sortedIndex;
    float       sort;
    qboolean    defaultShader;
    qboolean    explicitlyDefined;
    int         surfaceFlags;
    int         contentFlags;
    qboolean    entityMergable;
    qboolean    isSky;
    skyParms_t  sky;
    fogParms_t  fogParms;
    float       portalRange;
    int         multitextureEnv;
    cullType_t  cullType;
    qboolean    polygonOffset;
    qboolean    noMipMaps;
    qboolean    noPicMip;
    qboolean    fogPass;
    qboolean    needsNormal;
    qboolean    needsTangent;
    qboolean    needsColor;
    int         vertexAttribs;
    int         numDeforms;
    deformStage_t deforms[MAX_SHADER_DEFORMS];
    int         numUnfoggedPasses;
    shaderStage_t *stages[MAX_SHADER_STAGES];
    void        (*optimalStageIteratorFunc)(void);
    qboolean    isStaticShader;
    float       clampTime;
    float       timeOffset;
    int         numStates;
    shader_t    *remappedShader;
    shader_t    *next;
} shader_t;

// Current stage structure
typedef struct {
    qboolean    active;
    textureBundle_t bundle[NUM_TEXTURE_BUNDLES];
    waveForm_t  rgbWave;
    colorGen_t  rgbGen;
    waveForm_t  alphaWave;
    alphaGen_t  alphaGen;
    byte        constantColor[4];
    unsigned    stateBits;
    acff_t      adjustColorsForFog;
    qboolean    isDetail;
    qboolean    isFogged;
    stageVars_t *stageVars;
} shaderStage_t;
```

### Shader File Format

```
// Example Q3 shader
textures/base_wall/metalfloor {
    {
        map textures/base_wall/metalfloor.tga
        rgbGen identity
    }
    {
        map $lightmap
        rgbGen identity
        blendFunc GL_DST_COLOR GL_ZERO
    }
}
```

## New Material System Design

### 1. Core Material Structure

```c
// File: src/engine/renderer/tr_material.h (NEW FILE)

// Material flags (compatible with shader flags)
#define MATERIAL_SKY              0x00000001
#define MATERIAL_NOMIPMAPS        0x00000002
#define MATERIAL_POLYGONOFFSET    0x00000004
#define MATERIAL_PORTAL           0x00000008
#define MATERIAL_MULTITEXTURE     0x00000010
#define MATERIAL_FOG              0x00000020
#define MATERIAL_NEEDSNORMAL      0x00000040
#define MATERIAL_NEEDSTANGENT     0x00000080
#define MATERIAL_NEEDSCOLOR       0x00000100
#define MATERIAL_AUTOSPRITE       0x00000200
#define MATERIAL_LIGHTMAP         0x00000400
#define MATERIAL_VERTEXLIT        0x00000800
#define MATERIAL_NOSHADOWS        0x00001000
#define MATERIAL_NOSELFSHADOW     0x00002000
#define MATERIAL_FORCESHADOWS     0x00004000
#define MATERIAL_RECEIVESHADOWS   0x00008000
#define MATERIAL_ALPHATEST        0x00010000
#define MATERIAL_TRANSLUCENT      0x00020000
#define MATERIAL_TWOSIDED         0x00040000

// Material stage (enhanced from shaderStage_t)
typedef struct materialStage_s {
    qboolean            active;
    
    // Textures
    image_t*            colorMap;          // Diffuse/color texture
    image_t*            normalMap;         // Normal map
    image_t*            specularMap;       // Specular map
    image_t*            glowMap;           // Emissive map
    
    // Legacy texture bundles for compatibility
    textureBundle_t     bundle[NUM_TEXTURE_BUNDLES];
    
    // Texture coordinate generation
    texCoordGen_t       tcGen;
    vec2_t              tcGenVectors[2];   // For TCGEN_VECTOR
    
    // Texture modifications
    int                 numTexMods;
    texModInfo_t        texMods[MAX_SHADER_STAGE_TEXMODS];
    
    // Color/alpha generation
    colorGen_t          rgbGen;
    waveForm_t          rgbWave;
    alphaGen_t          alphaGen;
    waveForm_t          alphaWave;
    byte                constantColor[4];
    
    // Blend state
    unsigned int        stateBits;         // GLS_* flags
    int                 blendSrc;          // GL blend source
    int                 blendDst;          // GL blend destination
    
    // Lighting mode
    stageLighting_t     lighting;          // New: lighting type
    
    // Program selection
    int                 program;           // Shader program to use
    
    // Dynamic expressions
    int                 numExpressions;
    struct expression_s *expressions;      // Dynamic value expressions
    
} materialStage_t;

// Expression system for dynamic materials
typedef struct expression_s {
    int             destRegister;      // Target register
    expOpType_t     opType;           // Operation type
    int             srcRegister[2];    // Source registers
    float           constantValue;     // Constant operand
    int             tableIndex;        // For table lookups
} expression_t;

// Expression operation types
typedef enum {
    EXP_OP_ADD,
    EXP_OP_SUBTRACT,
    EXP_OP_MULTIPLY,
    EXP_OP_DIVIDE,
    EXP_OP_MOD,
    EXP_OP_TABLE,
    EXP_OP_SIN,
    EXP_OP_COS,
    EXP_OP_SQUARE,
    EXP_OP_INVERSE,
    EXP_OP_CLAMP,
    EXP_OP_MIN,
    EXP_OP_MAX,
    EXP_OP_RANDOM
} expOpType_t;

// Stage lighting types (for future per-pixel lighting)
typedef enum {
    SL_NONE,            // No lighting
    SL_AMBIENT,         // Ambient only
    SL_DIFFUSE,         // Diffuse lighting
    SL_SPECULAR,        // Specular lighting
    SL_BUMP,            // Bump/normal mapping
} stageLighting_t;

// Main material structure
typedef struct material_s {
    char                name[MAX_QPATH];
    int                 index;             // Index in tr.materials[]
    int                 sortedIndex;       // For sorting
    
    // Surface properties
    float               sort;               // Sort order
    int                 surfaceFlags;       // SURF_* flags
    int                 contentFlags;       // CONTENTS_* flags
    int                 materialFlags;      // MATERIAL_* flags
    
    // Rendering properties
    cullType_t          cullType;          // Culling mode
    qboolean            polygonOffset;     // Z-fighting prevention
    float               polygonOffsetValue;
    
    // Stages
    int                 numStages;
    materialStage_t     stages[MAX_MATERIAL_STAGES];
    
    // Deformations
    int                 numDeforms;
    deformStage_t       deforms[MAX_SHADER_DEFORMS];
    
    // Special effects
    qboolean            isSky;
    skyParms_t          sky;
    qboolean            isFog;
    fogParms_t          fogParms;
    float               portalRange;
    
    // Lighting properties (for future phases)
    qboolean            receivesShadows;
    qboolean            castsShadows;
    qboolean            noSelfShadow;
    float               specularExponent;   // Specular power
    vec3_t              specularColor;      // Specular color
    
    // Expression registers
    float               expressionRegisters[MAX_EXPRESSION_REGISTERS];
    int                 numRegisters;
    
    // Optimization
    int                 vertexAttribs;      // Required vertex attributes
    qboolean            isStaticMaterial;   // No dynamic content
    qboolean            hasLightmap;        // Uses lightmap
    
    // Compatibility
    void                (*optimalStageIteratorFunc)(void);  // Legacy
    
    // Linked list
    struct material_s   *next;
    struct material_s   *remappedMaterial;
    
    // Reference to original shader for compatibility
    shader_t            *legacyShader;      // For backward compatibility
    
} material_t;

#define MAX_MATERIAL_STAGES     8
#define MAX_EXPRESSION_REGISTERS 16
```

### 2. Material Parser Implementation

```c
// File: src/engine/renderer/tr_material.c (NEW FILE)

// Global material table
static material_t* materialHashTable[MATERIAL_HASH_SIZE];
static material_t  materials[MAX_MATERIALS];
static int         numMaterials;

// Parse material from shader text
material_t* Material_Parse(char *name, char **text) {
    material_t  *material;
    char        *token;
    int         stage = -1;
    
    // Allocate new material
    material = &materials[numMaterials++];
    Com_Memset(material, 0, sizeof(material_t));
    
    // Set defaults
    Q_strncpyz(material->name, name, sizeof(material->name));
    material->index = numMaterials - 1;
    material->cullType = CT_FRONT_SIDED;
    material->sort = SS_OPAQUE;
    
    // Parse material body
    token = COM_ParseExt(text, qtrue);
    if (token[0] != '{') {
        ri.Printf(PRINT_WARNING, "WARNING: expecting '{', found '%s' in material '%s'\n", 
                  token, name);
        return NULL;
    }
    
    while (1) {
        token = COM_ParseExt(text, qtrue);
        
        if (!token[0]) {
            ri.Printf(PRINT_WARNING, "WARNING: unexpected end of material '%s'\n", name);
            return NULL;
        }
        
        if (token[0] == '}') {
            break;
        }
        
        // Stage definition
        if (token[0] == '{') {
            stage++;
            if (stage >= MAX_MATERIAL_STAGES) {
                ri.Printf(PRINT_WARNING, "WARNING: too many stages in material '%s'\n", name);
                return NULL;
            }
            
            Material_ParseStage(material, &material->stages[stage], text);
            material->numStages++;
        }
        // Global material keywords
        else if (!Q_stricmp(token, "cull")) {
            Material_ParseCull(material, text);
        }
        else if (!Q_stricmp(token, "surfaceparm")) {
            Material_ParseSurfaceParm(material, text);
        }
        else if (!Q_stricmp(token, "nomipmaps")) {
            material->materialFlags |= MATERIAL_NOMIPMAPS;
        }
        else if (!Q_stricmp(token, "polygonoffset")) {
            material->polygonOffset = qtrue;
        }
        else if (!Q_stricmp(token, "sort")) {
            Material_ParseSort(material, text);
        }
        else if (!Q_stricmp(token, "deformvertexes")) {
            Material_ParseDeform(material, text);
        }
        else if (!Q_stricmp(token, "skyparms")) {
            Material_ParseSkyParms(material, text);
        }
        else if (!Q_stricmp(token, "fogparms")) {
            Material_ParseFogParms(material, text);
        }
        else if (!Q_stricmp(token, "portal")) {
            material->sort = SS_PORTAL;
            material->portalRange = 256;
        }
        else if (!Q_stricmp(token, "entitymergable")) {
            material->materialFlags |= MATERIAL_ENTITYMERGABLE;
        }
        // New material properties for advanced rendering
        else if (!Q_stricmp(token, "noshadows")) {
            material->materialFlags |= MATERIAL_NOSHADOWS;
        }
        else if (!Q_stricmp(token, "noselfshadow")) {
            material->materialFlags |= MATERIAL_NOSELFSHADOW;
        }
        else if (!Q_stricmp(token, "forceshadows")) {
            material->materialFlags |= MATERIAL_FORCESHADOWS;
        }
        else if (!Q_stricmp(token, "receiveshadows")) {
            material->materialFlags |= MATERIAL_RECEIVESHADOWS;
        }
        else if (!Q_stricmp(token, "twosided")) {
            material->materialFlags |= MATERIAL_TWOSIDED;
            material->cullType = CT_TWO_SIDED;
        }
        else if (!Q_stricmp(token, "translucent")) {
            material->materialFlags |= MATERIAL_TRANSLUCENT;
            material->sort = SS_BLEND0;
        }
        else if (!Q_stricmp(token, "specular")) {
            Material_ParseSpecular(material, text);
        }
        else {
            ri.Printf(PRINT_WARNING, "WARNING: unknown material keyword '%s' in '%s'\n", 
                      token, name);
        }
    }
    
    // Post-process material
    Material_Validate(material);
    Material_OptimizeStages(material);
    Material_ComputeVertexAttribs(material);
    
    return material;
}

// Parse material stage
void Material_ParseStage(material_t *material, materialStage_t *stage, char **text) {
    char *token;
    
    // Initialize stage
    Com_Memset(stage, 0, sizeof(materialStage_t));
    stage->active = qtrue;
    stage->rgbGen = CGEN_IDENTITY;
    stage->alphaGen = AGEN_CONST;
    stage->constantColor[0] = 255;
    stage->constantColor[1] = 255;
    stage->constantColor[2] = 255;
    stage->constantColor[3] = 255;
    stage->stateBits = GLS_DEFAULT;
    
    while (1) {
        token = COM_ParseExt(text, qtrue);
        
        if (!token[0]) {
            ri.Printf(PRINT_WARNING, "WARNING: unexpected end of stage in material '%s'\n", 
                      material->name);
            break;
        }
        
        if (token[0] == '}') {
            break;
        }
        
        // Texture maps
        if (!Q_stricmp(token, "map")) {
            Material_ParseMap(material, stage, text);
        }
        else if (!Q_stricmp(token, "clampmap")) {
            Material_ParseMap(material, stage, text);
            stage->bundle[0].isClampMap = qtrue;
        }
        else if (!Q_stricmp(token, "animmap")) {
            Material_ParseAnimMap(material, stage, text);
        }
        else if (!Q_stricmp(token, "normalmap") || !Q_stricmp(token, "bumpmap")) {
            token = COM_ParseExt(text, qfalse);
            stage->normalMap = R_FindImageFile(token, IMGTYPE_NORMAL, 
                                              IMGFLAG_NONE | IMGFLAG_NORMALMAP);
            stage->lighting = SL_BUMP;
        }
        else if (!Q_stricmp(token, "specularmap")) {
            token = COM_ParseExt(text, qfalse);
            stage->specularMap = R_FindImageFile(token, IMGTYPE_COLORALPHA, IMGFLAG_NONE);
            if (stage->lighting == SL_BUMP) {
                stage->lighting = SL_SPECULAR;
            }
        }
        else if (!Q_stricmp(token, "glowmap")) {
            token = COM_ParseExt(text, qfalse);
            stage->glowMap = R_FindImageFile(token, IMGTYPE_COLORALPHA, IMGFLAG_NONE);
        }
        // Texture coordinate generation
        else if (!Q_stricmp(token, "tcgen")) {
            Material_ParseTCGen(material, stage, text);
        }
        else if (!Q_stricmp(token, "tcmod")) {
            Material_ParseTCMod(material, stage, text);
        }
        // Color generation
        else if (!Q_stricmp(token, "rgbgen")) {
            Material_ParseRGBGen(material, stage, text);
        }
        else if (!Q_stricmp(token, "alphagen")) {
            Material_ParseAlphaGen(material, stage, text);
        }
        // Blend functions
        else if (!Q_stricmp(token, "blendfunc")) {
            Material_ParseBlendFunc(material, stage, text);
        }
        else if (!Q_stricmp(token, "alphafunc")) {
            Material_ParseAlphaFunc(material, stage, text);
        }
        // Other stage properties
        else if (!Q_stricmp(token, "detail")) {
            stage->isDetail = qtrue;
        }
        else if (!Q_stricmp(token, "alphatest")) {
            token = COM_ParseExt(text, qfalse);
            float alphaTest = atof(token);
            stage->stateBits |= GLS_ALPHATEST_ENABLE;
            // Store alpha test value in stage for future use
        }
    }
}
```

### 3. Shader Compatibility Layer

```c
// File: src/engine/renderer/tr_shader_compat.c (NEW FILE)

// Convert existing shader_t to material_t
material_t* Material_FromShader(shader_t *shader) {
    material_t *material;
    
    // Check if already converted
    if (shader->convertedMaterial) {
        return shader->convertedMaterial;
    }
    
    // Allocate new material
    material = &materials[numMaterials++];
    Com_Memset(material, 0, sizeof(material_t));
    
    // Copy basic properties
    Q_strncpyz(material->name, shader->name, sizeof(material->name));
    material->index = material - materials;
    material->sort = shader->sort;
    material->surfaceFlags = shader->surfaceFlags;
    material->contentFlags = shader->contentFlags;
    material->cullType = shader->cullType;
    material->polygonOffset = shader->polygonOffset;
    
    // Copy flags
    if (shader->noMipMaps) material->materialFlags |= MATERIAL_NOMIPMAPS;
    if (shader->isSky) {
        material->isSky = qtrue;
        material->sky = shader->sky;
    }
    if (shader->fogPass) {
        material->isFog = qtrue;
        material->fogParms = shader->fogParms;
    }
    
    // Convert stages
    material->numStages = shader->numUnfoggedPasses;
    for (int i = 0; i < shader->numUnfoggedPasses; i++) {
        Material_ConvertStage(&material->stages[i], shader->stages[i]);
    }
    
    // Copy deforms
    material->numDeforms = shader->numDeforms;
    Com_Memcpy(material->deforms, shader->deforms, 
               sizeof(deformStage_t) * shader->numDeforms);
    
    // Compute attributes
    material->vertexAttribs = shader->vertexAttribs;
    
    // Link back
    material->legacyShader = shader;
    shader->convertedMaterial = material;
    
    return material;
}

// Convert shader stage to material stage
void Material_ConvertStage(materialStage_t *mStage, shaderStage_t *sStage) {
    mStage->active = sStage->active;
    
    // Copy texture bundles
    Com_Memcpy(mStage->bundle, sStage->bundle, sizeof(textureBundle_t) * NUM_TEXTURE_BUNDLES);
    
    // Set primary texture
    if (sStage->bundle[0].image[0]) {
        mStage->colorMap = sStage->bundle[0].image[0];
    }
    
    // Copy generation functions
    mStage->rgbGen = sStage->rgbGen;
    mStage->rgbWave = sStage->rgbWave;
    mStage->alphaGen = sStage->alphaGen;
    mStage->alphaWave = sStage->alphaWave;
    Com_Memcpy(mStage->constantColor, sStage->constantColor, 4);
    
    // Copy state bits
    mStage->stateBits = sStage->stateBits;
    
    // Determine blend functions from state bits
    Material_ExtractBlendFunc(mStage);
    
    // Determine lighting mode
    if (sStage->bundle[0].isLightmap) {
        mStage->lighting = SL_DIFFUSE;
    } else {
        mStage->lighting = SL_NONE;
    }
}
```

### 4. Material System Integration

```c
// File: src/engine/renderer/tr_shader.c (MODIFY)

// Modified shader loading to use material system
shader_t *R_FindShader(const char *name, int lightmapIndex, qboolean mipRawImage) {
    char        strippedName[MAX_QPATH];
    shader_t    *sh;
    material_t  *mat;
    
    // ... existing validation code ...
    
    // Try to find as material first
    mat = Material_Find(strippedName);
    if (mat) {
        // Create compatibility shader wrapper
        sh = Material_CreateShaderWrapper(mat);
        return sh;
    }
    
    // Parse as material
    char *shaderText = NULL;
    shaderText = FindShaderInShaderText(strippedName);
    
    if (shaderText) {
        mat = Material_Parse(strippedName, &shaderText);
        
        // Create backward-compatible shader
        sh = Material_CreateShaderWrapper(mat);
    } else {
        // Create default material
        mat = Material_CreateDefault(strippedName, lightmapIndex);
        sh = Material_CreateShaderWrapper(mat);
    }
    
    return sh;
}

// Create shader wrapper for material
shader_t* Material_CreateShaderWrapper(material_t *material) {
    shader_t *shader;
    
    // Allocate shader
    shader = ri.Hunk_Alloc(sizeof(shader_t), h_low);
    Com_Memset(shader, 0, sizeof(shader_t));
    
    // Copy properties from material
    Q_strncpyz(shader->name, material->name, sizeof(shader->name));
    shader->index = tr.numShaders;
    shader->sort = material->sort;
    shader->surfaceFlags = material->surfaceFlags;
    shader->contentFlags = material->contentFlags;
    shader->cullType = material->cullType;
    shader->polygonOffset = material->polygonOffset;
    shader->vertexAttribs = material->vertexAttribs;
    
    // Link to material
    shader->material = material;
    material->legacyShader = shader;
    
    // Convert stages for compatibility
    shader->numUnfoggedPasses = material->numStages;
    for (int i = 0; i < material->numStages; i++) {
        shader->stages[i] = Material_CreateStageWrapper(&material->stages[i]);
    }
    
    // Add to shader list
    tr.shaders[tr.numShaders] = shader;
    shader->index = tr.numShaders;
    tr.numShaders++;
    
    return shader;
}
```

### 5. Expression System Implementation

```c
// File: src/engine/renderer/tr_expression.c (NEW FILE)

// Expression evaluation for dynamic materials
void Material_EvaluateExpressions(material_t *material, float time) {
    float *regs = material->expressionRegisters;
    
    // Set time register
    regs[REG_TIME] = time;
    regs[REG_PARM0] = r_materialParm0->value;
    regs[REG_PARM1] = r_materialParm1->value;
    
    // Evaluate all expressions
    for (int i = 0; i < material->numExpressions; i++) {
        expression_t *exp = &material->expressions[i];
        float result = 0;
        
        switch (exp->opType) {
        case EXP_OP_ADD:
            result = regs[exp->srcRegister[0]] + regs[exp->srcRegister[1]];
            break;
            
        case EXP_OP_MULTIPLY:
            result = regs[exp->srcRegister[0]] * regs[exp->srcRegister[1]];
            break;
            
        case EXP_OP_SIN:
            result = sin(regs[exp->srcRegister[0]]);
            break;
            
        case EXP_OP_TABLE:
            result = Material_TableLookup(exp->tableIndex, regs[exp->srcRegister[0]]);
            break;
            
        case EXP_OP_CLAMP:
            result = regs[exp->srcRegister[0]];
            if (result < 0) result = 0;
            if (result > 1) result = 1;
            break;
            
        // ... other operations ...
        }
        
        regs[exp->destRegister] = result;
    }
}

// Parse expression from material
void Material_ParseExpression(material_t *material, char **text) {
    char *token;
    expression_t exp;
    
    // Parse destination register
    token = COM_ParseExt(text, qfalse);
    exp.destRegister = Material_ParseRegister(token);
    
    // Parse operation
    token = COM_ParseExt(text, qfalse);
    exp.opType = Material_ParseOperation(token);
    
    // Parse operands
    token = COM_ParseExt(text, qfalse);
    exp.srcRegister[0] = Material_ParseRegister(token);
    
    if (Material_OperationNeedsSecondOperand(exp.opType)) {
        token = COM_ParseExt(text, qfalse);
        exp.srcRegister[1] = Material_ParseRegister(token);
    }
    
    // Add to material
    if (material->numExpressions < MAX_EXPRESSIONS) {
        material->expressions[material->numExpressions++] = exp;
    }
}
```

### 6. Optimized Stage Processing

```c
// File: src/engine/renderer/tr_material_opt.c (NEW FILE)

// Optimize material stages
void Material_OptimizeStages(material_t *material) {
    // Merge compatible stages
    Material_MergeStages(material);
    
    // Detect common patterns
    Material_DetectLightmapPattern(material);
    
    // Precompute static values
    Material_PrecomputeStatic(material);
    
    // Select optimal rendering path
    Material_SelectRenderPath(material);
}

// Merge lightmap stages
void Material_DetectLightmapPattern(material_t *material) {
    if (material->numStages != 2)
        return;
    
    materialStage_t *stage0 = &material->stages[0];
    materialStage_t *stage1 = &material->stages[1];
    
    // Check for standard lightmap pattern
    if (stage0->bundle[0].image[0] && 
        stage1->bundle[0].isLightmap &&
        stage1->stateBits == (GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO)) {
        
        // Mark as lightmapped material
        material->hasLightmap = qtrue;
        material->materialFlags |= MATERIAL_LIGHTMAP;
        
        // Can use optimized lightmap path
        material->optimalStageIteratorFunc = RB_StageIteratorLightmappedMultitexture;
    }
}

// Precompute static expression values
void Material_PrecomputeStatic(material_t *material) {
    // Check if material has only static content
    qboolean isStatic = qtrue;
    
    for (int i = 0; i < material->numStages; i++) {
        materialStage_t *stage = &material->stages[i];
        
        // Check for dynamic content
        if (stage->rgbGen != CGEN_IDENTITY && stage->rgbGen != CGEN_CONST) {
            isStatic = qfalse;
            break;
        }
        
        if (stage->alphaGen != AGEN_IDENTITY && stage->alphaGen != AGEN_CONST) {
            isStatic = qfalse;
            break;
        }
        
        if (stage->numTexMods > 0) {
            isStatic = qfalse;
            break;
        }
        
        if (stage->bundle[0].numImageAnimations > 1) {
            isStatic = qfalse;
            break;
        }
    }
    
    material->isStaticMaterial = isStatic;
    
    // Precompute vertex attributes
    Material_ComputeVertexAttribs(material);
}
```

### 7. Backend Integration

```c
// File: src/engine/renderer/tr_backend.c (MODIFY)

// Modified surface setup for materials
void RB_BeginSurface(shader_t *shader, int fogNum) {
    material_t *material;
    
    // Get material from shader
    if (shader->material) {
        material = shader->material;
    } else {
        // Legacy path - convert on the fly
        material = Material_FromShader(shader);
    }
    
    // Evaluate dynamic expressions
    if (!material->isStaticMaterial) {
        Material_EvaluateExpressions(material, backEnd.refdef.floatTime);
    }
    
    // Setup material state
    tess.numIndexes = 0;
    tess.numVertexes = 0;
    tess.shader = shader;
    tess.material = material;
    tess.fogNum = fogNum;
    tess.dlightBits = 0;
    tess.xstages = material->stages;  // Use material stages
    tess.numPasses = material->numStages;
    
    // Setup vertex attributes based on material requirements
    RB_SetupVertexAttributes(material->vertexAttribs);
}

// Process material stages
void RB_IterateMaterialStages(material_t *material) {
    int stage;
    
    // Iterate through material stages
    for (stage = 0; stage < material->numStages; stage++) {
        materialStage_t *pStage = &material->stages[stage];
        
        if (!pStage->active) {
            continue;
        }
        
        // Setup stage state
        RB_SetupMaterialStage(pStage);
        
        // Compute texture coordinates
        RB_CalcTexCoords(pStage);
        
        // Compute colors
        RB_CalcColors(pStage);
        
        // Select and use appropriate shader program
        if (pStage->program) {
            RB_UseProgram(pStage->program);
        } else {
            RB_SelectProgram(pStage);
        }
        
        // Draw geometry
        RB_DrawElements();
    }
}
```

## Memory Management

### Material Memory Allocation

```c
// Static allocation for materials
#define MAX_MATERIALS 4096
#define MATERIAL_HASH_SIZE 1024

static material_t materials[MAX_MATERIALS];
static int numMaterials;

// Hash table for fast lookup
static material_t* materialHashTable[MATERIAL_HASH_SIZE];

// Memory pools for dynamic data
typedef struct {
    byte    *base;
    int     size;
    int     used;
} materialMemPool_t;

static materialMemPool_t expressionPool;   // For expressions
static materialMemPool_t stringPool;       // For names

void R_InitMaterialSystem(void) {
    // Clear material array
    Com_Memset(materials, 0, sizeof(materials));
    numMaterials = 0;
    
    // Clear hash table
    Com_Memset(materialHashTable, 0, sizeof(materialHashTable));
    
    // Allocate memory pools
    expressionPool.size = 256 * 1024;  // 256KB for expressions
    expressionPool.base = ri.Hunk_Alloc(expressionPool.size, h_low);
    expressionPool.used = 0;
    
    stringPool.size = 128 * 1024;      // 128KB for strings
    stringPool.base = ri.Hunk_Alloc(stringPool.size, h_low);
    stringPool.used = 0;
    
    // Register material commands
    Material_RegisterCommands();
}
```

## Testing and Validation

### Material Validation

```c
void Material_Validate(material_t *material) {
    // Check for common errors
    if (material->numStages == 0) {
        ri.Printf(PRINT_WARNING, "WARNING: material '%s' has no stages\n", material->name);
        Material_CreateDefaultStage(material);
    }
    
    // Validate each stage
    for (int i = 0; i < material->numStages; i++) {
        materialStage_t *stage = &material->stages[i];
        
        // Ensure at least one texture
        if (!stage->colorMap && !stage->bundle[0].image[0]) {
            ri.Printf(PRINT_WARNING, "WARNING: material '%s' stage %d has no texture\n", 
                      material->name, i);
        }
        
        // Check blend function validity
        Material_ValidateBlendFunc(stage);
        
        // Validate texture coordinates
        Material_ValidateTCGen(stage);
    }
    
    // Check sort order
    if (material->sort < SS_BAD || material->sort > SS_MAX) {
        ri.Printf(PRINT_WARNING, "WARNING: material '%s' has invalid sort value %f\n", 
                  material->name, material->sort);
        material->sort = SS_OPAQUE;
    }
}
```

### Debug Commands

```c
// Debug console commands
void Material_ListMaterials_f(void) {
    ri.Printf(PRINT_ALL, "--- Materials ---\n");
    ri.Printf(PRINT_ALL, "Num  Name                    Stages  Flags     Sort\n");
    
    for (int i = 0; i < numMaterials; i++) {
        material_t *mat = &materials[i];
        ri.Printf(PRINT_ALL, "%3d: %-24s %d     %08x  %.2f\n",
                  i, mat->name, mat->numStages, mat->materialFlags, mat->sort);
    }
    
    ri.Printf(PRINT_ALL, "%d materials total\n", numMaterials);
}

void Material_Info_f(void) {
    if (ri.Cmd_Argc() != 2) {
        ri.Printf(PRINT_ALL, "usage: materialinfo <materialname>\n");
        return;
    }
    
    const char *name = ri.Cmd_Argv(1);
    material_t *mat = Material_Find(name);
    
    if (!mat) {
        ri.Printf(PRINT_ALL, "Material '%s' not found\n", name);
        return;
    }
    
    ri.Printf(PRINT_ALL, "=== Material: %s ===\n", mat->name);
    ri.Printf(PRINT_ALL, "Index: %d\n", mat->index);
    ri.Printf(PRINT_ALL, "Sort: %.2f\n", mat->sort);
    ri.Printf(PRINT_ALL, "Stages: %d\n", mat->numStages);
    ri.Printf(PRINT_ALL, "Flags: %08x\n", mat->materialFlags);
    ri.Printf(PRINT_ALL, "Cull: %s\n", 
              mat->cullType == CT_TWO_SIDED ? "none" :
              mat->cullType == CT_BACK_SIDED ? "back" : "front");
    
    // Print stage info
    for (int i = 0; i < mat->numStages; i++) {
        materialStage_t *stage = &mat->stages[i];
        ri.Printf(PRINT_ALL, "  Stage %d:\n", i);
        ri.Printf(PRINT_ALL, "    RGB Gen: %d\n", stage->rgbGen);
        ri.Printf(PRINT_ALL, "    Alpha Gen: %d\n", stage->alphaGen);
        ri.Printf(PRINT_ALL, "    State Bits: %08x\n", stage->stateBits);
    }
}
```

## Backward Compatibility

### Shader System Wrapper

```c
// Maintain shader_t interface for compatibility
typedef struct shader_s {
    // ... existing shader fields ...
    
    // New: link to material
    material_t  *material;
    
    // New: converted material flag
    qboolean    isMaterialBased;
} shader_t;

// Wrapper functions
shader_t* R_GetShaderByHandle(qhandle_t hShader) {
    if (hShader < 0 || hShader >= tr.numShaders) {
        return tr.defaultShader;
    }
    
    shader_t *shader = tr.shaders[hShader];
    
    // Ensure material exists
    if (!shader->material && !shader->isMaterialBased) {
        shader->material = Material_FromShader(shader);
        shader->isMaterialBased = qtrue;
    }
    
    return shader;
}
```

## Performance Considerations

### Optimization Strategies

1. **Stage Merging**: Combine compatible stages to reduce state changes
2. **Static Precomputation**: Precompute values for static materials
3. **Expression Caching**: Cache expression results when possible
4. **Batch Compatibility**: Group materials by state to improve batching

### Performance Metrics

```c
typedef struct {
    int     numMaterialChanges;
    int     numStageChanges;
    int     numExpressionEvals;
    double  materialSetupTime;
    double  expressionEvalTime;
} materialPerfStats_t;

void Material_GatherStats(materialPerfStats_t *stats) {
    stats->numMaterialChanges = backEnd.pc.c_shaderChanges;
    stats->numStageChanges = backEnd.pc.c_totalStages;
    // ... gather other statistics ...
}
```

## Success Criteria

Phase 3 is complete when:

1. ✓ Material system fully implemented
2. ✓ All Q3 shaders parse correctly
3. ✓ Expression system functional
4. ✓ No visual regressions
5. ✓ Backward compatibility maintained
6. ✓ Debug tools implemented
7. ✓ Performance equal or better
8. ✓ Documentation complete

## Dependencies for Next Phases

### Phase 4 (Vulkan Backend) Requirements
- Material stages for pipeline creation
- State bits for render state setup

### Phase 5 (Dynamic Lighting) Requirements
- Normal/specular map support
- Lighting mode per stage

### Phase 6 (Additive Lighting) Requirements
- Material lighting properties
- Multi-pass rendering support

This material system provides the foundation for advanced rendering features while maintaining full compatibility with existing Quake 3 content.