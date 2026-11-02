// Motion shaders for swaying trees and grass (AGENT_LOG 174-178). Attila's vegetation moves by its wind code, a pure
// function of the camera block's time_in_sec (camera_VS_PS c26.x), the wind block (wind_VS: wind_direction.xy /
// wind_vegetation_multiplier in c0, hstrength / vstrength in c1), a phase per instance and a weight per vertex. Each entry
// point repeats the game's position math instruction for instance (mad() where the game has mad, precise against fusing
// where it has mul then add, its constants by their bits) once at this frame's time with this frame's camera - equal to
// the game's clip position bit for bit, because the pass tests depth EQUAL against the game's depth - and once at the
// previous frame's time with the previous camera. windtest.c checks both against the game's own shaders.
//   tree_a: vs_d9228831741dc68e (and 6cd77149, 8b29150b: the same code) - branches / leaves, two LOD positions blended
//           by v6.x / v6.y
//   tree_b: vs_0099406c17c7aff8 (and 25b8fe1f, e8899e58) - the same wind, LOD blend by a single factor from v6.y
//   grass:  vs_c272bdade21308b2 (and 20bbbad8) - grass, fading with the distance from the camera
// Inputs keep the game's semantics, so our input layout is the game's elements and buffers as they are.
#pragma once

