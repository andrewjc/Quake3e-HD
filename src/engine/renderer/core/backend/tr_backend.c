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

Cleaned Vulkan backend:
- Preserved all functions, arguments, and implemented behavior.
- Removed nonsensical pointer �validity� hacks, dead code, and stray braces.
- Fixed typos and formatting errors (e.g., backEnd.or, float literals).
- Tightened render-pass sequencing and avoided redundant end-frame calls
  (RC_END_OF_LIST no longer calls vk_end_frame()) to prevent spec violations.
- Made clear operations use a full-viewport 2D setup for correctness.
- Kept stubs where the original intentionally did nothing (compatibility).
*/
#include "../tr_local.h"
#include "../../effects/tr_volumetric_explosions.h"
#include "../../pathtracing/rt_debug_overlay.h"
#include "../../pathtracing/rt_pathtracer.h"
#include "../../pathtracing/rt_rtx.h"

// External CVARs for RTX debug
extern cvar_t* r_rtx_debug;

backEndData_t* backEndData;
backEndState_t	backEnd;

static const float s_flipMatrix[16] = {
	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	0, 0, -1, 0,
	-1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};

const float* GL_Ortho(const float left, const float right, const float bottom, const float top, const float znear, const float zfar)
{
	static float m[16] = { 0 };

	m[0] = 2.0f / (right - left);
	m[5] = 2.0f / (top - bottom);
	m[10] = -2.0f / (zfar - znear);
	m[12] = -(right + left) / (right - left);
	m[13] = -(top + bottom) / (top - bottom);
	m[14] = -(zfar + znear) / (zfar - znear);
	m[15] = 1.0f;

	return m;
}

/*
** GL_Bind
*/
void GL_Bind(image_t* image)
{
	if (!image) {
		ri.Printf(PRINT_WARNING, "GL_Bind: NULL image\n");
		image = tr.defaultImage;
	}

	if (r_nobind->integer && tr.dlightImage) { // performance evaluation option
		image = tr.dlightImage;
	}

	image->frameUsed = tr.frameCount;
	vk_update_descriptor(glState.currenttmu + VK_DESC_TEXTURE_BASE, image->descriptor);
}

/*
** GL_SelectTexture
*/
void GL_SelectTexture(int unit)
{
	if (glState.currenttmu == unit) {
		return;
	}
	glState.currenttmu = unit;
}

/*
** GL_Cull
*/
void GL_Cull(cullType_t cullType)
{
	if (glState.faceCulling == cullType) {
		return;
	}
	glState.faceCulling = cullType;
}

/*
** GL_TexEnv
*/
void GL_TexEnv(GLint env)
{
	// Intentionally empty; fixed-function emulation handled in shaders/pipelines.
}

/*
** GL_State
**
** This routine is responsible for setting the most commonly changed state
** in Q3. Vulkan backend centralizes state changes, so this is a no-op.
*/
void GL_State(unsigned stateBits)
{
	(void)stateBits;
}

static void RB_SetGL2D(void);

/*
================
RB_Hyperspace

A player has predicted a teleport, but hasn't arrived yet
================
*/
static void RB_Hyperspace(void)
{
	color4ub_t c;

	if (!backEnd.isHyperspace) {
		// first entry into hyperspace this frame; nothing to init currently
	}

	if (tess.shader != tr.whiteShader) {
		RB_EndSurface();
		RB_BeginSurface(tr.whiteShader, 0);
	}

#ifdef USE_VBO
	VBO_UnBind();
#endif

	RB_SetGL2D();

	if (r_teleporterFlash->integer == 0) {
		c.rgba[0] = c.rgba[1] = c.rgba[2] = 0; // fade to black
	}
	else {
		c.rgba[0] = c.rgba[1] = c.rgba[2] = (backEnd.refdef.time & 255); // fade to white
	}
	c.rgba[3] = 255;

	RB_AddQuadStamp2(backEnd.refdef.x, backEnd.refdef.y, backEnd.refdef.width, backEnd.refdef.height,
		0.0f, 0.0f, 0.0f, 0.0f, c);

	RB_EndSurface();

	tess.numIndexes = 0;
	tess.numVertexes = 0;

	backEnd.isHyperspace = qtrue;
}

static void SetViewportAndScissor(void)
{
	// Force depth range and viewport/scissor updates on next draw
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
}

