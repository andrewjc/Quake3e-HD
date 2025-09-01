/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

Common includes and functions for RTX shaders
===========================================================================
*/

#ifndef COMMON_HLSLI
#define COMMON_HLSLI

// Vertex data structures
struct Vertex {
    float3 position;
    float3 normal;
    float2 texCoord;
    float3 tangent;
    float3 bitangent;
};

// Material properties
struct Material {
    float3 albedo;
    float  metallic;
    float  roughness;
    float  ao;
    float3 emissive;
    uint   textureFlags;
};

// Light structures
struct PointLight {
    float3 position;
    float  radius;
    float3 color;
    float  intensity;
};

struct DirectionalLight {
    float3 direction;
    float  intensity;
    float3 color;
    float  shadowBias;
};

struct SpotLight {
    float3 position;
    float  range;
    float3 direction;
    float  angle;
    float3 color;
    float  intensity;
};

// Structured buffers for scene data
StructuredBuffer<Vertex> g_Vertices : register(t1);
StructuredBuffer<uint> g_Indices : register(t2);
StructuredBuffer<Material> g_Materials : register(t3);
StructuredBuffer<PointLight> g_PointLights : register(t4);
StructuredBuffer<DirectionalLight> g_DirectionalLights : register(t5);
StructuredBuffer<SpotLight> g_SpotLights : register(t6);

Texture2D g_AlbedoTextures[] : register(t10);
Texture2D g_NormalTextures[] : register(t50);
Texture2D g_MetallicRoughnessTextures[] : register(t90);
SamplerState g_LinearSampler : register(s0);

// Instance data buffer
struct InstanceData {
    float4x4 transform;
    float4x4 normalTransform;
    uint materialID;
    uint vertexOffset;
    uint indexOffset;
    uint pad;
};

StructuredBuffer<InstanceData> g_InstanceData : register(t7);

// Helper functions
float3 GetVertexPosition(uint instanceID, uint primitiveID, float2 barycentrics) {
    InstanceData instance = g_InstanceData[instanceID];
    
    uint i0 = g_Indices[instance.indexOffset + primitiveID * 3 + 0];
    uint i1 = g_Indices[instance.indexOffset + primitiveID * 3 + 1];
    uint i2 = g_Indices[instance.indexOffset + primitiveID * 3 + 2];
    
    float3 v0 = g_Vertices[instance.vertexOffset + i0].position;
    float3 v1 = g_Vertices[instance.vertexOffset + i1].position;
    float3 v2 = g_Vertices[instance.vertexOffset + i2].position;
    
    float3 localPos = v0 * (1.0f - barycentrics.x - barycentrics.y) +
                      v1 * barycentrics.x +
                      v2 * barycentrics.y;
    
    return mul(instance.transform, float4(localPos, 1.0f)).xyz;
}

float3 GetVertexNormal(uint instanceID, uint primitiveID, float2 barycentrics) {
    InstanceData instance = g_InstanceData[instanceID];
    
    uint i0 = g_Indices[instance.indexOffset + primitiveID * 3 + 0];
    uint i1 = g_Indices[instance.indexOffset + primitiveID * 3 + 1];
    uint i2 = g_Indices[instance.indexOffset + primitiveID * 3 + 2];
    
    float3 n0 = g_Vertices[instance.vertexOffset + i0].normal;
    float3 n1 = g_Vertices[instance.vertexOffset + i1].normal;
    float3 n2 = g_Vertices[instance.vertexOffset + i2].normal;
    
    float3 localNormal = n0 * (1.0f - barycentrics.x - barycentrics.y) +
                         n1 * barycentrics.x +
                         n2 * barycentrics.y;
    
    return normalize(mul(instance.normalTransform, float4(localNormal, 0.0f)).xyz);
}

float2 GetVertexTexCoord(uint instanceID, uint primitiveID, float2 barycentrics) {
    InstanceData instance = g_InstanceData[instanceID];
    
    uint i0 = g_Indices[instance.indexOffset + primitiveID * 3 + 0];
    uint i1 = g_Indices[instance.indexOffset + primitiveID * 3 + 1];
    uint i2 = g_Indices[instance.indexOffset + primitiveID * 3 + 2];
    
    float2 uv0 = g_Vertices[instance.vertexOffset + i0].texCoord;
    float2 uv1 = g_Vertices[instance.vertexOffset + i1].texCoord;
    float2 uv2 = g_Vertices[instance.vertexOffset + i2].texCoord;
    
    return uv0 * (1.0f - barycentrics.x - barycentrics.y) +
           uv1 * barycentrics.x +
           uv2 * barycentrics.y;
}

