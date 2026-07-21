/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "../core/tr_local.h"
#include "tr_light.h"
#include "tr_light_dynamic.h"

extern cvar_t *r_showLightVolumes;

// External references
extern lightSystem_t tr_lightSystem;

// Preferred grid cell size in world units; grows on huge (space) maps so
// the cell count stays bounded
static const float LIGHT_GRID_CELL_SIZE = 64.0f;
#define LIGHT_GRID_MAX_DIM 96

// Directional lights affect every cell, so they are kept out of the grid
// entirely and appended to query results instead
#define LIGHT_GRID_MAX_DIRECTIONAL 8

// Light grid for spatial queries
static lightGrid_t *s_lightGrid = NULL;
static renderLight_t *s_gridDirectional[LIGHT_GRID_MAX_DIRECTIONAL];
static int s_gridNumDirectional = 0;

// Rebuild tracking: the grid is rebuilt only when the light population or
// the world changes, never per query
static int s_gridBuiltLightCount = -1;
static const world_t *s_gridBuiltWorld = NULL;

void R_DebugBounds(vec3_t mins, vec3_t maxs, vec4_t color);

/*
===============
R_InitLightGrid

Initialize grid bounds and allocate the (zeroed) cell table. Cell size
adapts to the world so huge open maps (space maps) cannot explode the cell
count: each axis is capped at LIGHT_GRID_MAX_DIM cells.
===============
*/
static void R_InitLightGrid(lightGrid_t *grid, vec3_t mins, vec3_t maxs, float cellSize) {
    int i;

    if (!grid) {
        return;
    }

    VectorCopy(mins, grid->mins);
    VectorCopy(maxs, grid->maxs);

    for (i = 0; i < 3; i++) {
        float extent = grid->maxs[i] - grid->mins[i];
        float axisCell = cellSize;
        if (extent > cellSize * LIGHT_GRID_MAX_DIM) {
            axisCell = extent / LIGHT_GRID_MAX_DIM;
        }
        grid->cellSize[i] = axisCell;
        grid->gridSize[i] = (int)ceil(extent / axisCell);
        if (grid->gridSize[i] < 1) {
            grid->gridSize[i] = 1;
        }
        if (grid->gridSize[i] > LIGHT_GRID_MAX_DIM) {
            grid->gridSize[i] = LIGHT_GRID_MAX_DIM;
        }
    }

    int totalCells = grid->gridSize[0] * grid->gridSize[1] * grid->gridSize[2];
    grid->cells = ri.Malloc(sizeof(renderLight_t **) * totalCells);
    Com_Memset(grid->cells, 0, sizeof(renderLight_t **) * totalCells);
}

/*
===============
R_LightAffectsCell
===============
*/
static qboolean R_LightAffectsCell(const renderLight_t *light,
                                   const vec3_t cellMin, const vec3_t cellMax) {
    if (light->type == RL_OMNI) {
        vec3_t closest, delta;
        for (int axis = 0; axis < 3; axis++) {
            if (light->origin[axis] < cellMin[axis]) {
                closest[axis] = cellMin[axis];
            } else if (light->origin[axis] > cellMax[axis]) {
                closest[axis] = cellMax[axis];
            } else {
                closest[axis] = light->origin[axis];
            }
        }
        VectorSubtract(closest, light->origin, delta);
        return VectorLength(delta) <= light->radius ? qtrue : qfalse;
    }

    // AABB overlap for other local light types
    if (cellMin[0] > light->maxs[0] || cellMax[0] < light->mins[0] ||
        cellMin[1] > light->maxs[1] || cellMax[1] < light->mins[1] ||
        cellMin[2] > light->maxs[2] || cellMax[2] < light->mins[2]) {
        return qfalse;
    }
    return qtrue;
}

