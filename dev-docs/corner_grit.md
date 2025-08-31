Overview
Goal: Make wall–floor joins look natural by blending the two materials with a procedural dirt/grit mask and faking a shallow curved fillet at the corner. This is implemented as a thin “seam overlay” mesh rendered on top of existing geometry with a dedicated Vulkan pipeline and a pair of shaders. It requires no changes to existing wall/floor surfaces and works with lightmapped Quake renderers.

What you get:
- Smooth material transition from wall texture to floor texture
- Subtle curved corner (bevel) via normal blending and optional micro displacement
- Procedural dirt/grit concentrated along the join
- Compatible with Quake-style planar texture mapping and lightmaps

The rest of this document is an implementation guide targeted at another AI/engineer to implement the feature.

Implementation guide
1) Technique summary
- Build a thin “seam strip” mesh centered along any edge where two faces meet at roughly a right angle.
- Each strip carries the two face normals, the two sets of planar UVs (wall and floor), and a parameter t in [0, 1] spanning from wall side (t=0) to floor side (t=1).
- In the fragment shader:
  - Blend wall/floor base textures using a smoothed transition with a small noise dither to kill banding.
  - Compute a “curved” corner normal by slerp-like interpolation between the wall and floor normals as t goes 0→1 (visually like a quarter-round fillet).
  - Add procedural dirt/grit concentrated near the middle of the seam; darken/roughen as desired.
  - If you have lightmaps, sample both faces’ lightmaps and blend them by the same factor t, or fall back to a simple directional light if no lightmaps.
- Render order: draw the seam overlay after base world geometry with a tiny depth bias to avoid z-fighting.

2) Geometry generation (CPU, at BSP/map load)
2.1 Detect candidate edges
- Parse faces and their edges.
- For each undirected edge shared by two faces F0 and F1:
  - Compute face normals n0, n1 (normalized).
  - If angle between them is within [60°, 120°], mark as candidate seam (a “corner”).
  - Optionally, filter to only wall–floor pairs if you want: treat floor/ceiling as faces with |dot(n, UP)| ≥ 0.7 and walls as |dot(n, UP)| < 0.3.

2.2 Build seam strips per edge
- For each candidate edge with endpoints P0, P1:
  - Let e = normalize(P1 − P0).
  - For each face i in {0,1} with normal n_i:
    - Compute an in-plane direction pointing “into” that face: d_i = normalize(cross(e, n_i)).
  - Choose a world-space seam width r (typical: 3–8 units for Quake scale).
  - Create a 2-triangle strip by offsetting along the two faces:
    - Wall-side vertices: V0w = P0 + d0*r, V1w = P1 + d0*r (t=0)
    - Floor-side vertices: V0f = P0 + d1*r, V1f = P1 + d1*r (t=1)
  - Attributes per-vertex:
    - position: vec3 (world space)
    - nWall: vec3 (n0)
    - nFloor: vec3 (n1)
    - uvWall: vec2 (planar UV using face F0’s texinfo at the vertex position)
    - uvFloor: vec2 (planar UV using face F1’s texinfo at the vertex position)
    - t: float (0.0 for wall-side vertices, 1.0 for floor-side vertices)
    - Optional: lmUVWall, lmUVFloor (if your renderer uses lightmap atlases per-face)
  - To compute planar UVs from Quake texinfo:
    - uv = vec2(dot(P, S.xyz) + S.w, dot(P, T.xyz) + T.w), where S and T are the face’s texture axes and offsets in world units.
  - To compute lightmap UVs: reuse existing engine code that bakes per-face into a lightmap atlas; fetch the per-face lightmap transform and compute lmUV the same way it’s done for the base face vertices.

2.3 Build buffers
- Aggregate all seam strips into one or a few static vertex/index buffers.
- Optional: batch by (wallTex, floorTex, wallLM, floorLM) to reduce descriptor rebinds.

3) Vulkan pipeline additions
3.1 New descriptor set layout
- Set 0: camera (reused from your world pipeline)
  - binding 0: Camera UBO with viewProj and optionally eye position
- Set 1: per-draw textures
  - binding 0: wall albedo (combined image sampler, sRGB)
  - binding 1: floor albedo (combined image sampler, sRGB)
  - binding 2: wall lightmap (combined image sampler, linear) [optional]
  - binding 3: floor lightmap (combined image sampler, linear) [optional]
- Push constants:
  - depthBias: float (tiny world-space push to defeat z-fighting)
  - noiseScale: float (world-space scale for grit)
  - dirtIntensity: float [0..1]
  - blendCenter: float (default 0.5)
  - blendWidth: float (e.g., 0.12)
  - noiseAmplitude: float (e.g., 0.05)
  - reserved floats…