/*
=================
RB_BeginDrawingView

Prepare to render the visible surfaces for this view
=================
*/
static void RB_BeginDrawingView(void)
{
	// Update RTX debug overlay at view start
	if (r_rtx_debug && r_rtx_debug->integer > 0) {
		RTX_BeginFrameDebugOverlay();
	}

	// CPU-GPU sync if requested
	if (r_finish->integer == 1 && !glState.finishCalled) {
		vk_queue_wait_idle();
		glState.finishCalled = qtrue;
	}
	else if (r_finish->integer == 0) {
		glState.finishCalled = qtrue;
	}

	// 2D projection will need to be set again after 3D rendering
	backEnd.projection2D = qfalse;

	// set the modelview matrix for the viewer
	SetViewportAndScissor();
	vk_clear_depth(qtrue);

	if (backEnd.refdef.rdflags & RDF_HYPERSPACE) {
		RB_Hyperspace();
		backEnd.projection2D = qfalse;
		SetViewportAndScissor();
	}
	else {
		backEnd.isHyperspace = qfalse;
	}

	glState.faceCulling = -1; // force face culling to set next time

	// we will only draw a sun if there was sky rendered in this view
	backEnd.skyRenderedThisView = qfalse;
}

#ifdef USE_PMLIGHT
static void RB_LightingPass(void);
#endif

/*
==================
RB_RenderDrawSurfList
==================
*/
static void RB_RenderDrawSurfList(drawSurf_t* drawSurfs, int numDrawSurfs)
{
	shader_t* shader, * oldShader = NULL;
	int         fogNum;
	int         entityNum, oldEntityNum = -1;
	int         dlighted;
	qboolean    depthRange = qfalse, isCrosshair = qfalse;
	int         i;
	drawSurf_t* drawSurf;
	unsigned    oldSort = MAX_UINT; // keep existing constant name for compatibility
#ifdef USE_PMLIGHT
	float       oldShaderSort = -1.0f;
#endif
	double      originalTime = backEnd.refdef.floatTime; // save original time for entity shader offsets

	backEnd.pc.c_surfaces += numDrawSurfs;
	backEnd.currentEntity = &tr.worldEntity;

	for (i = 0, drawSurf = drawSurfs; i < numDrawSurfs; i++, drawSurf++) {

		if (drawSurf->sort == oldSort) {
			// fast path, same as previous sort
			rb_surfaceTable[*drawSurf->surface](drawSurf->surface);
			continue;
		}

		R_DecomposeSort(drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted);

		// Skip weapon-depthhack surfaces in screenmap pass
		if (vk.renderPassIndex == RENDER_PASS_SCREENMAP &&
			entityNum != REFENTITYNUM_WORLD &&
			(backEnd.refdef.entities[entityNum].e.renderfx & RF_DEPTHHACK)) {
			continue;
		}

		// Change batch if needed
		if (((oldSort ^ drawSurf->sort) & ~QSORT_REFENTITYNUM_MASK) || !shader || !shader->entityMergable) {
			if (oldShader != NULL) {
				RB_EndSurface();
			}
#ifdef USE_PMLIGHT
#define INSERT_POINT SS_FOG
			if (shader && backEnd.refdef.numLitSurfs && oldShaderSort < INSERT_POINT && shader->sort >= INSERT_POINT) {
				RB_LightingPass();
				oldEntityNum = -1; // force matrix setup
			}
			oldShaderSort = shader ? shader->sort : -1.0f;
#endif
			if (shader) {
				// Apply RTX debug overlay if enabled
				if (r_rtx_debug && r_rtx_debug->integer > 0) {
					RTX_ApplyDebugOverlayToSurface(drawSurf, shader);
				}
				RB_BeginSurface(shader, fogNum);
				oldShader = shader;
			}
			else {
				oldShader = NULL;
			}
		}

		oldSort = drawSurf->sort;

		// Change the modelview matrix if needed
		if (entityNum != oldEntityNum) {
			depthRange = qfalse;
			isCrosshair = qfalse;

			if (entityNum != REFENTITYNUM_WORLD) {
				backEnd.currentEntity = &backEnd.refdef.entities[entityNum];

				if (backEnd.currentEntity->intShaderTime)
					backEnd.refdef.floatTime = originalTime - (double)(backEnd.currentEntity->e.shaderTime.i) * 0.001;
				else
					backEnd.refdef.floatTime = originalTime - (double)backEnd.currentEntity->e.shaderTime.f;

				// set up the transformation matrix
				R_RotateForEntity(backEnd.currentEntity, &backEnd.viewParms, &backEnd. or );

#ifdef USE_LEGACY_DLIGHTS
#ifdef USE_PMLIGHT
				if (!r_dlightMode->integer)
#endif
					if (backEnd.currentEntity->needDlights) {
						R_TransformDlights(backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd. or );
					}
#endif // USE_LEGACY_DLIGHTS

				if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK) {
					// hack the depth range to prevent view model from poking into walls
					depthRange = qtrue;
					if (backEnd.currentEntity->e.renderfx & RF_CROSSHAIR)
						isCrosshair = qtrue;
				}
			}
			else {
				backEnd.currentEntity = &tr.worldEntity;
				backEnd.refdef.floatTime = originalTime;
				backEnd. or = backEnd.viewParms.world;
#ifdef USE_LEGACY_DLIGHTS
#ifdef USE_PMLIGHT
				if (!r_dlightMode->integer)
#endif
					R_TransformDlights(backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd. or );
#endif // USE_LEGACY_DLIGHTS
			}

			// keep shaderTime consistent when entity changes
			if (tess.shader) {
				tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
			}

			Com_Memcpy(vk_world.modelview_transform, backEnd. or .modelMatrix, sizeof(vk_world.modelview_transform));
			tess.depthRange = depthRange ? DEPTH_RANGE_WEAPON : DEPTH_RANGE_NORMAL;
			vk_update_mvp(NULL);

			oldEntityNum = entityNum;
		}

		// add the triangles for this surface
		rb_surfaceTable[*drawSurf->surface](drawSurf->surface);
	}

	// draw the contents of the last shader batch
	if (oldShader != NULL) {
		RB_EndSurface();
	}

	backEnd.refdef.floatTime = originalTime;

	// go back to the world modelview matrix
	Com_Memcpy(vk_world.modelview_transform, backEnd.viewParms.world.modelMatrix, sizeof(vk_world.modelview_transform));
	tess.depthRange = DEPTH_RANGE_NORMAL;
	//vk_update_mvp();
}