static const char wind_hlsl[] =
    "cbuffer motion : register(b0) { float tex_w; float prev_tex_w; float2 size; };\n"
    "cbuffer camera : register(b1) { float4 cam[27]; };\n"        // byte copies of the game's camera_VS_PS block
    "cbuffer prev_camera : register(b2) { float4 pcam[27]; };\n"
    "cbuffer wind : register(b3) { float4 wind0; float4 wind1; };\n"   // a copy of the game's wind_VS block
    "struct VOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prev : TEXCOORD1; };\n"
    "float4 clip(float4 p, float4 r0, float4 r1, float4 r2, float4 r3) { return float4(dot(p, r0), dot(p, r1), dot(p, r2), dot(p, r3)); }\n"
    // the trees' sway added to the blended position b (xyz) for weight wt (v7.w) and phase ph (v14.x)
    "float3 tree_sway(float t, float4 b, float wt, float ph) {\n"
    "  precise float4 r0; precise float r2;\n"
    "  r0.x = mad(ph, 4.0, t);\n"
    "  r0.yz = t * float2(3.0, 0.5);\n"
    "  r0.xyz = sin(r0.xyz);\n"
    "  r0.x = r0.y * r0.x;\n"
    "  r0.y = mad(r0.z, 0.5, 0.75);\n"
    "  r0.x = r0.x * wind1.x;\n"
    "  r0.x = mad(r0.x, 0.5, 1.0);\n"
    "  r0.x = r0.y * r0.x;\n"
    "  r0.x = r0.x * wind1.x;\n"
    "  r0.y = wt * wind0.w;\n"
    "  r0.y = r0.y * wt;\n"
    "  r0.yz = r0.yy * wind0.xy;\n"
    "  r0.xz = r0.xx * r0.yz;\n"
    "  r0.y = 0;\n"
    "  r0.xyz = r0.xyz + b.xyz;\n"
    "  r0.w = b.x + t;\n"
    "  r0.w = r0.w * 6.0;\n"
    "  r0.w = sin(r0.w);\n"
    "  r2 = mad(ph, 2.0, t);\n"
    "  r2 = r2 * asfloat(0x3e99999a);\n"           // 0.3
    "  r2 = sin(r2);\n"
    "  r2 = r2 * wind1.y;\n"
    "  r0.w = r0.w * r2;\n"
    "  r2 = saturate(wt + asfloat(0xbf333333));\n"  // - 0.7
    "  r2 = r2 * wind0.w;\n"
    "  r0.w = r0.w * r2;\n"
    "  return mad(r0.www, float3(0, asfloat(0x40555555), 0), r0.xyz); }\n"   // 10/3
    "struct TreeIn { float4 p0 : POSITION0; float4 p1 : POSITION1; float4 p6 : POSITION6; float4 p7 : POSITION7;\n"
    "  float4 a0 : TEXCOORD0; float4 a1 : TEXCOORD1; float4 a2 : TEXCOORD2; float4 b0 : TEXCOORD3; float4 b1 : TEXCOORD4;\n"
    "  float4 b2 : TEXCOORD5; float4 ph : TEXCOORD6; };\n"
    "VOut tree_a(TreeIn v) {\n"
    "  precise float4 r1, r2;\n"
    "  r1 = float4(dot(v.b0, v.p1), dot(v.b1, v.p1), dot(v.b2, v.p1), v.p1.w);\n"
    "  r1 = r1 * v.p6.y;\n"
    "  r2 = float4(dot(v.a0, v.p0), dot(v.a1, v.p0), dot(v.a2, v.p0), v.p0.w);\n"
    "  r1 = mad(r2, v.p6.x, r1);\n"
    "  VOut o;\n"
    "  o.cur = clip(float4(tree_sway(cam[26].x, r1, v.p7.w, v.ph.x), r1.w), cam[9], cam[10], cam[11], cam[12]);\n"
    "  o.prev = clip(float4(tree_sway(pcam[26].x, r1, v.p7.w, v.ph.x), r1.w), pcam[9], pcam[10], pcam[11], pcam[12]);\n"
    "  o.pos = o.cur; return o; }\n"
    "VOut tree_b(TreeIn v) {\n"
    "  precise float4 r1, r2; precise float f;\n"
    "  f = saturate(0 < v.p6.y && v.p6.y < 1 ? 1 - v.p6.y : v.p6.y);\n"
    "  r1 = float4(dot(v.b0, v.p1), dot(v.b1, v.p1), dot(v.b2, v.p1), v.p1.w);\n"
    "  r2 = float4(dot(v.a0, v.p0), dot(v.a1, v.p0), dot(v.a2, v.p0), v.p0.w);\n"
    "  r1 = r1 - r2;\n"
    "  r1 = mad(f, r1, r2);\n"
    "  VOut o;\n"
    "  o.cur = clip(float4(tree_sway(cam[26].x, r1, v.p7.w, v.ph.x), r1.w), cam[9], cam[10], cam[11], cam[12]);\n"
    "  o.prev = clip(float4(tree_sway(pcam[26].x, r1, v.p7.w, v.ph.x), r1.w), pcam[9], pcam[10], pcam[11], pcam[12]);\n"
    "  o.pos = o.cur; return o; }\n"
    "struct GrassIn { float4 pos : POSITION0; float4 col : COLOR0; float4 r0 : TEXCOORD1; float4 r1 : TEXCOORD2; float4 r2 : TEXCOORD3; };\n"
    // the grass at time t seen from camera block c (its position is in c0: the sway fades with the distance)
    "float4 grass_at(GrassIn v, float t, float4 c0) {\n"
    "  precise float4 r0, r1, r2;\n"
    "  r0.xyz = float3(v.r0.w, v.r1.w, v.r2.w) - c0.xyz;\n"
    "  r0.x = dot(r0.xyz, r0.xyz);\n"
    "  r0.x = sqrt(r0.x);\n"
    "  r0.x = r0.x * asfloat(0x3c23d70a);\n"        // 0.01
    "  r0.x = min(r0.x, 1.0);\n"
    "  r0.y = saturate(v.col.w);\n"
    "  r0.x = mad(r0.y, -r0.x, 1.0);\n"
    "  r0.y = v.pos.y + asfloat(0x3f19999a);\n"     // 0.6
    "  r0.y = mad(r0.x, r0.y, asfloat(0xbf19999a));\n"
    "  r0.xz = v.pos.xz; r0.w = 1;\n"
    "  r1.z = dot(v.r2, r0); r1.x = dot(v.r0, r0); r1.y = dot(v.r1, r0);\n"
    "  r0.xy = mad(r1.xz, 0.25, t.xx);\n"
    "  r0.z = r0.y * asfloat(0x3e99999a);\n"
    "  r0.xyz = sin(r0.xyz);\n"
    "  r0.z = r0.z * wind1.y;\n"
    "  r0.w = r1.x + t;\n"
    "  r0.w = r0.w * 5.0;\n"
    "  r0.w = cos(r0.w);\n"
    "  r0.z = r0.w * r0.z;\n"
    "  r0.w = saturate(v.col.w + asfloat(0xbf333333));\n"
    "  r0.w = r0.w * wind0.w;\n"
    "  r0.z = r0.z * r0.w;\n"
    "  r0.w = t * 3.0;\n"
    "  r0.w = sin(r0.w);\n"
    "  r0.w = r0.w * r0.y;\n"
    "  r0.x = r0.y + r0.x;\n"
    "  r0.y = r0.w * wind1.x;\n"
    "  r0.y = mad(r0.y, 0.5, 0.5);\n"
    "  r0.y = r0.y * wind1.x;\n"
    "  r0.w = v.col.w * wind0.w;\n"
    "  r2.xy = r0.ww * wind0.xy;\n"
    "  r0.yw = r0.yy * r2.xy;\n"
    "  r2.xz = r0.yw * 0.5;\n"
    "  r2.y = 0;\n"
    "  r0.yzw = mad(r0.zzz, float3(0, asfloat(0x40155555), 0), r2.xyz);\n"   // 7/3
    "  r1.xyz = r0.yzw + r1.xyz;\n"
    "  r1.w = 1;\n"
    "  return r1; }\n"
    "VOut grass(GrassIn v) {\n"
    "  VOut o;\n"
    "  o.cur = clip(grass_at(v, cam[26].x, cam[0]), cam[9], cam[10], cam[11], cam[12]);\n"
    "  o.prev = clip(grass_at(v, pcam[26].x, pcam[0]), pcam[9], pcam[10], pcam[11], pcam[12]);\n"
    "  o.pos = o.cur; return o; }\n";