3.2 Pipeline state
- Depth test: enable, compare = less-or-equal
- Depth write: disable (transparent overlay on top of existing surfaces)
- Color blending: disable (we fully cover the strip area), or enable alpha=1.0 anyway
- Rasterizer: polygon offset OR use depthBias push in vertex shader
- Culling: back-face cull (construction should yield consistent winding), or disable if geometry is paper-thin
- Vertex input: see shader locations below
- sRGB aware sampling for albedo; lightmaps stay linear

4) Shaders
Note: #version 450 core, GLSL compiled to SPIR-V. Toggle USE_LIGHTMAP if you wire lightmaps.

bevel_seam.vert
```glsl
#version 450

layout(set = 0, binding = 0) uniform Camera {
    mat4 uViewProj;
} uCam;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNWall;
layout(location = 2) in vec3 inNFloor;
layout(location = 3) in vec2 inUVWall;
layout(location = 4) in vec2 inUVFloor;
layout(location = 5) in float inT;          // 0 = wall side, 1 = floor side
layout(location = 6) in vec2 inLMUVWall;    // optional, when USE_LIGHTMAP
layout(location = 7) in vec2 inLMUVFloor;   // optional, when USE_LIGHTMAP

layout(push_constant) uniform PC {
    float depthBias;     // small world-space push along blended normal, e.g., 0.2
    float pad0, pad1, pad2;
} pc;

layout(location = 0) out vec2 vUVWall;
layout(location = 1) out vec2 vUVFloor;
layout(location = 2) out vec3 vNWall;
layout(location = 3) out vec3 vNFloor;
layout(location = 4) out vec3 vWorldPos;
layout(location = 5) out float vT;
layout(location = 6) out vec2 vLMUVWall;
layout(location = 7) out vec2 vLMUVFloor;

void main() {
    vec3 nW = normalize(inNWall);
    vec3 nF = normalize(inNFloor);

    // Angle between face normals (can be ~90 deg but not assumed)
    float ang = acos(clamp(dot(nW, nF), -1.0, 1.0));
    float theta = clamp(inT, 0.0, 1.0) * ang;

    // Approx spherical interpolation for the bevel normal
    vec3 nBlend = normalize(nW * cos(theta) + nF * sin(theta));

    // Push slightly along the blended normal to eliminate z-fighting
    vec3 worldPos = inPos + nBlend * pc.depthBias;

    vUVWall   = inUVWall;
    vUVFloor  = inUVFloor;
    vNWall    = nW;
    vNFloor   = nF;
    vWorldPos = worldPos;
    vT        = inT;
    vLMUVWall   = inLMUVWall;
    vLMUVFloor  = inLMUVFloor;

    gl_Position = uCam.uViewProj * vec4(worldPos, 1.0);
}
```

bevel_seam.frag
```glsl
#version 450

layout(set = 1, binding = 0) uniform sampler2D uWallAlbedo;
layout(set = 1, binding = 1) uniform sampler2D uFloorAlbedo;

#ifdef USE_LIGHTMAP
layout(set = 1, binding = 2) uniform sampler2D uWallLightmap;
layout(set = 1, binding = 3) uniform sampler2D uFloorLightmap;
#endif

layout(location = 0) in vec2 vUVWall;
layout(location = 1) in vec2 vUVFloor;
layout(location = 2) in vec3 vNWall;
layout(location = 3) in vec3 vNFloor;
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) in float vT;
layout(location = 6) in vec2 vLMUVWall;
layout(location = 7) in vec2 vLMUVFloor;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PCF {
    float noiseScale;       // e.g., 0.35
    float dirtIntensity;    // 0..1
    float blendCenter;      // default 0.5
    float blendWidth;       // e.g., 0.12
    float noiseAmplitude;   // e.g., 0.06 (blend dither)
    float unused0, unused1, unused2;
} pcf;

// Simple 3D value noise + fBm for procedural grit
float hash31(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

float noise3(vec3 x) {
    vec3 p = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(p + vec3(0,0,0));
    float n100 = hash31(p + vec3(1,0,0));
    float n010 = hash31(p + vec3(0,1,0));
    float n110 = hash31(p + vec3(1,1,0));
    float n001 = hash31(p + vec3(0,0,1));
    float n101 = hash31(p + vec3(1,0,1));
    float n011 = hash31(p + vec3(0,1,1));
    float n111 = hash31(p + vec3(1,1,1));
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float fbm(vec3 p) {
    float a = 0.5;
    float s = 0.0;
    for (int i = 0; i < 4; ++i) {
        s += a * noise3(p);
        p *= 2.03;
        a *= 0.5;
    }
    return s;
}

void main() {
    vec3 nW = normalize(vNWall);
    vec3 nF = normalize(vNFloor);

    // Curved bevel normal
    float ang = acos(clamp(dot(nW, nF), -1.0, 1.0));
    float theta = clamp(vT, 0.0, 1.0) * ang;
    vec3 nBevel = normalize(nW * cos(theta) + nF * sin(theta));

    // Blend factor between wall and floor with noise dither
    float s = clamp(vT, 0.0, 1.0);
    float e0 = pcf.blendCenter - pcf.blendWidth;
    float e1 = pcf.blendCenter + pcf.blendWidth;

    float n = noise3(vWorldPos * pcf.noiseScale) * 2.0 - 1.0;
    float t = smoothstep(e0, e1, s + pcf.noiseAmplitude * n);

    vec3 cWall  = texture(uWallAlbedo,  vUVWall).rgb;
    vec3 cFloor = texture(uFloorAlbedo, vUVFloor).rgb;
    vec3 albedo = mix(cWall, cFloor, t);

    // Procedural dirt/grit concentrated near the seam center
    float prox = 1.0 - abs(2.0 * s - 1.0);      // 0 at edges, 1 at center
    float grit = fbm(vWorldPos * (pcf.noiseScale * 1.35));
    float dirt = clamp((grit * 0.7 + 0.3) * pow(prox, 1.5), 0.0, 1.0);
    albedo *= 1.0 - dirt * pcf.dirtIntensity;

    // Lighting
    vec3 lit;
    #ifdef USE_LIGHTMAP
        vec3 Lw = texture(uWallLightmap,  vLMUVWall).rgb;
        vec3 Lf = texture(uFloorLightmap, vLMUVFloor).rgb;
        vec3 L  = mix(Lw, Lf, t);
        lit = albedo * L;    // Quake lightmaps are usually linear intensity
    #else
        // Fallback simple lambert
        vec3 lightDir = normalize(vec3(0.35, 0.75, 0.55));
        float ndotl = max(dot(nBevel, lightDir), 0.0);
        lit = albedo * (0.2 + 0.8 * ndotl);
    #endif

    outColor = vec4(lit, 1.0);
}
```

