// Motion pass shaders (dev; DLSS_PLAN "Per-object motion vectors", AGENT_LOG 97-99). The vertex shader repeats the
// skinning of Attila's main-view model shaders (vs_a51431ebf7d15a22 = 2 bones; vs_520dbe181fbd767a and
// vs_3d52a1037cfa96ab = 4 bones, from their disassembly) once with this frame's bones and instance record and once with
// the previous frame's, and outputs both clip positions; the pixel shader writes the pixel motion (previous minus
// current, in pixels of the render size). The game's vertex and instance buffers are used as they are: our input layout
// takes the game's elements plus PREV0..3 = the previous record (world rows, then bone base and animation texture
// index), from its own vertex buffer. The previous bones are in a texture array (one slice per animation texture: big
// battles use several). Compile with SKIN4 defined for the 4-bone layouts. The jitter is in the viewport.
#pragma once

static const char motion_hlsl[] =
    "cbuffer motion : register(b0) { float tex_w; float prev_tex_w; float2 size; };\n"
    // byte copies of the game's camera_VS_PS block (view_projection at byte 144) of this frame and the previous one: the
    // current clip position must equal the game's bit for bit, because the pass tests depth EQUAL against the game's depth
    "cbuffer camera : register(b1) { float4 c_pad[9]; row_major float4x4 vp; };\n"
    "cbuffer prev_camera : register(b2) { float4 p_pad[9]; row_major float4x4 prev_vp; };\n"
    "Texture2D<float4> anim : register(t0);\n"
    "Texture2DArray<float4> prev_anim : register(t1);\n"
    "struct VIn {\n"
    "  float4 pos : POSITION0; float4 bi : POSITION1;\n"
    "#ifdef SKIN4\n"
    "  float4 bw : POSITION2;\n"
    "#endif\n"
    "  float4 w0 : TEXCOORD0; float4 w1 : TEXCOORD1; float4 w2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "  float4 p0 : PREV0; float4 p1 : PREV1; float4 p2 : PREV2; float4 p3 : PREV3; };\n"
    "struct VOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prev : TEXCOORD1; };\n"
    // one texel: this frame's texture (prev == 0) or slice s of the previous frame's (the branch folds away when inlined)
    "float4 texel(int prev, int col, int row, int s) {\n"
    "  return prev ? prev_anim.Load(int4(col, row, s, 0)) : anim.Load(int3(col, row, 0)); }\n"
    // one bone: a 3x4 matrix in 3 texels from texel index i (row / column by the texture width)
    "float3 bone(int prev, int s, int i, int w, float4 p) {\n"
    "  int row = i / w; int col = i - row * w;\n"
    "  return float3(dot(p, texel(prev, col, row, s)), dot(p, texel(prev, col + 1, row, s)), dot(p, texel(prev, col + 2, row, s))); }\n"
    "float3 skin(int prev, int s, float texw, VIn v, float base) {\n"
    "  int w = (int)floor(texw + 0.5), b = (int)floor(base + 0.5);\n"
    "  float4 p = float4(v.pos.xyz, 1);\n"
    "#ifdef SKIN4\n"
    "  int4 i = (int4)floor(v.bi.zyxw * 255.01) * 3 + b;\n"   // i.x: index in .z, i.y: .y, i.z: .x, i.w: .w
    "  float3 r = bone(prev, s, i.y, w, p) * v.bw.y;\n"
    "  r = v.bw.z * bone(prev, s, i.x, w, p) + r;\n"
    "  r = v.bw.x * bone(prev, s, i.z, w, p) + r;\n"
    "  return (1 - ((v.bw.y + v.bw.z) + v.bw.x)) * bone(prev, s, i.w, w, p) + r;\n"
    "#else\n"
    "  int2 i = (int2)floor(v.bi.zy * 255.01) * 3 + b;\n"     // i.x: index in .z (weight .x), i.y: index in .y (1 - .x)
    "  float3 r = bone(prev, s, i.y, w, p) * (1 - v.bi.x);\n"
    "  return v.bi.x * bone(prev, s, i.x, w, p) + r;\n"
    "#endif\n"
    "}\n"
    "float4 place(float3 s, float4 r0, float4 r1, float4 r2, row_major float4x4 m) {\n"
    "  float4 q = float4(s, 1);\n"
    "  return mul(m, float4(dot(q, r0), dot(q, r1), dot(q, r2), 1)); }\n"
    "VOut vs(VIn v) {\n"
    "  VOut o;\n"
    "  o.cur = place(skin(0, 0, tex_w, v, v.t3.x), v.w0, v.w1, v.w2, vp);\n"
    // no history (bone base < 0, a model new this frame): the current pose and place, so only the camera moves it
    "  if (v.p3.x < 0) o.prev = place(skin(0, 0, tex_w, v, v.t3.x), v.w0, v.w1, v.w2, prev_vp);\n"
    "  else o.prev = place(skin(1, (int)v.p3.y, prev_tex_w, v, v.p3.x), v.p0, v.p1, v.p2, prev_vp);\n"
    "  o.pos = o.cur;\n"
    "  return o; }\n"
    "float2 ps(VOut i) : SV_Target {\n"
    "  float2 c = i.cur.xy / i.cur.w, p = i.prev.xy / i.prev.w;\n"
    "  return (p - c) * float2(0.5, -0.5) * size; }\n";
