/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

RTX Miss Shader
Handles rays that miss all geometry
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : enable

// Ray payload for primary rays
layout(location = 0) rayPayloadInEXT struct HitInfo {
    vec3 color;
    float distance;
    vec3 normal;
    float roughness;
    vec3 albedo;
    float metallic;
    vec3 worldPos;
    uint materialID;
    vec2 uv;
    uint primitiveID;
    uint instanceID;
    uint hitType;
} hitInfo;

layout(binding = 8, set = 0) uniform sampler2D environmentMap;
layout(binding = 9, set = 0) uniform EnvironmentData {
    vec3 sunDirection;
    float sunIntensity;
    vec3 sunColor;
    float skyIntensity;
    vec3 fogColor;
    float fogDensity;
    float fogStart;
    float fogEnd;
    uint useEnvironmentMap;
    uint useProceduralSky;
    float time;
    float cloudCoverage;
} env;

// Procedural sky generation
vec3 proceduralSky(vec3 direction) {
    float sunAmount = max(dot(direction, env.sunDirection), 0.0);
    
    // Sky gradient
    float skyGradient = pow(max(direction.y, 0.0), 0.4);
    vec3 skyColor = mix(vec3(0.6, 0.7, 0.9), vec3(0.1, 0.3, 0.7), skyGradient);
    
    // Sun disc
    float sunDisc = smoothstep(0.9995, 0.9999, sunAmount);
    vec3 sun = env.sunColor * sunDisc * env.sunIntensity;
    
    // Atmospheric scattering approximation
    float scatter = pow(sunAmount, 8.0) * 0.5;
    skyColor += env.sunColor * scatter;
    
    // Simple cloud layer
    if (env.cloudCoverage > 0.0) {
        float cloudHeight = 0.3;
        if (direction.y > cloudHeight) {
            float cloudFactor = smoothstep(cloudHeight, cloudHeight + 0.1, direction.y);
            float cloudNoise = sin(direction.x * 10.0 + env.time) * 
                              cos(direction.z * 10.0 + env.time * 0.5);
            cloudNoise = smoothstep(-1.0, 1.0, cloudNoise) * env.cloudCoverage;
            skyColor = mix(skyColor, vec3(1.0, 1.0, 1.0), cloudNoise * cloudFactor);
        }
    }
    
    return skyColor + sun;
}

// Equirectangular environment map sampling
vec3 sampleEnvironmentMap(vec3 direction) {
    // Convert direction to UV coordinates
    float theta = acos(direction.y);
    float phi = atan(direction.z, direction.x);
    vec2 uv = vec2(phi / (2.0 * 3.14159265359) + 0.5, theta / 3.14159265359);
    
    return texture(environmentMap, uv).rgb * env.skyIntensity;
}

void main() {
    vec3 direction = gl_WorldRayDirectionEXT;
    vec3 color;
    
    if (env.useEnvironmentMap > 0) {
        color = sampleEnvironmentMap(direction);
    } else if (env.useProceduralSky > 0) {
        color = proceduralSky(direction);
    } else {
        // Simple gradient sky fallback
        float t = 0.5 * (direction.y + 1.0);
        color = mix(vec3(0.8, 0.85, 1.0), vec3(0.4, 0.6, 1.0), t);
        
        // Add sun
        float sunAmount = max(dot(direction, env.sunDirection), 0.0);
        color += env.sunColor * pow(sunAmount, 256.0) * env.sunIntensity;
    }
    
    // Apply fog if we're looking near the horizon
    if (env.fogDensity > 0.0 && direction.y < 0.2) {
        float fogFactor = exp(-direction.y * 10.0 * env.fogDensity);
        color = mix(color, env.fogColor, fogFactor);
    }
    
    hitInfo.color = color;
    hitInfo.distance = -1.0;
    hitInfo.hitType = 0; // Miss
    hitInfo.albedo = color;
    hitInfo.normal = -direction;
}