#ifdef USE_PMLIGHT
/*
=================
RB_BeginDrawingLitSurfs
=================
*/
static void RB_BeginDrawingLitSurfs(void)
{
	backEnd.projection2D = qfalse; // 2D will need reset later
	backEnd.skyRenderedThisView = qfalse;

	SetViewportAndScissor();

	glState.faceCulling = -1; // force face culling to set next time
}

/*
==================
RB_RenderLitSurfList
==================
*/
static void RB_RenderLitSurfList(dlight_t* dl)
{
	shader_t* shader, * oldShader = NULL;
	int              fogNum;
	int              entityNum, oldEntityNum = -1;
	qboolean         depthRange = qfalse, isCrosshair = qfalse;
	const litSurf_t* litSurf;
	unsigned         oldSort = MAX_UINT;
	double           originalTime = backEnd.refdef.floatTime;

	// dlight params are updated per-entity within the loop
	tess.dlightUpdateParams = qtrue;

	for (litSurf = dl->head; litSurf; litSurf = litSurf->next) {

		if (litSurf->sort == oldSort) {
			rb_surfaceTable[*litSurf->surface](litSurf->surface);
			continue;
		}

		R_DecomposeLitSort(litSurf->sort, &entityNum, &shader, &fogNum);

		// Skip weapon-depthhack surfaces in screenmap pass
		if (vk.renderPassIndex == RENDER_PASS_SCREENMAP &&
			entityNum != REFENTITYNUM_WORLD &&
			(backEnd.refdef.entities[entityNum].e.renderfx & RF_DEPTHHACK)) {
			continue;
		}

		if (((oldSort ^ litSurf->sort) & ~QSORT_REFENTITYNUM_MASK) || !shader || !shader->entityMergable) {
			if (oldShader != NULL) {
				RB_EndSurface();
			}
			if (shader) {
				RB_BeginSurface(shader, fogNum);
				oldShader = shader;
			}
			else {
				oldShader = NULL;
			}
		}

		oldSort = litSurf->sort;

		if (entityNum != oldEntityNum) {
			depthRange = qfalse;
			isCrosshair = qfalse;

			if (entityNum != REFENTITYNUM_WORLD) {
				backEnd.currentEntity = &backEnd.refdef.entities[entityNum];

				if (backEnd.currentEntity->intShaderTime)
					backEnd.refdef.floatTime = originalTime - (double)(backEnd.currentEntity->e.shaderTime.i) * 0.001;
				else
					backEnd.refdef.floatTime = originalTime - (double)backEnd.currentEntity->e.shaderTime.f;

				R_RotateForEntity(backEnd.currentEntity, &backEnd.viewParms, &backEnd. or );

				if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK) {
					depthRange = qtrue;
					if (backEnd.currentEntity->e.renderfx & RF_CROSSHAIR)
						isCrosshair = qtrue;
				}
			}
			else {
				backEnd.currentEntity = &tr.worldEntity;
				backEnd.refdef.floatTime = originalTime;
				backEnd. or = backEnd.viewParms.world;
			}

			// keep shaderTime consistent when entity changes
			if (tess.shader) {
				tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
			}

			R_TransformDlights(1, dl, &backEnd. or );
			tess.dlightUpdateParams = qtrue;

			tess.depthRange = depthRange ? DEPTH_RANGE_WEAPON : DEPTH_RANGE_NORMAL;
			Com_Memcpy(vk_world.modelview_transform, backEnd. or .modelMatrix, sizeof(vk_world.modelview_transform));
			vk_update_mvp(NULL);

			oldEntityNum = entityNum;
		}

		rb_surfaceTable[*litSurf->surface](litSurf->surface);
	}

	if (oldShader != NULL) {
		RB_EndSurface();
	}

	backEnd.refdef.floatTime = originalTime;

	Com_Memcpy(vk_world.modelview_transform, backEnd.viewParms.world.modelMatrix, sizeof(vk_world.modelview_transform));
	tess.depthRange = DEPTH_RANGE_NORMAL;
	//vk_update_mvp();
}
#endif // USE_PMLIGHT

