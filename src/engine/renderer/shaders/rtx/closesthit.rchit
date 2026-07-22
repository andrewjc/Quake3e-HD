/*
===========================================================================
Copyright (C) 2024 Quake3e-HD Project

RTX Closest Hit Shader

Resolves the surface at the hit point: geometry attributes, per-triangle
material, textures. Returns pure surface data through the payload — all
lighting (light loop, shadow rays, ambient) happens in the ray generation
shader so no rays are ever traced from this stage.
===========================================================================
*/

#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_scalar_block_layout : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference2 : require

// Hit payload: surface description for the raygen lighting pass.
// color carries the surface emission term only.
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

// Intersection attributes
hitAttributeEXT vec2 baryCoords;

// Vertex data
struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 texCoord;
    vec3 tangent;
    vec4 color;
};

// Index buffer reference
layout(buffer_reference, scalar) buffer IndexBuffer {
    uvec3 indices[];
};

// Vertex buffer reference
layout(buffer_reference, scalar) buffer VertexBuffer {
    Vertex vertices[];
};

// Instance data — must match rtxInstanceGpuData_t (scalar layout)
struct InstanceData {
    uint64_t vertexBufferAddress;
    uint64_t indexBufferAddress;
    uint materialIndex;             // whole-instance fallback material
    uint triangleMaterialOffset;    // start of this instance's atlas range
    uint triangleMaterialCount;     // 0 = no per-triangle materials
    uint instanceFlags;
    mat4 normalMatrix;
    vec4 customData;
};

// Instance buffer
layout(binding = 10, set = 0, scalar) buffer InstanceDataBuffer {
    InstanceData instances[];
} instanceData;

// Material data
struct Material {
    vec4 albedo;
    vec4 specular;
    vec4 emission;
    float roughness;
    float metallic;
    float normalScale;
    float occlusionStrength;
    uint albedoTexture;
    uint normalTexture;
    uint roughnessTexture;
    uint metallicTexture;
    uint emissionTexture;
    uint occlusionTexture;
    uint lightmapTexture;
    uint flags;
    uint heightTexture;   // parallax-occlusion height/displacement map (0 = none)
    float heightScale;    // parallax depth as a fraction of UV span
};

// Material buffer
layout(binding = 11, set = 0, scalar) buffer MaterialBuffer {
    Material materials[];
} materialData;

// Texture array
layout(binding = 12, set = 0) uniform sampler2D textures[];

// Per-triangle material atlas: materialIndex per world triangle, addressed
// by instance.triangleMaterialOffset + gl_PrimitiveID
layout(binding = 20, set = 0, scalar) buffer TriangleMaterialBuffer {
    uint indices[];
} triangleMaterials;

// Debug settings uniform buffer
layout(binding = 18, set = 0) uniform DebugSettings {
    uint noTextures;     // 1 = disable textures (use grey)
    uint debugMode;      // Various debug visualization modes
    float debugOverlayBlend;
    uint debugFlags;
} debugSettings;

vec2 getTexCoord(vec3 barycentrics, vec2 uv0, vec2 uv1, vec2 uv2) {
    return uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
}

// Parallax occlusion mapping: march the height field against the tangent-space
// view direction so the surface reads with real depth (recessed mortar, raised
// brick, riveted plate) instead of a flat normal-mapped fake. Returns the
// parallax-shifted UV; all material maps are then sampled at that UV so the
// albedo, normal and AO all shift together. Ray-tracing shaders have no implicit
// derivatives, so height is fetched with textureLod(...,0).
vec2 parallaxUV(vec2 uv, vec3 viewT, uint heightTex, float scale) {
    // Steep-POM: more layers at grazing angles where parallax is strongest,
    // fewer looking head-on. viewT.z is the surface-facing cosine.
    float nSteps = mix(32.0, 8.0, clamp(viewT.z, 0.0, 1.0));
    float layer = 1.0 / nSteps;
    // Total UV shift toward the viewer for a full-depth texel, biased by the
    // view slope so steep angles shift more.
    vec2 dUV = (viewT.xy / max(viewT.z, 0.1)) * scale * layer;
    float curDepth = 0.0;
    vec2 curUV = uv;
    float h = 1.0 - textureLod(textures[nonuniformEXT(heightTex - 1u)], curUV, 0.0).r;
    for (int i = 0; i < 32; ++i) {
        if (float(i) >= nSteps || curDepth >= h) {
            break;
        }
        curUV -= dUV;
        h = 1.0 - textureLod(textures[nonuniformEXT(heightTex - 1u)], curUV, 0.0).r;
        curDepth += layer;
    }
    // Interpolate across the layer that first went below the height field so the
    // parallax edge is smooth instead of stair-stepped.
    vec2 prevUV = curUV + dUV;
    float afterD = h - curDepth;
    float beforeD = (1.0 - textureLod(textures[nonuniformEXT(heightTex - 1u)], prevUV, 0.0).r)
                    - (curDepth - layer);
    float w = afterD / (afterD - beforeD + 1e-5);
    return mix(curUV, prevUV, clamp(w, 0.0, 1.0));
}

