/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Vertex shader for per-pixel lighting
===========================================================================
*/

#version 330 core

// Vertex attributes
layout(location = 0) in vec3 attr_Position;
layout(location = 1) in vec2 attr_TexCoord0;
layout(location = 2) in vec3 attr_Normal;
layout(location = 3) in vec4 attr_Color;
layout(location = 4) in vec3 attr_Tangent;
layout(location = 5) in vec3 attr_Bitangent;

// Uniforms
uniform mat4 u_ModelViewProjectionMatrix;
uniform mat4 u_ModelViewMatrix;
uniform mat3 u_NormalMatrix;
uniform vec3 u_ViewOrigin;

// Outputs to fragment shader
out vec3 var_Position;
out vec2 var_TexCoords;
out vec3 var_Normal;
out vec4 var_Color;
out vec3 var_Tangent;
out vec3 var_Bitangent;
out vec3 var_ViewDir;

void main()
{
    // Transform vertex position
    gl_Position = u_ModelViewProjectionMatrix * vec4(attr_Position, 1.0);
    
    // World space position for lighting
    vec4 worldPos = u_ModelViewMatrix * vec4(attr_Position, 1.0);
    var_Position = worldPos.xyz;
    
    // Pass through texture coordinates
    var_TexCoords = attr_TexCoord0;
    
    // Transform normal to world space
    var_Normal = normalize(u_NormalMatrix * attr_Normal);
    
    // Transform tangent space vectors
    var_Tangent = normalize(u_NormalMatrix * attr_Tangent);
    var_Bitangent = normalize(u_NormalMatrix * attr_Bitangent);
    
    // Calculate view direction
    var_ViewDir = normalize(u_ViewOrigin - var_Position);
    
    // Pass through vertex color
    var_Color = attr_Color;
}