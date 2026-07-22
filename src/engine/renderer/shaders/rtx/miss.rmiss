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
    vec3 tangent;
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
    vec4 skyColor;      // rgb = map sky average color, a = brightness
} env;

// Value-noise FBM for the animated cloud layer
float skyHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float skyValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);

    float a = skyHash(i);
    float b = skyHash(i + vec2(1.0, 0.0));
    float c = skyHash(i + vec2(0.0, 1.0));
    float d = skyHash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float skyFbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        v += amp * skyValueNoise(p);
        p = p * 2.03 + vec2(17.3, 9.1);
        amp *= 0.5;
    }
    return v;
}

// Procedural sky built from the map's averaged sky color (Quake is Z-up),
// with an FBM cloud deck drifting over time.
vec3 proceduralSky(vec3 direction) {
    float sunAmount = max(dot(direction, env.sunDirection), 0.0);

    // Gradient: map sky color at the horizon, slightly darker at zenith
    vec3 base = env.skyColor.rgb * env.skyColor.a;
    float skyGradient = pow(max(direction.z, 0.0), 0.4);
    vec3 skyColor = mix(base, base * 0.6, skyGradient);

    // Sun disc
    float sunDisc = smoothstep(0.9995, 0.9999, sunAmount);
    vec3 sun = env.sunColor * sunDisc * env.sunIntensity;

    // Atmospheric scattering approximation
    float scatter = pow(sunAmount, 8.0) * 0.35;
    skyColor += env.sunColor * scatter;

    // Drifting cloud deck projected onto a virtual plane above the horizon
    if (env.cloudCoverage > 0.0 && direction.z > 0.04) {
        vec2 planeUV = direction.xy / max(direction.z, 0.08);
        vec2 wind = vec2(0.014, 0.006) * env.time;
        float d = skyFbm(planeUV * 0.55 + wind);

        // Coverage remaps the noise so higher coverage grows the clouds
        float cloud = smoothstep(1.0 - env.cloudCoverage, 1.0 - env.cloudCoverage + 0.28, d);

        // Fade toward the horizon where the plane projection stretches
        cloud *= smoothstep(0.04, 0.22, direction.z);

        // Sun-lit tops with a hint of silver lining near the sun
        vec3 cloudColor = base * 1.35 + env.sunColor * (0.18 + 0.5 * pow(sunAmount, 4.0));
        skyColor = mix(skyColor, cloudColor, cloud * 0.85);
    }

    return skyColor + sun;
}

// Equirectangular environment map sampling (Z-up)
vec3 sampleEnvironmentMap(vec3 direction) {
    // Convert direction to UV coordinates
    float theta = acos(clamp(direction.z, -1.0, 1.0));
    float phi = atan(direction.y, direction.x);
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
        // Simple gradient sky fallback from the map sky color (Z-up)
        vec3 base = env.skyColor.rgb * env.skyColor.a;
        float t = 0.5 * (direction.z + 1.0);
        color = mix(base, base * 0.7, t);

        // Add sun
        float sunAmount = max(dot(direction, env.sunDirection), 0.0);
        color += env.sunColor * pow(sunAmount, 256.0) * env.sunIntensity;
    }

    // Apply fog if we're looking near the horizon (Z-up)
    if (env.fogDensity > 0.0 && direction.z < 0.2) {
        float fogFactor = exp(-direction.z * 10.0 * env.fogDensity);
        color = mix(color, env.fogColor, fogFactor);
    }
    
    // The lighting pipeline is linear; the sky color is authored in gamma
    // space, so linearize it before it enters the radiance accumulator.
    color = pow(max(color, vec3(0.0)), vec3(2.2));

    hitInfo.color = color;
    hitInfo.distance = -1.0;
    hitInfo.hitType = 0; // Miss
    hitInfo.albedo = color;
    hitInfo.normal = -direction;
}