/*
===============
R_BuildLightGrid

Build the spatial grid for fast light lookups. Two passes over the local
lights: count per cell, then fill exact-size lists — one allocation per
occupied cell, all heap-owned and freed on the next rebuild. Rebuilds run
only when the active light population or the world changes.
===============
*/
void R_BuildLightGrid(void) {
    int i, j, k, l;

    if (s_lightGrid &&
        s_gridBuiltLightCount == tr_lightSystem.numActiveLights &&
        s_gridBuiltWorld == tr.world) {
        return;
    }

    if (!s_lightGrid) {
        // Heap-allocated, not Hunk: the Hunk is wiped on every map load, so a
        // Hunk-owned grid left this static pointer dangling into reused memory
        // after a level change (R_ClearLightGrid then read a garbage cell table
        // and crashed). The per-cell lists and the cell table are already heap
        // (ri.Malloc), so the whole structure now shares one lifetime and
        // survives map transitions; R_ShutdownLightGrid frees it on teardown.
        s_lightGrid = ri.Malloc(sizeof(lightGrid_t));
        Com_Memset(s_lightGrid, 0, sizeof(lightGrid_t));
    }

    // Release the previous build entirely
    R_ClearLightGrid();

    vec3_t worldMins, worldMaxs;
    if (tr.world) {
        VectorCopy(tr.world->nodes[0].mins, worldMins);
        VectorCopy(tr.world->nodes[0].maxs, worldMaxs);
    } else {
        VectorSet(worldMins, -65536, -65536, -65536);
        VectorSet(worldMaxs, 65536, 65536, 65536);
    }

    R_InitLightGrid(s_lightGrid, worldMins, worldMaxs, LIGHT_GRID_CELL_SIZE);

    const int dimX = s_lightGrid->gridSize[0];
    const int dimY = s_lightGrid->gridSize[1];
    const int dimZ = s_lightGrid->gridSize[2];
    const int totalCells = dimX * dimY * dimZ;

    int *counts = ri.Hunk_AllocateTempMemory(sizeof(int) * totalCells);
    Com_Memset(counts, 0, sizeof(int) * totalCells);

    s_gridNumDirectional = 0;

    // Pass 1: directional split + per-cell counts; pass 2: fill
    for (int pass = 0; pass < 2; pass++) {
        for (l = 0; l < tr_lightSystem.numActiveLights; l++) {
            renderLight_t *light = tr_lightSystem.activeLights[l];
            if (!light) {
                continue;
            }

            if (light->type == RL_DIRECTIONAL) {
                if (pass == 0 && s_gridNumDirectional < LIGHT_GRID_MAX_DIRECTIONAL) {
                    s_gridDirectional[s_gridNumDirectional++] = light;
                }
                continue;
            }

            int minCell[3], maxCell[3];
            for (i = 0; i < 3; i++) {
                minCell[i] = (int)floor((light->mins[i] - s_lightGrid->mins[i]) / s_lightGrid->cellSize[i]);
                maxCell[i] = (int)ceil((light->maxs[i] - s_lightGrid->mins[i]) / s_lightGrid->cellSize[i]);
                if (minCell[i] < 0) minCell[i] = 0;
                if (maxCell[i] >= s_lightGrid->gridSize[i]) maxCell[i] = s_lightGrid->gridSize[i] - 1;
            }

            for (i = minCell[0]; i <= maxCell[0]; i++) {
                for (j = minCell[1]; j <= maxCell[1]; j++) {
                    for (k = minCell[2]; k <= maxCell[2]; k++) {
                        vec3_t cellMin, cellMax;
                        cellMin[0] = s_lightGrid->mins[0] + i * s_lightGrid->cellSize[0];
                        cellMin[1] = s_lightGrid->mins[1] + j * s_lightGrid->cellSize[1];
                        cellMin[2] = s_lightGrid->mins[2] + k * s_lightGrid->cellSize[2];
                        cellMax[0] = cellMin[0] + s_lightGrid->cellSize[0];
                        cellMax[1] = cellMin[1] + s_lightGrid->cellSize[1];
                        cellMax[2] = cellMin[2] + s_lightGrid->cellSize[2];

                        if (!R_LightAffectsCell(light, cellMin, cellMax)) {
                            continue;
                        }

                        int cellIndex = i + j * dimX + k * dimX * dimY;
                        if (pass == 0) {
                            counts[cellIndex]++;
                        } else {
                            renderLight_t **list = s_lightGrid->cells[cellIndex];
                            int n = 0;
                            while (list[n]) {
                                n++;
                            }
                            list[n] = light;
                        }
                    }
                }
            }
        }

        if (pass == 0) {
            // Allocate exact-size, null-terminated lists for occupied cells
            for (i = 0; i < totalCells; i++) {
                if (counts[i] > 0) {
                    s_lightGrid->cells[i] = ri.Malloc(sizeof(renderLight_t *) * (counts[i] + 1));
                    Com_Memset(s_lightGrid->cells[i], 0, sizeof(renderLight_t *) * (counts[i] + 1));
                }
            }
        }
    }

    ri.Hunk_FreeTempMemory(counts);

    s_gridBuiltLightCount = tr_lightSystem.numActiveLights;
    s_gridBuiltWorld = tr.world;
}