5) Engine wiring details
5.1 Data structures (CPU)
- For each seam vertex, pack:
  - vec3 pos
  - vec3 nWall
  - vec3 nFloor
  - vec2 uvWall
  - vec2 uvFloor
  - float t
  - vec2 lmUVWall (optional)
  - vec2 lmUVFloor (optional)
- Keep an index buffer with two triangles per edge segment.

5.2 Map loader changes (pseudo)
- Build adjacency:
  - Map<SortedEdge(vA,vB), vector<faceId>> edgeToFaces
- For each entry with faces.size()==2:
  - F0=faces[0], F1=faces[1]
  - n0, n1 = normalized plane normals
  - if angle in [60°,120°], build seam strip with width r
  - Compute uvWall from F0’s texinfo at vertex positions; uvFloor from F1’s texinfo
  - If using lightmaps, compute lmUV* via the engine’s lightmap atlas mapping

5.3 Rendering
- Build a new graphics pipeline with the above shaders.
- Record draw:
  - Sort/batch by material pair (wallTex,floorTex[,wallLM,floorLM]) to reduce descriptor switches.
  - For each batch:
    - Bind pipeline and common set 0 with camera UBO
    - Bind set 1 with the two albedos (and two LMs if enabled)
    - Push constants:
      - depthBias = 0.1–0.3 world units
      - noiseScale = 0.25–0.5 (tune)
      - dirtIntensity = 0.2–0.6 (tune per style)
      - blendCenter = 0.5
      - blendWidth = 0.10–0.15
      - noiseAmplitude = 0.03–0.08
    - Bind seam vertex/index buffers and draw
- Render order: after opaque world geometry, before transparent sprites/particles. Depth test on, depth write off.

5.4 sRGB and formats
- Ensure albedo textures use sRGB formats and are sampled with sRGB decode enabled.
- Lightmaps are linear; keep them in linear format/sampler.

5.5 Avoiding z-fighting
- Use both: a tiny depthBias in vertex push (pc.depthBias ≈ 0.2) and polygon offset in the pipeline (optional).
- Alternatively, nudge positions along camera view direction via gl_FragDepth offset – not recommended; stick to vertex push or poly offset.

6) Tuning tips
- Width r: 3–8 world units depending on map scale and texture resolution.
- Dirt intensity: 0.3 looks natural; increase toward grungy style.
- Noise scale: linked to texture density; start at about 1 noise cell per 2–3 world units.
- Blend width: 0.10–0.15 creates a visible but tight transition; widen if you want more smear.
- If you want more curvature, you can subdivide the strip across t (e.g., add a center row with t=0.5) to let the vertex shader push the midline slightly along nBevel for a micro “geo” fillet:
  - In VS, for t in (0,1), add small displacement: worldPos += nBevel * bevelAmount, where bevelAmount ~ r * 0.15.
  - Keep small to avoid gaps with base faces.

