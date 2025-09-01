/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Material Override System Console Commands
===========================================================================
*/

#include "../tr_local.h"
#include "tr_material.h"
#include "tr_material_override.h"

// External state
extern materialOverrideSystem_t matOverrideSystem;
extern cvar_t *r_materialOverride;
extern cvar_t *r_materialAutoGen;
extern cvar_t *r_materialExport;
extern cvar_t *r_materialGenQuality;
extern cvar_t *r_materialDebug;

/*
================
MatOver_RegenerateAll_f

Console command to regenerate all material overrides
================
*/
void MatOver_RegenerateAll_f(void) {
    int count = 0;
    
    if (!r_materialOverride->integer) {
        ri.Printf(PRINT_ALL, "Material override system is disabled\n");
        return;
    }
    
    ri.Printf(PRINT_ALL, "Regenerating all material overrides...\n");
    
    // Clear existing overrides
    for (int i = 0; i < matOverrideSystem.numOverrides; i++) {
        if (matOverrideSystem.overrides[i]) {
            // Mark for regeneration
            matOverrideSystem.overrides[i]->needsGeneration = qtrue;
            count++;
        }
    }
    
    ri.Printf(PRINT_ALL, "Marked %d materials for regeneration\n", count);
}

/*
================
MatOver_ExportAll_f

Console command to export all generated materials
================
*/
void MatOver_ExportAll_f(void) {
    int count = 0;
    char basePath[MAX_QPATH];
    
    if (!r_materialOverride->integer) {
        ri.Printf(PRINT_ALL, "Material override system is disabled\n");
        return;
    }
    
    Com_sprintf(basePath, sizeof(basePath), "materials/export");
    
    ri.Printf(PRINT_ALL, "Exporting all material overrides to %s...\n", basePath);
    
    for (int i = 0; i < matOverrideSystem.numOverrides; i++) {
        materialOverride_t *override = matOverrideSystem.overrides[i];
        if (override && override->generated) {
            char path[MAX_QPATH];
            Com_sprintf(path, sizeof(path), "%s/%s", basePath, override->name);
            
            if (MatOver_ExportMaps(override, path)) {
                count++;
            }
        }
    }
    
    ri.Printf(PRINT_ALL, "Exported %d material overrides\n", count);
}

/*
================
MatOver_ClearCache_f

Console command to clear material cache
================
*/
void MatOver_ClearCache_f(void) {
    ri.Printf(PRINT_ALL, "Clearing material override cache...\n");
    
    // Free all cached data
    for (int i = 0; i < matOverrideSystem.numOverrides; i++) {
        if (matOverrideSystem.overrides[i]) {
            for (int j = 0; j < MATMAP_COUNT; j++) {
                if (matOverrideSystem.overrides[i]->maps[j]) {
                    // Free map data
                    matOverrideSystem.overrides[i]->maps[j] = NULL;
                }
            }
        }
    }
    
    // Reset cache
    matOverrideSystem.cacheUsed = 0;
    
    ri.Printf(PRINT_ALL, "Material cache cleared\n");
}

/*
================
MatOver_Status_f

Console command to show material override system status
================
*/
void MatOver_Status_f(void) {
    int generated = 0;
    int pending = 0;
    int cached = 0;
    
    ri.Printf(PRINT_ALL, "\n==== Material Override System Status ====\n");
    ri.Printf(PRINT_ALL, "System: %s\n", r_materialOverride->integer ? "Enabled" : "Disabled");
    ri.Printf(PRINT_ALL, "Auto-generation: %s\n", r_materialAutoGen->integer ? "Enabled" : "Disabled");
    ri.Printf(PRINT_ALL, "Export: %s\n", r_materialExport->integer ? "Enabled" : "Disabled");
    ri.Printf(PRINT_ALL, "Quality: %d\n", r_materialGenQuality->integer);
    
    // Count overrides
    for (int i = 0; i < matOverrideSystem.numOverrides; i++) {
        materialOverride_t *override = matOverrideSystem.overrides[i];
        if (override) {
            if (override->generated) {
                generated++;
            }
            if (override->needsGeneration) {
                pending++;
            }
            if (override->maps[0]) {  // Has at least one map cached
                cached++;
            }
        }
    }
    
    ri.Printf(PRINT_ALL, "\nOverrides:\n");
    ri.Printf(PRINT_ALL, "  Total: %d\n", matOverrideSystem.numOverrides);
    ri.Printf(PRINT_ALL, "  Generated: %d\n", generated);
    ri.Printf(PRINT_ALL, "  Pending: %d\n", pending);
    ri.Printf(PRINT_ALL, "  Cached: %d\n", cached);
    
    ri.Printf(PRINT_ALL, "\nCache:\n");
    ri.Printf(PRINT_ALL, "  Size: %d MB\n", matOverrideSystem.cacheSize / (1024 * 1024));
    ri.Printf(PRINT_ALL, "  Used: %d MB (%.1f%%)\n", 
              matOverrideSystem.cacheUsed / (1024 * 1024),
              (float)matOverrideSystem.cacheUsed / (float)matOverrideSystem.cacheSize * 100.0f);
    
    ri.Printf(PRINT_ALL, "\nGeneration Parameters:\n");
    ri.Printf(PRINT_ALL, "  Sobel Strength: %.2f\n", matOverrideSystem.defaultParams.sobelStrength);
    ri.Printf(PRINT_ALL, "  Luminance Weight: %.2f\n", matOverrideSystem.defaultParams.luminanceWeight);
    ri.Printf(PRINT_ALL, "  Frequency Scale: %.2f\n", matOverrideSystem.defaultParams.frequencyScale);
    ri.Printf(PRINT_ALL, "  Chromatic Threshold: %.2f\n", matOverrideSystem.defaultParams.chromaticThreshold);
    
    ri.Printf(PRINT_ALL, "==========================================\n");
}

/*
================
MatOver_ListOverrides_f

List all material overrides (debug)
================
*/
void MatOver_ListOverrides_f(void) {
    if (!r_materialDebug->integer) {
        ri.Printf(PRINT_ALL, "Set r_materialDebug 1 to use this command\n");
        return;
    }
    
    ri.Printf(PRINT_ALL, "\n==== Material Overrides ====\n");
    
    for (int i = 0; i < matOverrideSystem.numOverrides; i++) {
        materialOverride_t *override = matOverrideSystem.overrides[i];
        if (override) {
            ri.Printf(PRINT_ALL, "%3d: %-32s %s %s\n",
                      i,
                      override->name,
                      override->generated ? "[GEN]" : "[   ]",
                      override->needsGeneration ? "[PENDING]" : "");
        }
    }
    
    ri.Printf(PRINT_ALL, "Total: %d overrides\n", matOverrideSystem.numOverrides);
}