/*
===============
R_ClearLightGrid

Free every cell list and the cell table from the previous build.
===============
*/
void R_ClearLightGrid(void) {
    int i;

    if (!s_lightGrid) {
        return;
    }

    if (s_lightGrid->cells) {
        int totalCells = s_lightGrid->gridSize[0] * s_lightGrid->gridSize[1] * s_lightGrid->gridSize[2];
        for (i = 0; i < totalCells; i++) {
            if (s_lightGrid->cells[i]) {
                ri.Free(s_lightGrid->cells[i]);
                s_lightGrid->cells[i] = NULL;
            }
        }
        ri.Free(s_lightGrid->cells);
        s_lightGrid->cells = NULL;
    }

    s_gridNumDirectional = 0;
    s_gridBuiltLightCount = -1;
    s_gridBuiltWorld = NULL;
}

/*
===============
R_ShutdownLightGrid

Release the grid entirely on renderer shutdown. RE_Shutdown's ri.FreeAll()
reclaims the underlying heap, so the static pointer MUST be nulled here or the
next R_Init would rebuild against a dangling address.
===============
*/
void R_ShutdownLightGrid(void) {
    R_ClearLightGrid();
    if (s_lightGrid) {
        ri.Free(s_lightGrid);
        s_lightGrid = NULL;
    }
}

/*
===============
R_GetLightsInCell

Get lights affecting a grid cell (already implemented in tr_light_cull.c)
This is an alternative implementation using the grid
===============
*/
renderLight_t** R_GetLightsInGridCell(int x, int y, int z) {
    static renderLight_t *emptyList[1] = { NULL };
    
    if (!s_lightGrid || !s_lightGrid->cells) {
        return emptyList;
    }
    
    // Bounds check
    if (x < 0 || x >= s_lightGrid->gridSize[0] ||
        y < 0 || y >= s_lightGrid->gridSize[1] ||
        z < 0 || z >= s_lightGrid->gridSize[2]) {
        return emptyList;
    }
    
    int cellIndex = x + y * s_lightGrid->gridSize[0] + 
                   z * s_lightGrid->gridSize[0] * s_lightGrid->gridSize[1];
    
    if (s_lightGrid->cells[cellIndex]) {
        return s_lightGrid->cells[cellIndex];
    }
    
    return emptyList;
}

/*
===============
R_GetLightsAtPoint

Get lights affecting a world position using the grid
===============
*/
renderLight_t** R_GetLightsAtPoint(vec3_t point) {
    int cellCoord[3];
    int i;
    
    if (!s_lightGrid) {
        R_BuildLightGrid();
        if (!s_lightGrid) {
            static renderLight_t *emptyList[1] = { NULL };
            return emptyList;
        }
    }
    
    // Calculate cell coordinates
    for (i = 0; i < 3; i++) {
        cellCoord[i] = (int)floor((point[i] - s_lightGrid->mins[i]) / s_lightGrid->cellSize[i]);
    }
    
    return R_GetLightsInGridCell(cellCoord[0], cellCoord[1], cellCoord[2]);
}