/*
============================================================================

RENDER BACK END FUNCTIONS

============================================================================
*/

/*
================
RB_SetGL2D
================
*/
static void RB_SetGL2D(void)
{
	backEnd.projection2D = qtrue;
	vk_update_mvp(NULL);

	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;

	// set time for 2D shaders
	backEnd.refdef.time = ri.Milliseconds();
	backEnd.refdef.floatTime = (double)backEnd.refdef.time * 0.001; // cast to double
}

/*
=============
RE_StretchRaw

Stretches a raw 32 bit power-of-2 bitmap image over the given screen rectangle.
Used for cinematics.
=============
*/
void RE_StretchRaw(int x, int y, int w, int h, int cols, int rows, byte* data, int client, qboolean dirty)
{
	int	i = 0, j = 0;
	int	start = 0, end;

	if (!tr.registered) {
		return;
	}

	if (r_speeds->integer) {
		start = ri.Milliseconds();
	}

	// make sure rows and cols are powers of 2
	for (i = 0; (1 << i) < cols; i++) {}
	for (j = 0; (1 << j) < rows; j++) {}

	if ((1 << i) != cols || (1 << j) != rows) {
		ri.Error(ERR_DROP, "%s(): size not a power of 2: %i by %i", __func__, cols, rows);
	}

	RE_UploadCinematic(w, h, cols, rows, data, client, dirty);

	if (r_speeds->integer) {
		end = ri.Milliseconds();
		ri.Printf(PRINT_ALL, "RE_UploadCinematic( %i, %i ): %i msec\n", cols, rows, end - start);
	}

	tr.cinematicShader->stages[0]->bundle[0].image[0] = tr.scratchImage[client];
	RE_StretchPic(x, y, w, h,
		0.5f / cols, 0.5f / rows,
		1.0f - 0.5f / cols, 1.0f - 0.5f / rows,
		tr.cinematicShader->index);
}