7) Integration with Quake lightmaps
- If your engine provides per-face lightmap UVs, pass both sets on the seam vertices and bind both lightmap textures for the batch; the fragment shader blends them by t.
- If each face’s lightmap lives in a large atlas, seam vertices should carry lmUV computed from the correct per-face atlas transform.
- If wiring two lightmaps per draw is too costly, you can:
  - Sample only the lightmap of the “dominant” side (choose by s < 0.5 ? wall : floor). This is cheaper but may show a slight lighting mismatch exactly at the centerline.

8) Optimization options
- Batch seams by material pairs; on typical Q1/Q2 maps, the number of distinct pairs near walkable edges isn’t huge.
- Replace in-shader fbm with a precomputed 3D noise texture (RGBA8) and sample it; reduces ALU.
- Use descriptor indexing/bindless for materials if your renderer already supports VK_EXT_descriptor_indexing, otherwise stick to simple rebinds per batch.

9) Testing checklist
- No z-fighting or flicker at camera oblique angles.
- No lightmap seams: ensure lmUV mapping matches base faces.
- Correct sRGB/lightmap handling: toggling the overlay should preserve overall scene brightness.
- Performance: Overdraw is limited to the narrow seam strips; profile on big maps.

10) Minimal CPU pseudo for one seam strip
```cpp
struct SeamVert {
    glm::vec3 pos;
    glm::vec3 nWall;
    glm::vec3 nFloor;
    glm::vec2 uvWall;
    glm::vec2 uvFloor;
    float t;
    glm::vec2 lmUVWall;
    glm::vec2 lmUVFloor;
};

void addSeamForEdge(const Face& F0, const Face& F1, const glm::vec3& P0, const glm::vec3& P1, float r) {
    glm::vec3 n0 = normalize(F0.normal);
    glm::vec3 n1 = normalize(F1.normal);
    float ang = acos(glm::clamp(glm::dot(n0, n1), -1.0f, 1.0f));
    if (ang < glm::radians(60.0f) || ang > glm::radians(120.0f)) return;

    glm::vec3 e = glm::normalize(P1 - P0);
    glm::vec3 d0 = glm::normalize(glm::cross(e, n0));
    glm::vec3 d1 = glm::normalize(glm::cross(e, n1));

    glm::vec3 V0w = P0 + d0 * r;
    glm::vec3 V1w = P1 + d0 * r;
    glm::vec3 V0f = P0 + d1 * r;
    glm::vec3 V1f = P1 + d1 * r;

    auto makeV = [&](glm::vec3 pos, float t)->SeamVert{
        SeamVert v{};
        v.pos   = pos;
        v.nWall = n0;
        v.nFloor= n1;
        v.uvWall  = planarUV(F0.texinfo, pos);
        v.uvFloor = planarUV(F1.texinfo, pos);
        v.t = t;
        if (useLightmaps) {
            v.lmUVWall  = lightmapUV(F0, pos);
            v.lmUVFloor = lightmapUV(F1, pos);
        }
        return v;
    };

    uint32_t base = (uint32_t)seamVerts.size();
    seamVerts.push_back(makeV(V0w, 0.0f)); // 0
    seamVerts.push_back(makeV(V1w, 0.0f)); // 1
    seamVerts.push_back(makeV(V0f, 1.0f)); // 2
    seamVerts.push_back(makeV(V1f, 1.0f)); // 3

    seamIndices.insert(seamIndices.end(), {
        base + 0, base + 2, base + 1,
        base + 1, base + 2, base + 3
    });
}
```

11) Parameters to expose as cvars
- r_seam_width (default 4.0)
- r_seam_depthBias (default 0.2)
- r_seam_noiseScale (default 0.35)
- r_seam_blendWidth (default 0.12)
- r_seam_noiseAmp (default 0.06)
- r_seam_dirt (default 0.4)
- r_seam_useLightmap (0/1)

Notes and future upgrades
- Height-based blending: If you have material height maps, blend factor t can be made height-aware for more convincing interpenetration.
- Small geometric fillet: Add a center vertex row at t=0.5 and offset it slightly along nBevel in VS for a subtle real curve.
- Normal/roughness maps: You can sample both materials’ normal maps and blend those too; modulate roughness by dirt to increase realism.
- Decal alternative: If modifying geometry is undesirable, you could render a screen-space decal along detected edges, but you’ll lose accurate per-face UVs.

This setup is intentionally conservative for Quake: minimal changes, stable with lightmaps, and cheap. If you want me to tailor this to your exact Quake fork (vkQuake, Q2VKPT, etc.), share the renderer’s resource bindings for albedo/lightmap and I’ll align the descriptor set layout and UV/lightmap math precisely.