/*
===============
R_GetNearbyLights

Get lights from neighboring cells for smooth transitions
===============
*/
int R_GetNearbyLights(vec3_t point, renderLight_t **lightList, int maxLights) {
    int cellCoord[3];
    int i, j, k;
    int numLights = 0;
    
    if (!s_lightGrid) {
        return 0;
    }
    
    // Calculate cell coordinates
    for (i = 0; i < 3; i++) {
        cellCoord[i] = (int)floor((point[i] - s_lightGrid->mins[i]) / s_lightGrid->cellSize[i]);
    }
    
    // Directional lights are global (kept out of the cells) — they always
    // affect the query point
    for (i = 0; i < s_gridNumDirectional && numLights < maxLights; i++) {
        lightList[numLights++] = s_gridDirectional[i];
    }

    // Check 3x3x3 neighborhood
    for (i = -1; i <= 1 && numLights < maxLights; i++) {
        for (j = -1; j <= 1 && numLights < maxLights; j++) {
            for (k = -1; k <= 1 && numLights < maxLights; k++) {
                renderLight_t **cellLights = R_GetLightsInGridCell(
                    cellCoord[0] + i,
                    cellCoord[1] + j,
                    cellCoord[2] + k
                );

                // Add unique lights to list
                for (int l = 0; cellLights[l] && numLights < maxLights; l++) {
                    qboolean found = qfalse;

                    // Check if light is already in list
                    for (int m = 0; m < numLights; m++) {
                        if (lightList[m] == cellLights[l]) {
                            found = qtrue;
                            break;
                        }
                    }

                    if (!found) {
                        lightList[numLights++] = cellLights[l];
                    }
                }
            }
        }
    }

    return numLights;
}

/*
===============
R_ComputeSceneLighting

Accumulates lighting at a point using the probe/light system
===============
*/
void R_ComputeSceneLighting(const vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir) {
    const int maxNearby = 32;
    renderLight_t *nearby[32];
    vec3_t directionAccum;
    float directionWeight = 0.0f;
    int numLights;
    int i;

    // Default baseline so scenes without probes still have some light
    VectorSet(ambientLight,
              tr.identityLight * 16.0f,
              tr.identityLight * 16.0f,
              tr.identityLight * 16.0f);
    VectorClear(directedLight);
    VectorClear(lightDir);
    VectorClear(directionAccum);

    // Ensure the spatial grid is ready before sampling
    R_BuildLightGrid();
    numLights = R_GetNearbyLights(point, nearby, maxNearby);

    if (numLights <= 0) {
        float fallback = tr.identityLight * 150.0f;
        VectorSet(ambientLight, fallback, fallback, fallback);
        VectorSet(directedLight, fallback, fallback, fallback);

        if (VectorLengthSquared(tr.sunDirection) > 0.0f) {
            VectorCopy(tr.sunDirection, lightDir);
        } else {
            VectorSet(lightDir, 0.0f, 0.0f, 1.0f);
        }
        return;
    }

    for (i = 0; i < numLights; ++i) {
        renderLight_t *light = nearby[i];
        float brightness = 0.0f;
        float ambientRatio = 0.0f;
        float directRatio = 0.0f;
        vec3_t dirToLight;
        VectorClear(dirToLight);

        if (!light) {
            continue;
        }

        switch (light->type) {
            case RL_DIRECTIONAL: {
                vec3_t dir;
                VectorCopy(light->target, dir);
                if (VectorNormalize(dir) == 0.0f) {
                    VectorCopy(tr.sunDirection, dir);
                    if (VectorNormalize(dir) == 0.0f) {
                        VectorSet(dir, 0.0f, 0.0f, -1.0f);
                    }
                }
                VectorScale(dir, -1.0f, dirToLight);

                brightness = light->intensity > 0.0f ? light->intensity : 0.0f;
                ambientRatio = 0.15f;
                directRatio = 0.85f;
                break;
            }

            case RL_OMNI:
            case RL_PROJ: {
                vec3_t delta;
                float distance;
                float denom;
                float radius;

                VectorSubtract(light->origin, point, delta);
                distance = VectorLength(delta);
                if (distance > 0.0f) {
                    VectorScale(delta, 1.0f / distance, dirToLight);
                } else {
                    VectorSet(dirToLight, 0.0f, 0.0f, 1.0f);
                }

                radius = (light->cutoffDistance > 0.0f) ? light->cutoffDistance : light->radius;
                if (radius > 0.0f && distance > radius) {
                    continue;
                }

                denom = light->constant +
                        light->linear * distance +
                        light->quadratic * distance * distance;
                if (denom <= 0.0001f) {
                    denom = 0.0001f;
                }

                brightness = (light->intensity > 0.0f ? light->intensity : 0.0f) * (1.0f / denom);
                ambientRatio = 0.35f;
                directRatio = 0.65f;
                break;
            }

            case RL_AMBIENT:
                brightness = light->intensity > 0.0f ? light->intensity : 0.0f;
                ambientRatio = 1.0f;
                directRatio = 0.0f;
                break;

            default:
                continue;
        }

        if (brightness <= 0.0f) {
            continue;
        }

        // Convert brightness into 0..255 scale contributions
        {
            float ambientScale = brightness * ambientRatio * tr.identityLightByte;
            float directScale = brightness * directRatio * tr.identityLightByte;

            if (ambientScale > 0.0f) {
                VectorMA(ambientLight, ambientScale, light->color, ambientLight);
            }

            if (directScale > 0.0f) {
                VectorMA(directedLight, directScale, light->color, directedLight);
                VectorMA(directionAccum, directScale, dirToLight, directionAccum);
                directionWeight += directScale;
            }
        }
    }

    if (directionWeight > 0.0f && VectorNormalize2(directionAccum, lightDir) != 0.0f) {
        // normalised above
    } else if (VectorLengthSquared(tr.sunDirection) > 0.0f) {
        VectorCopy(tr.sunDirection, lightDir);
    } else {
        VectorSet(lightDir, 0.0f, 0.0f, 1.0f);
    }

    // Clamp outputs to byte range
    for (i = 0; i < 3; ++i) {
        ambientLight[i] = Com_Clamp(0.0f, (float)tr.identityLightByte, ambientLight[i]);
        directedLight[i] = Com_Clamp(0.0f, (float)tr.identityLightByte, directedLight[i]);
    }
}