float3 GetAlbedo(uint instanceID, float2 texCoord) {
    InstanceData instance = g_InstanceData[instanceID];
    Material material = g_Materials[instance.materialID];
    
    if (material.textureFlags & 0x1) {
        // Has albedo texture
        uint textureIndex = instance.materialID;
        return g_AlbedoTextures[textureIndex].SampleLevel(g_LinearSampler, texCoord, 0).rgb;
    }
    
    return material.albedo;
}

float3 GetNormalFromMap(uint instanceID, float2 texCoord, float3 worldNormal, float3 worldTangent, float3 worldBitangent) {
    InstanceData instance = g_InstanceData[instanceID];
    Material material = g_Materials[instance.materialID];
    
    if (material.textureFlags & 0x2) {
        // Has normal map
        uint textureIndex = instance.materialID;
        float3 normalMapValue = g_NormalTextures[textureIndex].SampleLevel(g_LinearSampler, texCoord, 0).rgb;
        normalMapValue = normalMapValue * 2.0f - 1.0f;
        
        // Transform from tangent space to world space
        float3x3 TBN = float3x3(worldTangent, worldBitangent, worldNormal);
        return normalize(mul(normalMapValue, TBN));
    }
    
    return worldNormal;
}

// Lighting calculations
float3 CalculateDirectLighting(float3 position, float3 normal, float3 albedo) {
    float3 lighting = float3(0, 0, 0);
    
    // Directional lights
    uint numDirLights, stride;
    g_DirectionalLights.GetDimensions(numDirLights, stride);
    
    for (uint i = 0; i < numDirLights; i++) {
        DirectionalLight light = g_DirectionalLights[i];
        float NdotL = max(0.0f, dot(normal, -light.direction));
        
        // Shadow ray
        if (NdotL > 0.0f) {
            RayDesc shadowRay;
            shadowRay.Origin = position + normal * 0.001f;
            shadowRay.Direction = -light.direction;
            shadowRay.TMin = 0.001f;
            shadowRay.TMax = 10000.0f;
            
            // Simple shadow query
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadowQuery;
            shadowQuery.TraceRayInline(
                g_SceneTLAS,
                RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                0xFF,
                shadowRay
            );
            
            shadowQuery.Proceed();
            
            if (shadowQuery.CommittedStatus() == COMMITTED_NOTHING) {
                lighting += albedo * light.color * light.intensity * NdotL;
            }
        }
    }
    
    // Point lights
    uint numPointLights;
    g_PointLights.GetDimensions(numPointLights, stride);
    
    for (uint j = 0; j < numPointLights; j++) {
        PointLight light = g_PointLights[j];
        float3 lightVec = light.position - position;
        float distance = length(lightVec);
        
        if (distance < light.radius) {
            float3 lightDir = lightVec / distance;
            float NdotL = max(0.0f, dot(normal, lightDir));
            
            if (NdotL > 0.0f) {
                // Shadow ray
                RayDesc shadowRay;
                shadowRay.Origin = position + normal * 0.001f;
                shadowRay.Direction = lightDir;
                shadowRay.TMin = 0.001f;
                shadowRay.TMax = distance;
                
                RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadowQuery;
                shadowQuery.TraceRayInline(
                    g_SceneTLAS,
                    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                    0xFF,
                    shadowRay
                );
                
                shadowQuery.Proceed();
                
                if (shadowQuery.CommittedStatus() == COMMITTED_NOTHING) {
                    float attenuation = 1.0f / (1.0f + 0.09f * distance + 0.032f * distance * distance);
                    float falloff = saturate(1.0f - (distance / light.radius));
                    lighting += albedo * light.color * light.intensity * NdotL * attenuation * falloff;
                }
            }
        }
    }
    
    return lighting;
}

// BRDF functions for PBR
float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float DistributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0f);
    float NdotH2 = NdotH * NdotH;
    
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = 3.14159265f * denom * denom;
    
    return num / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0f);
    float k = (r * r) / 8.0f;
    
    float num = NdotV;
    float denom = NdotV * (1.0f - k) + k;
    
    return num / denom;
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    
    return ggx1 * ggx2;
}

float3 CalculatePBR(float3 albedo, float metallic, float roughness, float3 N, float3 V, float3 L, float3 radiance) {
    float3 H = normalize(V + L);
    
    float3 F0 = float3(0.04f, 0.04f, 0.04f);
    F0 = lerp(F0, albedo, metallic);
    
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);
    
    float3 kS = F;
    float3 kD = float3(1.0f, 1.0f, 1.0f) - kS;
    kD *= 1.0f - metallic;
    
    float3 numerator = NDF * G * F;
    float denominator = 4.0f * max(dot(N, V), 0.0f) * max(dot(N, L), 0.0f) + 0.0001f;
    float3 specular = numerator / denominator;
    
    float NdotL = max(dot(N, L), 0.0f);
    return (kD * albedo / 3.14159265f + specular) * radiance * NdotL;
}

#endif // COMMON_HLSLI