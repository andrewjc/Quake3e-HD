/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Fragment shader for per-pixel lighting
===========================================================================
*/

#version 330 core

// Inputs from vertex shader
in vec3 var_Position;
in vec2 var_TexCoords;
in vec3 var_Normal;
in vec4 var_Color;
in vec3 var_Tangent;
in vec3 var_Bitangent;
in vec3 var_ViewDir;

// Texture samplers
uniform sampler2D u_DiffuseMap;
uniform sampler2D u_NormalMap;
uniform sampler2D u_SpecularMap;
uniform sampler2D u_GlowMap;

// Light uniforms
uniform vec4 u_LightPosition;     // xyz = position, w = radius
uniform vec4 u_LightColor;        // rgb = color, a = intensity
uniform vec4 u_LightAttenuation;  // x = constant, y = linear, z = quadratic, w = cutoff

// Material properties
uniform float u_SpecularPower = 32.0;
uniform float u_SpecularScale = 1.0;

// Output
out vec4 out_Color;

vec3 CalcBumpedNormal()
{
    // Sample normal map
    vec3 normal = texture(u_NormalMap, var_TexCoords).rgb;
    normal = normalize(normal * 2.0 - 1.0);
    
    // Transform from tangent space to world space
    mat3 TBN = mat3(
        normalize(var_Tangent),
        normalize(var_Bitangent),
        normalize(var_Normal)
    );
    
    return normalize(TBN * normal);
}

vec3 CalculateLighting(vec3 normal, vec3 viewDir, vec3 diffuseColor, vec3 specularColor)
{
    // Calculate light direction and distance
    vec3 lightDir = u_LightPosition.xyz - var_Position;
    float distance = length(lightDir);
    lightDir = normalize(lightDir);
    
    // Early out if beyond light radius
    if (distance > u_LightAttenuation.w) {
        return vec3(0.0);
    }
    
    // Calculate attenuation
    float attenuation = 1.0 / (
        u_LightAttenuation.x +                    // constant
        u_LightAttenuation.y * distance +         // linear
        u_LightAttenuation.z * distance * distance // quadratic
    );
    
    // Radius falloff
    float falloff = max(0.0, 1.0 - (distance / u_LightPosition.w));
    attenuation *= falloff * falloff;
    
    // Diffuse lighting (Lambert)
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = u_LightColor.rgb * u_LightColor.a * diffuseColor * NdotL;
    
    // Specular lighting (Blinn-Phong)
    vec3 specular = vec3(0.0);
    if (NdotL > 0.0) {
        vec3 halfwayDir = normalize(lightDir + viewDir);
        float NdotH = max(dot(normal, halfwayDir), 0.0);
        float specularStrength = pow(NdotH, u_SpecularPower);
        specular = u_LightColor.rgb * u_LightColor.a * specularColor * specularStrength * u_SpecularScale;
    }
    
    // Apply attenuation
    return (diffuse + specular) * attenuation;
}

void main()
{
    // Sample textures
    vec4 diffuse = texture(u_DiffuseMap, var_TexCoords);
    
    // Alpha test
    if (diffuse.a < 0.5) {
        discard;
    }
    
    // Apply vertex color
    diffuse *= var_Color;
    
    // Get normal (either from normal map or interpolated)
    vec3 normal;
    if (textureSize(u_NormalMap, 0).x > 1) {
        normal = CalcBumpedNormal();
    } else {
        normal = normalize(var_Normal);
    }
    
    // Sample specular map
    vec3 specular = texture(u_SpecularMap, var_TexCoords).rgb;
    
    // Calculate lighting
    vec3 lighting = CalculateLighting(normal, var_ViewDir, diffuse.rgb, specular);
    
    // Add glow/emissive
    vec3 glow = texture(u_GlowMap, var_TexCoords).rgb;
    lighting += glow;
    
    // Output final color
    out_Color = vec4(lighting, diffuse.a);
}