/*
===============
R_DrawLightGrid

Debug visualization of the light grid
===============
*/
void R_DrawLightGrid(void) {
    int i, j, k;
    vec3_t cellMin, cellMax;
    vec4_t color;
    
    if (!s_lightGrid || !r_showLightVolumes || !r_showLightVolumes->integer) {
        return;
    }
    
    // Draw grid cells that contain lights
    for (i = 0; i < s_lightGrid->gridSize[0]; i++) {
        for (j = 0; j < s_lightGrid->gridSize[1]; j++) {
            for (k = 0; k < s_lightGrid->gridSize[2]; k++) {
                int cellIndex = i + j * s_lightGrid->gridSize[0] + 
                               k * s_lightGrid->gridSize[0] * s_lightGrid->gridSize[1];
                
                if (s_lightGrid->cells[cellIndex] && s_lightGrid->cells[cellIndex][0]) {
                    // Calculate cell bounds
                    cellMin[0] = s_lightGrid->mins[0] + i * s_lightGrid->cellSize[0];
                    cellMin[1] = s_lightGrid->mins[1] + j * s_lightGrid->cellSize[1];
                    cellMin[2] = s_lightGrid->mins[2] + k * s_lightGrid->cellSize[2];
                    cellMax[0] = cellMin[0] + s_lightGrid->cellSize[0];
                    cellMax[1] = cellMin[1] + s_lightGrid->cellSize[1];
                    cellMax[2] = cellMin[2] + s_lightGrid->cellSize[2];
                    
                    // Count lights in cell
                    int lightCount = 0;
                    renderLight_t **lights = s_lightGrid->cells[cellIndex];
                    while (lights[lightCount]) {
                        lightCount++;
                    }
                    
                    // Color based on light count
                    if (lightCount == 1) {
                        VectorSet(color, 0, 0, 1); // Blue for 1 light
                    } else if (lightCount == 2) {
                        VectorSet(color, 0, 1, 0); // Green for 2 lights
                    } else if (lightCount == 3) {
                        VectorSet(color, 1, 1, 0); // Yellow for 3 lights
                    } else {
                        VectorSet(color, 1, 0, 0); // Red for 4+ lights
                    }
                    color[3] = 0.2f;
                    
                    // Draw cell bounds
                    R_DebugBounds(cellMin, cellMax, color);
                }
            }
        }
    }
}
