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
#ifndef TR_COMMON_H
#define TR_COMMON_H

#define USE_VULKAN

#include "../common/q_shared.h"
#include "tr_public.h"

#define MAX_TEXTURE_UNITS 8

typedef enum
{
	IMGFLAG_NONE           = 0x0000,
	IMGFLAG_MIPMAP         = 0x0001,
	IMGFLAG_PICMIP         = 0x0002,
	IMGFLAG_CLAMPTOEDGE    = 0x0004,
	IMGFLAG_CLAMPTOBORDER  = 0x0008,
	IMGFLAG_NO_COMPRESSION = 0x0010,
	IMGFLAG_NOLIGHTSCALE   = 0x0020,
	IMGFLAG_LIGHTMAP       = 0x0040,
	IMGFLAG_NOSCALE        = 0x0080,
	IMGFLAG_RGB            = 0x0100,
	IMGFLAG_COLORSHIFT     = 0x0200,
	IMGFLAG_NORMALMAP      = 0x0400,	// Image is a normal map
	IMGFLAG_NOMIPMAP       = 0x0800,	// Don't generate mipmaps
} imgFlags_t;

// Image type constants for enhanced material system
typedef enum {
	IMGTYPE_COLORALPHA = 0,		// Standard color texture with alpha
	IMGTYPE_NORMAL = 1,			// Normal map texture
	IMGTYPE_SPECULAR = 2,		// Specular map texture
} imgType_t;

// Vertex attribute flags
#define ATTR_POSITION		0x0001
#define ATTR_TEXCOORD		0x0002
#define ATTR_LIGHTCOORD		0x0004
#define ATTR_NORMAL		0x0008
#define ATTR_COLOR		0x0010
#define ATTR_TANGENT		0x0020

// Helper function for float to int conversion
static inline int Q_ftol(float f) {
    return (int)f;
}

// OpenGL blend factor constants
#define GL_ZERO                           0x0000
#define GL_ONE                            0x0001
#define GL_SRC_COLOR                      0x0300
#define GL_ONE_MINUS_SRC_COLOR            0x0301
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_DST_ALPHA                      0x0304
#define GL_ONE_MINUS_DST_ALPHA            0x0305
#define GL_DST_COLOR                      0x0306
#define GL_ONE_MINUS_DST_COLOR            0x0307
#define GL_SRC_ALPHA_SATURATE             0x0308

// OpenGL primitive types
#define GL_LINES                          0x0001
#define GL_LINE_LOOP                      0x0002
#define GL_LINE_STRIP                     0x0003
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006

// Texture binding constants
#define TB_COLORMAP                       0
#define TB_LIGHTMAP                       1
#define TB_NORMALMAP                      2

typedef enum {
	CT_FRONT_SIDED = 0,
	CT_BACK_SIDED,
	CT_TWO_SIDED
} cullType_t;

typedef struct image_s image_t;

// any change in the LIGHTMAP_* defines here MUST be reflected in
// R_FindShader() in tr_bsp.c
#define LIGHTMAP_2D         -4	// shader is for 2D rendering
#define LIGHTMAP_BY_VERTEX  -3	// pre-lit triangle models
#define LIGHTMAP_WHITEIMAGE -2
#define LIGHTMAP_NONE       -1

extern glconfig_t	glConfig;		// outside of TR since it shouldn't be cleared during ref re-init

// These variables should live inside glConfig but can't because of
// compatibility issues to the original ID vms.  If you release a stand-alone
// game and your mod uses tr_types.h from this build you can safely move them
// to the glconfig_t struct.
extern qboolean  textureFilterAnisotropic;
extern int       maxAnisotropy;

//
// cvars
//
//extern cvar_t *r_stencilbits;			// number of desired stencil bits
extern cvar_t *r_texturebits;			// number of desired texture bits
										// 0 = use framebuffer depth
										// 16 = use 16-bit textures
										// 32 = use 32-bit textures
										// all else = error

extern cvar_t *r_drawBuffer;

extern cvar_t *r_allowExtensions;				// global enable/disable of OpenGL extensions
extern cvar_t *r_ext_compressed_textures;		// these control use of specific extensions
extern cvar_t *r_ext_multitexture;
extern cvar_t *r_ext_compiled_vertex_array;
extern cvar_t *r_ext_texture_env_add;

extern cvar_t *r_ext_texture_filter_anisotropic;
extern cvar_t *r_ext_max_anisotropy;

float R_NoiseGet4f( float x, float y, float z, double t );
void  R_NoiseInit( void );

image_t *R_FindImageFile( const char *name, imgFlags_t flags );
image_t *R_CreateImage( const char *name, const char *name2, byte *pic, int width, int height, imgFlags_t flags );
void R_UploadSubImage( byte *data, int x, int y, int width, int height, image_t *image );

qhandle_t RE_RegisterShaderLightMap( const char *name, int lightmapIndex );
qhandle_t RE_RegisterShader( const char *name );
qhandle_t RE_RegisterShaderNoMip( const char *name );
qhandle_t RE_RegisterShaderFromImage(const char *name, int lightmapIndex, image_t *image, qboolean mipRawImage);

// font stuff
void R_InitFreeType( void );
void R_DoneFreeType( void );
void RE_RegisterFont(const char *fontName, int pointSize, fontInfo_t *font);

/*
=============================================================

IMAGE LOADERS

=============================================================
*/

void R_LoadBMP( const char *name, byte **pic, int *width, int *height );
void R_LoadJPG( const char *name, byte **pic, int *width, int *height );
void R_LoadPCX( const char *name, byte **pic, int *width, int *height );
void R_LoadPNG( const char *name, byte **pic, int *width, int *height );
void R_LoadTGA( const char *name, byte **pic, int *width, int *height );

/*
====================================================================

IMPLEMENTATION SPECIFIC FUNCTIONS

====================================================================
*/

#endif