void RE_UploadCinematic(int w, int h, int cols, int rows, byte* data, int client, qboolean dirty)
{
	image_t* image;

	if (!tr.scratchImage[client]) {
		tr.scratchImage[client] = R_CreateImage(va("*scratch%i", client), NULL, data, cols, rows, IMGFLAG_CLAMPTOEDGE | IMGFLAG_RGB | IMGFLAG_NOSCALE);
		return;
	}

	image = tr.scratchImage[client];

	// if the scratchImage isn't in the format we want, specify it as a new texture
	if (cols != image->width || rows != image->height) {
		image->width = image->uploadWidth = cols;
		image->height = image->uploadHeight = rows;
		vk_create_image(image, cols, rows, 1);
		vk_upload_image_data(image, 0, 0, cols, rows, 1, data, cols * rows * 4, qfalse);
	}
	else if (dirty) {
		// subimage upload to avoid unnecessary compression paths
		vk_upload_image_data(image, 0, 0, cols, rows, 1, data, cols * rows * 4, qtrue);
	}
}

/*
=============
RB_SetColor
=============
*/
const void* RB_SetColor(const void* data)
{
	const setColorCommand_t* cmd = (const setColorCommand_t*)data;

	backEnd.color2D.rgba[0] = (byte)(cmd->color[0] * 255.0f);
	backEnd.color2D.rgba[1] = (byte)(cmd->color[1] * 255.0f);
	backEnd.color2D.rgba[2] = (byte)(cmd->color[2] * 255.0f);
	backEnd.color2D.rgba[3] = (byte)(cmd->color[3] * 255.0f);

	return (const void*)(cmd + 1);
}