vec3 applyNormalMap(vec3 normal, vec3 tangent, vec3 normalMapSample, float scale) {
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);
    vec3 mappedNormal = normalMapSample * 2.0 - 1.0;
    mappedNormal.xy *= scale;
    vec3 result = TBN * mappedNormal;
    // Degenerate samples (e.g. mid-gray texels from misidentified maps)
    // would normalize to NaN and poison every ray spawned from this hit
    if (dot(result, result) < 1e-8) {
        return normal;
    }
    return normalize(result);
}

void main() {
    // Get instance data
    InstanceData instance = instanceData.instances[gl_InstanceCustomIndexEXT];

    // Get vertex indices
    IndexBuffer indexBuffer = IndexBuffer(instance.indexBufferAddress);
    uvec3 indices = indexBuffer.indices[gl_PrimitiveID];

    // Get vertices
    VertexBuffer vertexBuffer = VertexBuffer(instance.vertexBufferAddress);
    Vertex v0 = vertexBuffer.vertices[indices.x];
    Vertex v1 = vertexBuffer.vertices[indices.y];
    Vertex v2 = vertexBuffer.vertices[indices.z];

    // Calculate barycentric coordinates
    vec3 barycentrics = vec3(1.0 - baryCoords.x - baryCoords.y, baryCoords.x, baryCoords.y);

    // Interpolate vertex attributes. Some surfaces carry zero normals or
    // tangents; a normalize(0) here becomes NaN, and a NaN direction later
    // hard-faults the traversal hardware — fall back to the geometric normal.
    vec3 normal = v0.normal * barycentrics.x + v1.normal * barycentrics.y + v2.normal * barycentrics.z;
    if (dot(normal, normal) < 1e-6) {
        normal = cross(v1.position - v0.position, v2.position - v0.position);
        if (dot(normal, normal) < 1e-12) {
            normal = -gl_WorldRayDirectionEXT;
        }
    }
    normal = normalize(normal);
    vec2 texCoord = getTexCoord(barycentrics, v0.texCoord, v1.texCoord, v2.texCoord);
    vec3 tangent = v0.tangent * barycentrics.x + v1.tangent * barycentrics.y + v2.tangent * barycentrics.z;
    bool tangentValid = dot(tangent, tangent) > 1e-6;
    if (tangentValid) {
        tangent = normalize(tangent);
    }

    // Transform to world space
    vec3 worldPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    vec3 worldNormal = normalize(mat3(instance.normalMatrix) * normal);
    vec3 worldTangent = tangentValid ? normalize(mat3(instance.normalMatrix) * tangent) : vec3(0.0);

    // Flip the normal toward the incoming ray so backfaces shade correctly
    // (facing cull is disabled for world geometry).
    if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0) {
        worldNormal = -worldNormal;
    }

    // Resolve the material: world batches carry one material per triangle in
    // the atlas; other instances use the whole-instance fallback. Atlas bit
    // 31 marks underwater triangles (caustic receivers).
    uint materialIndex = instance.materialIndex;
    bool underwater = false;
    if (instance.triangleMaterialCount > 0u && gl_PrimitiveID < instance.triangleMaterialCount) {
        uint atlasEntry = triangleMaterials.indices[instance.triangleMaterialOffset + gl_PrimitiveID];
        materialIndex = atlasEntry & 0x7FFFFFFFu;
        underwater = (atlasEntry & 0x80000000u) != 0u;
    }
    Material mat = materialData.materials[materialIndex];

    // Parallax occlusion mapping (real surface depth). Uses the geometric
    // surface frame (before normal mapping) and the incoming ray as the view
    // direction; only runs when a height map is present (loaded only while
    // rt_parallax is on) and the tangent basis is valid. All subsequent map
    // samples use the shifted UV.
    if (mat.heightTexture != 0u && tangentValid && debugSettings.noTextures == 0
        && (debugSettings.debugFlags & 1u) != 0u) {
        vec3 bitangentG = cross(worldNormal, worldTangent);
        vec3 viewDir = -gl_WorldRayDirectionEXT;
        vec3 viewT = vec3(dot(viewDir, worldTangent),
                          dot(viewDir, bitangentG),
                          dot(viewDir, worldNormal));
        if (viewT.z > 0.05) {
            texCoord = parallaxUV(texCoord, viewT, mat.heightTexture, mat.heightScale);
        }
    }

    // Sample textures. NOTE: BSP vertex colors carry the legacy baked
    // vertex lighting — the path tracer computes its own lighting, so they
    // must not modulate the material albedo.
    vec3 albedo = mat.albedo.rgb;
    if (mat.albedoTexture != 0 && debugSettings.noTextures == 0) {
        albedo *= texture(textures[nonuniformEXT(mat.albedoTexture - 1)], texCoord).rgb;
    } else if (debugSettings.noTextures != 0) {
        // Use default grey color when textures are disabled for debugging
        albedo = vec3(0.5, 0.5, 0.5);
    }
    // Game textures are gamma-encoded; lighting must run on linear albedo or
    // the final display encode double-brightens and washes out the image
    albedo = pow(max(albedo, vec3(0.0)), vec3(2.2));

    // Normal mapping (requires a valid tangent basis)
    if (mat.normalTexture != 0 && debugSettings.noTextures == 0 && tangentValid) {
        vec3 normalMap = texture(textures[nonuniformEXT(mat.normalTexture - 1)], texCoord).rgb;
        worldNormal = applyNormalMap(worldNormal, worldTangent, normalMap, mat.normalScale);
    }

    // PBR parameters
    float roughness = mat.roughness;
    if (mat.roughnessTexture != 0 && debugSettings.noTextures == 0) {
        roughness *= texture(textures[nonuniformEXT(mat.roughnessTexture - 1)], texCoord).r;
    } else if (debugSettings.noTextures != 0) {
        roughness = 0.5;
    }

    float metallic = mat.metallic;
    if (mat.metallicTexture != 0 && debugSettings.noTextures == 0) {
        metallic *= texture(textures[nonuniformEXT(mat.metallicTexture - 1)], texCoord).r;
    } else if (debugSettings.noTextures != 0) {
        metallic = 0.0;
    }

    // Ambient occlusion folded into the albedo the lighting pass consumes
    float ao = 1.0;
    if (mat.occlusionTexture != 0 && debugSettings.noTextures == 0) {
        ao = texture(textures[nonuniformEXT(mat.occlusionTexture - 1)], texCoord).r;
        ao = mix(1.0, ao, mat.occlusionStrength);
    }

    // Emission (linearized like the albedo)
    vec3 emission = mat.emission.rgb * mat.emission.a;
    if (mat.emissionTexture != 0 && debugSettings.noTextures == 0) {
        vec3 emitTex = texture(textures[nonuniformEXT(mat.emissionTexture - 1)], texCoord).rgb;
        emission *= pow(max(emitTex, vec3(0.0)), vec3(2.2));
    } else if (debugSettings.noTextures != 0) {
        emission = vec3(0.0);
    }

    // Store surface information; lighting happens in raygen. materialID bit
    // 31 carries the underwater (caustic receiver) flag, bit 30 marks a
    // refractive surface (water / glass) for the raygen reflect+refract path.
    // MATERIAL_FLAG_WATER = 1<<4, MATERIAL_FLAG_GLASS = 1<<5.
    uint refractiveBit = ((mat.flags & ((1u << 4) | (1u << 5))) != 0u) ? 0x40000000u : 0u;
    hitInfo.color = emission;
    hitInfo.distance = gl_HitTEXT;
    hitInfo.normal = worldNormal;
    hitInfo.roughness = roughness;
    hitInfo.albedo = albedo * ao;
    hitInfo.metallic = metallic;
    hitInfo.worldPos = worldPos;
    hitInfo.materialID = materialIndex | (underwater ? 0x80000000u : 0u) | refractiveBit;
    hitInfo.uv = texCoord;
    hitInfo.primitiveID = gl_PrimitiveID;
    hitInfo.instanceID = gl_InstanceCustomIndexEXT;
    hitInfo.hitType = 1; // Hit
}
