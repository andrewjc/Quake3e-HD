/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

RTX Shadow Miss Shader
Handles shadow rays that miss all geometry (no shadow)
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require

// Shadow ray payload - 1.0 = no shadow, 0.0 = full shadow
layout(location = 1) rayPayloadInEXT float shadowPayload;

void main() {
    // Ray missed all geometry, so no shadow
    shadowPayload = 1.0;
}