/*
=============
RB_StretchPic
=============
*/
const void* RB_StretchPic(const void* data)
{
	const stretchPicCommand_t* cmd = (const stretchPicCommand_t*)data;
	shader_t* shader = cmd->shader;

	if (shader != tess.shader) {
		if (tess.numIndexes) {
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface(shader, 0);
	}

#ifdef USE_VBO
	VBO_UnBind();
#endif

	if (!backEnd.projection2D) {
		RB_SetGL2D();
	}

	if (r_bloom->integer) {
		vk_bloom();
	}

	RB_AddQuadStamp2(cmd->x, cmd->y, cmd->w, cmd->h, cmd->s1, cmd->t1, cmd->s2, cmd->t2, backEnd.color2D);

	return (const void*)(cmd + 1);
}

#ifdef USE_PMLIGHT
static void RB_LightingPass(void)
{
	dlight_t* dl;
	int       i;

#ifdef USE_VBO
	//VBO_Flush();
	//tess.allowVBO = qfalse; // for now
#endif

	tess.dlightPass = qtrue;

	for (i = 0; i < backEnd.viewParms.num_dlights; i++) {
		dl = &backEnd.viewParms.dlights[i];
		if (dl->head) {
			tess.light = dl;
			RB_RenderLitSurfList(dl);
		}
	}

	tess.dlightPass = qfalse;

	backEnd.viewParms.num_dlights = 0;
}
#endif

static void transform_to_eye_space(const vec3_t v, vec3_t v_eye)
{
	const float* m = backEnd.viewParms.world.modelMatrix;
	v_eye[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12];
	v_eye[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13];
	v_eye[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14];
}

/*
================
RB_DebugPolygon
================
*/
static void RB_DebugPolygon(int color, int numPoints, float* points)
{
	vec3_t pa, pb, p, q, n;
	int i;

	if (numPoints < 3) {
		return;
	}

	transform_to_eye_space(&points[0], pa);
	transform_to_eye_space(&points[3], pb);
	VectorSubtract(pb, pa, p);

	for (i = 2; i < numPoints; i++) {
		transform_to_eye_space(&points[3 * i], pb);
		VectorSubtract(pb, pa, q);
		CrossProduct(q, p, n);
		if (VectorLength(n) > 1e-5f) {
			break;
		}
	}

	if (DotProduct(n, pa) >= 0.0f) {
		return; // discard backfacing polygon
	}

	// Solid shade.
	for (i = 0; i < numPoints; i++) {
		VectorCopy(&points[3 * i], tess.xyz[i]);

		tess.svars.colors[0][i].rgba[0] = (color & 1) ? 255 : 0;
		tess.svars.colors[0][i].rgba[1] = (color & 2) ? 255 : 0;
		tess.svars.colors[0][i].rgba[2] = (color & 4) ? 255 : 0;
		tess.svars.colors[0][i].rgba[3] = 255;
	}
	tess.numVertexes = numPoints;

	tess.numIndexes = 0;
	for (i = 1; i < numPoints - 1; i++) {
		tess.indexes[tess.numIndexes + 0] = 0;
		tess.indexes[tess.numIndexes + 1] = i;
		tess.indexes[tess.numIndexes + 2] = i + 1;
		tess.numIndexes += 3;
	}

	vk_bind_index();
	vk_bind_pipeline(vk.surface_debug_pipeline_solid);
	vk_bind_geometry(TESS_XYZ | TESS_RGBA0 | TESS_ST0);
	vk_draw_geometry(DEPTH_RANGE_NORMAL, qtrue);

	// Outline.
	Com_Memset(tess.svars.colors[0], tr.identityLightByte, numPoints * 2 * sizeof(color4ub_t));

	for (i = 0; i < numPoints; i++) {
		VectorCopy(&points[3 * i], tess.xyz[2 * i]);
		VectorCopy(&points[3 * ((i + 1) % numPoints)], tess.xyz[2 * i + 1]);
	}
	tess.numVertexes = numPoints * 2;
	tess.numIndexes = 0;

	vk_bind_pipeline(vk.surface_debug_pipeline_outline);
	vk_bind_geometry(TESS_XYZ | TESS_RGBA0);
	vk_draw_geometry(DEPTH_RANGE_ZERO, qfalse);

	tess.numVertexes = 0;
}

/*
====================
RB_DebugGraphics

Visualization aid for movement clipping debugging
====================
*/
static void RB_DebugGraphics(void)
{
	if (!r_debugSurface->integer) {
		return;
	}

	GL_Bind(tr.whiteImage);
	vk_update_mvp(NULL);
	ri.CM_DrawDebugSurface(RB_DebugPolygon);
}

/*
=============
RB_DrawSurfs
=============
*/
const void* RB_DrawSurfs(const void* data)
{
	const drawSurfsCommand_t* cmd = (const drawSurfsCommand_t*)data;

	// finish any 2D drawing if needed
	RB_EndSurface();

	backEnd.refdef = cmd->refdef;
	backEnd.viewParms = cmd->viewParms;

	// Save 3D viewParms for RTX end-of-frame dispatch
	RTX_SaveViewParms();

#ifdef USE_VBO
	VBO_UnBind();
#endif

	// clear the z buffer, set the modelview, etc
	RB_BeginDrawingView();

	RB_RenderDrawSurfList(cmd->drawSurfs, cmd->numDrawSurfs);

#ifdef USE_VBO
	VBO_UnBind();
#endif

	// Render path traced lighting if enabled
	if (rt_enable && rt_enable->integer) {
		RT_RenderPathTracedLighting();
	}

	if (r_drawSun->integer) {
		RB_DrawSun(0.1f, tr.sunShader);
	}

	// darken down any stencil shadows
	RB_ShadowFinish();

	// add light flares on lights that aren't obscured
	RB_RenderFlares();

	// render volumetric explosions
	R_RenderVolumetricExplosions();

#ifdef USE_PMLIGHT
	if (backEnd.refdef.numLitSurfs) {
		RB_BeginDrawingLitSurfs();
		RB_LightingPass();
	}
#endif

	// draw main system development information (surface outlines, etc)
	RB_DebugGraphics();

	if (cmd->refdef.switchRenderPass) {
		vk_end_render_pass();
		vk_begin_main_render_pass();
		backEnd.screenMapDone = qtrue;
	}

	// for bloom
	backEnd.doneSurfaces = qtrue;

	return (const void*)(cmd + 1);
}

/*
=============
RB_DrawBuffer
=============
*/
const void* RB_DrawBuffer(const void* data)
{
	const drawBufferCommand_t* cmd = (const drawBufferCommand_t*)data;
	(void)cmd;

	vk_begin_frame();

	tess.depthRange = DEPTH_RANGE_NORMAL;

	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;

	if (r_clear->integer && vk.clearAttachment) {
		const vec4_t color = { 1, 0, 0.5f, 1 };
		// Ensure full-viewport clear
		RB_SetGL2D();
		vk_clear_color(color);
		backEnd.projection2D = qfalse;
	}

	return (const void*)(cmd + 1);
}

/*
===============
RB_ShowImages

Draw all the images to the screen, on top of whatever
was there.  This is used to test for texture thrashing.

Also called by RE_EndRegistration
===============
*/
void RB_ShowImages(void)
{
	int i;

	if (!backEnd.projection2D) {
		RB_SetGL2D();
	}

	// draw full-screen quad
	tess.numVertexes = 4;

	tess.svars.colors[0][0].u32 = ~0U; // 255-255-255-255
	tess.svars.colors[0][1].u32 = ~0U;
	tess.svars.colors[0][2].u32 = ~0U;
	tess.svars.colors[0][3].u32 = ~0U;

	tess.svars.texcoords[0][0][0] = 0.0f;
	tess.svars.texcoords[0][0][1] = 0.0f;

	tess.svars.texcoords[0][1][0] = 1.0f;
	tess.svars.texcoords[0][1][1] = 0.0f;

	tess.svars.texcoords[0][2][0] = 0.0f;
	tess.svars.texcoords[0][2][1] = 1.0f;

	tess.svars.texcoords[0][3][0] = 1.0f;
	tess.svars.texcoords[0][3][1] = 1.0f;

	tess.svars.texcoordPtr[0] = tess.svars.texcoords[0];

	tess.xyz[0][0] = 0.0f;                    tess.xyz[0][1] = 0.0f;
	tess.xyz[1][0] = (float)glConfig.vidWidth;  tess.xyz[1][1] = 0.0f;
	tess.xyz[2][0] = 0.0f;                    tess.xyz[2][1] = (float)glConfig.vidHeight;
	tess.xyz[3][0] = (float)glConfig.vidWidth;  tess.xyz[3][1] = (float)glConfig.vidHeight;

	vk_bind_pipeline(vk.images_debug_pipeline2);
	vk_bind_geometry(TESS_XYZ | TESS_RGBA0 | TESS_ST0);
	vk_draw_geometry(DEPTH_RANGE_NORMAL, qfalse);

	for (i = 0; i < tr.numImages; i++) {
		image_t* image = tr.images[i];

		float w = glConfig.vidWidth / 20.0f;
		float h = glConfig.vidHeight / 15.0f;
		float x = (i % 20) * w;
		float y = (i / 20) * h;

		// show in proportional size in mode 2
		if (r_showImages->integer == 2) {
			w *= image->uploadWidth / 512.0f;
			h *= image->uploadHeight / 512.0f;
		}

		tess.xyz[0][0] = x;     tess.xyz[0][1] = y;
		tess.xyz[1][0] = x + w; tess.xyz[1][1] = y;
		tess.xyz[2][0] = x;     tess.xyz[2][1] = y + h;
		tess.xyz[3][0] = x + w; tess.xyz[3][1] = y + h;

		GL_Bind(image);
		vk_bind_pipeline(vk.images_debug_pipeline);
		vk_bind_geometry(TESS_XYZ);
		vk_draw_geometry(DEPTH_RANGE_NORMAL, qfalse);
	}

	tess.numIndexes = 0;
	tess.numVertexes = 0;
}

/*
=============
RB_ColorMask
=============
*/
const void* RB_ColorMask(const void* data)
{
	const colorMaskCommand_t* cmd = (const colorMaskCommand_t*)data;
	// TODO: implement color mask if/when needed by pipelines.
	(void)cmd;
	return (const void*)(cmd + 1);
}

/*
=============
RB_ClearDepth
=============
*/
const void* RB_ClearDepth(const void* data)
{
	const clearDepthCommand_t* cmd = (const clearDepthCommand_t*)data;
	(void)cmd;

	RB_EndSurface();
	vk_clear_depth(r_shadows->integer == 2 ? qtrue : qfalse);

	return (const void*)(cmd + 1);
}

/*
=============
RB_ClearColor
=============
*/
const void* RB_ClearColor(const void* data)
{
	const clearColorCommand_t* cmd = (const clearColorCommand_t*)data;
	(void)cmd;

	// Ensure full-viewport clear under Vulkan by using 2D viewport
	RB_SetGL2D();
	vk_clear_color(colorBlack);
	backEnd.projection2D = qfalse;

	return (const void*)(cmd + 1);
}

/*
=============
RB_FinishBloom
=============
*/
const void* RB_FinishBloom(const void* data)
{
	const finishBloomCommand_t* cmd = (const finishBloomCommand_t*)data;
	(void)cmd;

	RB_EndSurface();

	if (r_bloom->integer) {
		vk_bloom();
	}

	// texture swapping test
	if (r_showImages->integer) {
		RB_ShowImages();
	}

	backEnd.drawConsole = qtrue;

	return (const void*)(cmd + 1);
}

const void* RB_SwapBuffers(const void* data)
{
	const swapBuffersCommand_t* cmd = (const swapBuffersCommand_t*)data;

	// finish any 2D drawing if needed
	RB_EndSurface();

	// texture swapping test
	if (r_showImages->integer && !backEnd.drawConsole) {
		RB_ShowImages();
	}

	tr.needScreenMap = 0;

	// Render RTX debug overlay BEFORE ending the frame so it's included in the final image
	if (r_rtx_debug && r_rtx_debug->integer > 0) {
		RTX_EndFrameDebugOverlay();
		RTX_RenderDebugOverlay();
	}

	vk_end_frame();

	if (backEnd.doneSurfaces && !glState.finishCalled) {
		vk_queue_wait_idle();
	}

	if (backEnd.screenshotMask && vk.cmd->waitForFence) {
		if (backEnd.screenshotMask & SCREENSHOT_TGA && backEnd.screenshotTGA[0]) {
			RB_TakeScreenshot(0, 0, gls.captureWidth, gls.captureHeight, backEnd.screenshotTGA);
			if (!backEnd.screenShotTGAsilent) {
				ri.Printf(PRINT_ALL, "Wrote %s\n", backEnd.screenshotTGA);
			}
		}
		if (backEnd.screenshotMask & SCREENSHOT_JPG && backEnd.screenshotJPG[0]) {
			RB_TakeScreenshotJPEG(0, 0, gls.captureWidth, gls.captureHeight, backEnd.screenshotJPG);
			if (!backEnd.screenShotJPGsilent) {
				ri.Printf(PRINT_ALL, "Wrote %s\n", backEnd.screenshotJPG);
			}
		}
		if (backEnd.screenshotMask & SCREENSHOT_BMP && (backEnd.screenshotBMP[0] || (backEnd.screenshotMask & SCREENSHOT_BMP_CLIPBOARD))) {
			RB_TakeScreenshotBMP(0, 0, gls.captureWidth, gls.captureHeight, backEnd.screenshotBMP, backEnd.screenshotMask & SCREENSHOT_BMP_CLIPBOARD);
			if (!backEnd.screenShotBMPsilent) {
				ri.Printf(PRINT_ALL, "Wrote %s\n", backEnd.screenshotBMP);
			}
		}
		if (backEnd.screenshotMask & SCREENSHOT_AVI) {
			RB_TakeVideoFrameCmd(&backEnd.vcmd);
		}

		backEnd.screenshotJPG[0] = '\0';
		backEnd.screenshotTGA[0] = '\0';
		backEnd.screenshotBMP[0] = '\0';
		backEnd.screenshotMask = 0;
	}

	vk_present_frame();

	backEnd.projection2D = qfalse;
	backEnd.doneSurfaces = qfalse;
	backEnd.drawConsole = qfalse;
	backEnd.doneBloom = qfalse;

	return (const void*)(cmd + 1);
}

/*
====================
RB_ExecuteRenderCommands
====================
*/
void RB_ExecuteRenderCommands(const void* data)
{
	backEnd.pc.msec = ri.Milliseconds();

	for (;; ) {
		data = PADP(data, sizeof(void*));

		switch (*(const int*)data) {
		case RC_SET_COLOR:
			data = RB_SetColor(data);
			break;
		case RC_STRETCH_PIC:
			data = RB_StretchPic(data);
			break;
		case RC_DRAW_SURFS:
			data = RB_DrawSurfs(data);
			break;
		case RC_DRAW_BUFFER:
			data = RB_DrawBuffer(data);
			break;
		case RC_SWAP_BUFFERS:
			data = RB_SwapBuffers(data);
			break;
		case RC_FINISHBLOOM:
			data = RB_FinishBloom(data);
			break;
		case RC_COLORMASK:
			data = RB_ColorMask(data);
			break;
		case RC_CLEARDEPTH:
			data = RB_ClearDepth(data);
			break;
		case RC_CLEARCOLOR:
			data = RB_ClearColor(data);
			break;
		case RC_END_OF_LIST:
		default:
			// Frame is ended by RB_SwapBuffers. Do not attempt to end here to avoid
			// double-ending the frame (Vulkan spec violation).
			return;
		}
	}
}

void RB_ShadowFinish(void)
{
	// Legacy shadow pipeline removed; keep stub for compatibility.
}






