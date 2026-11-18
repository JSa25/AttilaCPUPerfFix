// Flag wave (dev, notes/banners.md "Our own system"): our copies of the game's two rigid prop vertex shaders, used in
// their place by bridgeprobe.c when flagwave_on.txt is in the game folder.
//   main:   vs_55ca645c65c0fbe6 (main view, stage +0x1CCE4E4)
//   shadow: vs_eb3006be08a96700 (shadow maps, stage +0x1C9CF00)
// Each repeats the game's code instruction for instruction (flagtest.c checks every output bit for bit against the
// game's bytecode from the motion probe) with one change: a vertex whose POSITION.w is not 1.0 is cloth, placed by
// wave() before the instance transform (hanging from the pole, streaming downwind as the wind rises, rippling), and its
// normal / tangents follow (main view). The pack (tools/banner_test/make_wave_pack.py) encodes in w the weight, side
// and size. Every model the game ships has w = 1.0 and is untouched. Time = the camera block's time_in_sec (c26.x, the
// vegetation wind's clock); wind = the game's wind_VS block (which bridgeprobe.c lends to the shadow shader too).
#pragma once

// cloth simulation sizes (shared by the shaders and bridgeprobe.c): FS_GX particles along the pole x FS_GY away from it
// per flag, FS_SLOTS flags (slot ids 16 bits in the buckets), FS_SLOT4 float4s of slot data, FS_GATHER flag instances gathered per
// frame (one match thread each), FS_NB lookup buckets (2 m cells of the ground, hashed; 2 float4s each, up to 12 flags)
#define FS_GX 24
#define FS_GY 36
#define FS_N (FS_GX * FS_GY)
#define FS_SLOTS 512
#define FS_SLOT4 12
#define FS_GATHER 1024
#define FS_NB 512
#define FS_STR2(x) #x
#define FS_STR(x) FS_STR2(x)
#define FS_DEFS "#define FS_GX " FS_STR(FS_GX) "\n#define FS_GY " FS_STR(FS_GY) "\n#define FS_N " FS_STR(FS_N) "\n#define FS_SLOTS " \
    FS_STR(FS_SLOTS) "\n#define FS_SLOT4 " FS_STR(FS_SLOT4) "\n#define FS_GATHER " FS_STR(FS_GATHER) "\n#define FS_NB " FS_STR(FS_NB) "\n" \
    "uint fs_bucket(int2 c) { return ((uint)(c.x * 73856093) ^ (uint)(c.y * 19349663)) & (FS_NB - 1); }\n"

static const char flag_hlsl[] =
    "cbuffer camera_VS_PS : register(b0) { float4 cam[27]; };\n"
    "cbuffer rigid_config_PS : register(b1) { float4 cfg[5]; };\n"
    "cbuffer wind_VS : register(b2) { float4 wind0; float4 wind1; };\n"
    "Texture2D<uint4> t_sel : register(t0);\n"
    "struct VIn { float4 p : POSITION0; float4 uv : TEXCOORD0; float4 n0 : NORMAL0; float4 n1 : NORMAL1; float4 n2 : NORMAL2;\n"
    "  float4 col : COLOR0; float4 r0 : POSITION1; float4 r1 : POSITION2; float4 r2 : POSITION3; float4 i3 : POSITION4;\n"
    "  float4 i4 : POSITION5; float4 i5 : POSITION6; float4 i6 : POSITION7; uint iid : SV_InstanceID; };\n"
    "struct VOut { float4 pos : SV_Position; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;\n"
    "  float4 t3 : TEXCOORD3; float3 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;\n"
    "  float4 c1 : COLOR1; float4 c2 : COLOR2; float4 nrm : NORMAL0; uint sel : POSITION1; };\n"
    "struct SOut { float4 pos : SV_Position; float4 t0 : TEXCOORD0; float2 t1 : TEXCOORD1; };\n"
    FS_DEFS
    // the cloth simulation's results (flag_cs_hlsl, bridgeprobe.c lends them to slots t14 / t15 while our shaders are set)
    "StructuredBuffer<float4> fs_slot : register(t14);\n"
    "StructuredBuffer<float4> fs_grid : register(t15);\n"
    // the simulated cloth for this vertex, if its flag has a slot: the nearest slot of the same model whose pole was
    // within 1 m of this instance's last frame (the simulation runs a frame behind: shifted by the instance's move since).
    // Out: world position (with the cloth's own thickness), and the cloth's frame e (along the pole), f (away from it),
    // z (its normal, on the side the flat model's +z faced)
    // Catmull-Rom weights of the 4 points around t, and their slopes
    "float4 cr_w(float t) { float t2 = t * t, t3 = t2 * t; return 0.5 * float4(-t + 2.0 * t2 - t3, 2.0 - 5.0 * t2 + 3.0 * t3, t + 4.0 * t2 - 3.0 * t3, -t2 + t3); }\n"
    "float4 cr_d(float t) { float t2 = t * t; return 0.5 * float4(-1.0 + 4.0 * t - 3.0 * t2, -10.0 * t + 9.0 * t2, 1.0 + 8.0 * t - 9.0 * t2, -2.0 * t + 3.0 * t2); }\n"
    "float4 bs_w(float t) { float t2 = t * t, t3 = t2 * t; return (1.0 / 6.0) * float4(1.0 - 3.0 * t + 3.0 * t2 - t3, 4.0 - 6.0 * t2 + 3.0 * t3, 1.0 + 3.0 * t + 3.0 * t2 - 3.0 * t3, t3); }\n"
    "float4 bs_d(float t) { float t2 = t * t; return 0.5 * float4(-1.0 + 2.0 * t - t2, -4.0 * t + 3.0 * t2, 1.0 + 2.0 * t - 3.0 * t2, t2); }\n"
    "bool sim_place(VIn v, float x, float u, float far, float side, float ax, out float3 P, out float3 e, out float3 f, out float3 z, out float4 m0, out float4 m1, out float4 m2) {\n"
    "  P = 0.0; e = 0.0; f = 0.0; z = 0.0; m0 = v.r0; m1 = v.r1; m2 = v.r2;\n"
    // the flags simulated this frame, by 2 m cell of the ground (buckets after the header): the 2 x 2 cells around this
    // instance hold every pole within 1 m of it
    "  float3 O = float3(v.r0.w, v.r1.w, v.r2.w);\n"
    "  int2 c0 = (int2)floor(O.xz * 0.5 - 0.5);\n"
    "  int best = -1; float bd = 0.6;\n"   // its own cloth only (1 m let a flag without one borrow a neighbour's, 1 m off)
    "  [unroll] for (int cy = 0; cy < 2; cy++) [unroll] for (int cx = 0; cx < 2; cx++) {\n"
    "    uint bi = FS_SLOTS * FS_SLOT4 + 1 + 2 * fs_bucket(c0 + int2(cx, cy));\n"
    "    float4 be = fs_slot[bi], be2 = fs_slot[bi + 1];\n"
    "    uint n = min(asuint(be.x), 12u);\n"
    "    [loop] for (uint q = 0; q < n; q++) {\n"
    "      uint wd = asuint(q < 2 ? be.y : q < 4 ? be.z : q < 6 ? be.w : q < 8 ? be2.x : q < 10 ? be2.y : be2.z);\n"
    "      uint s = (wd >> (16 * (q & 1))) & 0xFFFF;\n"
    // against where that flag should be now (this frame is one after the last Present's)
    "      float4 q0 = fs_slot[s * FS_SLOT4];\n"
    // the nearer of its last pole and where its speed puts it now (one bad speed reading must not lose it)
    "      float d = min(length(O - q0.xyz), length(O - (q0.xyz + fs_slot[s * FS_SLOT4 + 6].xyz * (fs_slot[FS_SLOTS * FS_SLOT4].x + 1.0 - q0.w))));\n"
    "      if (d < bd && abs(fs_slot[s * FS_SLOT4 + 4].z - far) < 0.3) { best = (int)s; bd = d; }\n"
    "    }\n"
    "  }\n"
    "  if (best < 0) return false;\n"
    "  float4 s0 = fs_slot[best * FS_SLOT4], p4 = fs_slot[best * FS_SLOT4 + 4], p5 = fs_slot[best * FS_SLOT4 + 5];\n"
    "  float fi = saturate((x - p4.x) / max(p4.y - p4.x, 1e-3)) * (FS_GX - 1);\n"
    "  float du = abs(ax < 1.5 ? v.p.y - p5.x : v.p.z - p5.y);\n"
    "  float fj = saturate(du / max(far, 1e-3)) * (FS_GY - 1);\n"
    "  int i0 = min((int)fi, FS_GX - 2), j0 = min((int)fj, FS_GY - 2);\n"
    "  float a = fi - i0, b = fj - j0;\n"
    // a smooth surface through the particles (Catmull-Rom, 4 x 4 around the cell; the edges repeat), and its slopes
    "  float4 wa = cr_w(a), wb = cr_w(b);\n"
    "  float4 ba = bs_w(a), bb = bs_w(b), da = bs_d(a), db = bs_d(b);\n"
    "  float3 Ps = 0.0, Pa = 0.0, Pb = 0.0;\n"
    "  [unroll] for (int jj = 0; jj < 4; jj++) [unroll] for (int ii = 0; ii < 4; ii++) {\n"
    "    int gi = clamp(i0 - 1 + ii, 0, FS_GX - 1), gj = clamp(j0 - 1 + jj, 0, FS_GY - 1);\n"
    "    float3 q = fs_grid[best * FS_N + gj * FS_GX + gi].xyz;\n"
    "    Ps += wa[ii] * wb[jj] * q; Pa += da[ii] * bb[jj] * q; Pb += ba[ii] * db[jj] * q;\n"
    "  }\n"
    "  e = normalize(Pa);\n"
    "  f = normalize(Pb);\n"
    // the thin axis of the flat model (z for cloth along y, y for cloth along z) and its sign as cross(along pole, out)
    "  z = (ax < 1.5 ? side : -side) * normalize(cross(e, f));\n"
    "  P = Ps + z * (ax < 1.5 ? v.p.z - p5.y : v.p.y - p5.x);\n"
    // the instance rows the cloth was stepped with: taken back through them, the cloth moves and turns with its pole
    // until the next step (a frame later, or 2-4 for far flags)
    "  m0 = fs_slot[best * FS_SLOT4 + 8]; m1 = fs_slot[best * FS_SLOT4 + 9]; m2 = fs_slot[best * FS_SLOT4 + 10];\n"
    "  return true; }\n"
    // local = inverse of the instance rows (any scale), for positions (with the translation) and directions
    "float3 to_local(float4 r0, float4 r1, float4 r2, float3 w, float tr) {\n"
    "  float3 a0 = r0.xyz, a1 = r1.xyz, a2 = r2.xyz;\n"
    "  float3 c0 = cross(a1, a2), c1 = cross(a2, a0), c2 = cross(a0, a1);\n"
    "  w -= tr * float3(r0.w, r1.w, r2.w);\n"
    "  return (c0 * w.x + c1 * w.y + c2 * w.z) / dot(a0, c0); }\n"
    // the cloth (notes/banners.md): POSITION.w = side * (F + 0.5 * (1 - k)) from make_wave_pack.py; w = 1.0 (the game's
    // models) leaves the vertex untouched. k: 0 at the pole .. 1 at the free edge; F: the cloth's size in quarter metres;
    // side: which way (local y) the cloth was modelled. The pole runs along local x. Each fibre of cloth leaves the pole
    // in one direction f: downwind across the pole, mixed with world down by the wind's strength (calm: it hangs along the
    // pole; strong wind: it streams out, slow gusts lifting and dropping it). Three ripples run along the fibre and
    // displace the cloth along its normal; e, f, z = the deformed cloth's frame at the vertex for normals and tangents.
    "struct Wv { float3 p; float3 e; float3 f; float3 z; float side; float ax; float on; };\n"
    "Wv wave(VIn v) {\n"
    "  Wv r; r.p = v.p.xyz; r.e = float3(1.0, 0.0, 0.0); r.f = float3(0.0, 1.0, 0.0); r.z = float3(0.0, 0.0, 1.0); r.side = 1.0; r.ax = 1.0; r.on = 0.0;\n"
    // our own cloth only. make_wave_pack guarantees |w| >= 2 (F is 2..31 plus a fraction under 1), and says so:
    // "never 1.0: the game other models (w = 1.0) stay apart". Testing w != 1.0 instead caught every model whose w
    // is anything else - w = 0 among them, which 1212 ships on its _w_mask banners and Dawnless Days evidently
    // ships on spears, swords and shields: players saw those weapons flapping as cloth (report 2026-11-18).
    "  [branch] if (abs(v.p.w) >= 2.0) {\n"
    // frac 0..0.5: cloth along y, k = 1 - 2 frac; 0.52..0.98: cloth along z (lance pennants), k = (0.98 - frac) / 0.46
    "    float aw = abs(v.p.w), F = floor(aw), fr = aw - F;\n"
    "    float ax = fr > 0.51 ? 2.0 : 1.0;\n"
    "    float k = ax > 1.5 ? saturate((0.98 - fr) / 0.46) : saturate(1.0 - 2.0 * fr);\n"
    "    float far = max(F, 2.0) * 0.25;\n"
    "    float side = v.p.w < 0.0 ? -1.0 : 1.0;\n"
    "    float u = k * far, x = v.p.x;\n"
    "    r.side = side; r.ax = ax; r.on = 1.0;\n"
    // simulated: from the cloth simulation, taken back into the model's frame (the wave below only without it)
    "    float3 SP, Se, Sf, Sz; float4 m0, m1, m2;\n"
    "    [branch] if (sim_place(v, x, u, far, side, ax, SP, Se, Sf, Sz, m0, m1, m2)) {\n"
    "      r.p = to_local(m0, m1, m2, SP, 1.0);\n"
    "      r.e = normalize(to_local(m0, m1, m2, Se, 0.0)); r.f = normalize(to_local(m0, m1, m2, Sf, 0.0)); r.z = normalize(to_local(m0, m1, m2, Sz, 0.0));\n"
    "      return r;\n"
    "    }\n"
    "    float t = cam[26].x;\n"
    // a fixed phase per flag from where it stands, small enough that a walking bearer does not hurry the ripples
    "    float ph = (v.r0.w + v.r2.w) * 0.05;\n"
    // the game's wind (world x, z) and world down in the instance's local frame (rows transposed; a uniform scale cancels)
    "    float3 wl = v.r0.xyz * wind0.x + v.r2.xyz * wind0.y;\n"
    "    float3 dl = normalize(-v.r1.xyz);\n"
    "    float2 wp = wl.yz;\n"
    "    float lw = length(wp);\n"
    "    float3 o = lw > 1e-4 ? float3(0.0, wp / lw) : (ax < 1.5 ? float3(0.0, side, 0.0) : float3(0.0, 0.0, side));\n"
    "    float so = ax < 1.5 ? side : -side;\n"   // the flat model's thin axis as cross(along pole, out)
    "    float h = saturate(wind1.x) * saturate(length(wind0.xy) * 4.0);\n"
    "    float s = h * (0.75 + 0.25 * sin(t * 0.7 + ph * 3.0) * sin(t * 0.23 + 1.3));\n"   // gusts: half to full strength
    "    float3 f = normalize(o * max(1.6 * s, 0.3) + dl);\n"
    "    float3 n = so * normalize(float3(0.0, -f.z, f.y));\n"   // so * cross(x, f): the cloth's normal
    // ripples: speed fixed for the battle (from the wind's strength, never from time-varying values: t is large)
    "    float ws = 0.6 + 0.6 * saturate(wind1.x);\n"
    "    float a1 = u * 7.5 + x * 1.8 - t * 8.0 * ws + ph;\n"       // ~0.8 m ripple, slightly diagonal
    "    float a2 = u * 15.0 - x * 3.1 - t * 12.5 * ws + ph * 1.7;\n"   // ~0.4 m ripple the other way
    "    float a3 = u * 4.0 - t * 3.0 * ws + ph * 0.6;\n"           // slow ~1.6 m swell
    "    float S = 0.55 * sin(a1) + 0.18 * sin(a2) + 0.45 * sin(a3);\n"
    "    float Su = 0.55 * 7.5 * cos(a1) + 0.18 * 15.0 * cos(a2) + 0.45 * 4.0 * cos(a3);\n"
    "    float Sx = 0.55 * 1.8 * cos(a1) - 0.18 * 3.1 * cos(a2);\n"
    "    float amp = 0.08 * sqrt(far) * (0.5 + 0.7 * s);\n"         // ~0.15 m on a 2 m banner in full wind
    "    float A = amp * k;\n"
    "    float d = A * S, du = amp / far * S + A * Su, dx = A * Sx;\n"
    // the pole point of this vertex's fibre, then along the fibre, then across (ripple and the cloth's own thickness)
    "    r.p = (ax < 1.5 ? float3(x, v.p.y - side * u, 0.0) : float3(x, 0.0, v.p.z - side * u)) + f * u + n * (d + (ax < 1.5 ? v.p.z : v.p.y));\n"
    "    r.e = normalize(float3(1.0, 0.0, 0.0) + n * dx);\n"
    "    r.f = normalize(f + n * du);\n"
    "    r.z = so * normalize(cross(r.e, r.f));\n"
    "  }\n"
    "  return r; }\n"
    // a normal or tangent of the flat model (x along the pole, side * the out axis along the fibre, the thin axis across)
    // into the cloth's frame
    "float3 remap(float3 n, Wv r) { return n.x * r.e + (r.ax < 1.5 ? n.y : n.z) * r.side * r.f + (r.ax < 1.5 ? n.z : n.y) * r.z; }\n"
    "float fs_flip(VIn v, Wv r, float3 nmodel, float3 nworld) {\n"
    "  if (r.on == 0.0) return 1.0;\n"
    // Both halves of the decision must be LOW FREQUENCY, or the shading snaps between neighbours and draws a
    // line along every ripple crest (fscrest_test: 78 seams, neighbours as opposite as -0.9998).
    // face: which sheet of the cloth this vertex is on - a property of the sheet, not of the curvature, so the
    //       two faces end up sharing one normal.
    // view: the whole flag instance taken into world, so it cannot differ between neighbouring vertices.
    "  float face = (r.ax < 1.5 ? nmodel.z : nmodel.y) < 0.0 ? -1.0 : 1.0;\n"
    "  float3 thin = r.ax < 1.5 ? float3(v.r0.z, v.r1.z, v.r2.z) : float3(v.r0.y, v.r1.y, v.r2.y);\n"
    "  return face * (dot(thin, cam[12].xyz) > 0.0 ? -1.0 : 1.0);\n"
    "}\n"
    "VOut main_vs(VIn v) {\n"
    "  VOut o;\n"
    "  Wv r = wave(v);\n"
    "  float4 p = float4(r.p, 1.0);\n"
    "  float4 w = float4(dot(p, v.r0), dot(p, v.r1), dot(p, v.r2), 1.0);\n"
    "  float4 c = float4(dot(w, cam[9]), dot(w, cam[10]), dot(w, cam[11]), dot(w, cam[12]));\n"
    "  o.pos = c;\n"
    "  o.c1 = float4(c.x, c.y, c.w, v.i3.y);\n"
    "  o.t0 = w;\n"
    "  o.t1 = v.uv;\n"
    "  float3 nm0 = v.n0.xyz * 2.0 - 1.0;\n"
    "  float3 n = nm0;\n"
    "  [branch] if (r.on != 0.0) n = remap(n, r);\n"
    "  n = float3(dot(n, v.r0.xyz), dot(n, v.r1.xyz), dot(n, v.r2.xyz));\n"
    "  float sflip = fs_flip(v, r, nm0, n);\n"
    "  n *= sflip;\n"
    "  o.t2 = float4(normalize(n), wind0.x);\n"
    "  n = v.n1.xyz * 2.0 - 1.0;\n"
    "  [branch] if (r.on != 0.0) n = remap(n, r);\n"
    "  n = float3(dot(n, v.r0.xyz), dot(n, v.r1.xyz), dot(n, v.r2.xyz));\n"
    "  o.t3 = float4(normalize(n), wind0.y);\n"
    "  n = v.n2.xyz * 2.0 - 1.0;\n"
    "  [branch] if (r.on != 0.0) n = remap(n, r);\n"
    "  n = float3(dot(n, v.r0.xyz), dot(n, v.r1.xyz), dot(n, v.r2.xyz));\n"
    "  o.t4 = normalize(n) * sflip;\n"
    "  o.t5 = v.i3; o.t6 = v.i4; o.t7 = v.i5;\n"
    "  o.c2 = v.col; o.nrm = v.i6;\n"
    "  o.sel = t_sel.Load(int3(asint(v.iid) + asint(cfg[4].w), 0, 0)).x;\n"
    "  return o; }\n"
    "SOut shadow_vs(VIn v) {\n"
    "  SOut o;\n"
    "  float4 p = float4(wave(v).p, 1.0);\n"
    "  float4 w = float4(dot(p, v.r0), dot(p, v.r1), dot(p, v.r2), 1.0);\n"
    "  o.pos = float4(dot(w, cam[9]), dot(w, cam[10]), dot(w, cam[11]), dot(w, cam[12]));\n"
    "  o.t0 = w;\n"
    "  o.t1 = v.uv.xy;\n"
    "  return o; }\n";

// The cloth simulation (compute, run by bridgeprobe.c once a frame before our shaders draw, on the flags gathered from the
// previous frame's draws). Slot data (FS_SLOT4 float4s per flag): 0 pole origin xyz, frame last seen; 1-3 instance rows;
// 4 xmin, xmax (the cloth along the pole), far (its size), side; 5 y, z of the pole in the model, init flag, frame claimed;
// 8-10 the instance rows the cloth was last stepped with (the shader draws the cloth rigidly with its pole from them);
// after the slots: the current frame. Grid: FS_SLOTS * FS_N positions, then as many previous positions.
//   match: one thread; every gathered flag instance claims the nearest slot of the same model within 1.5 m (the same
//          instance drawn again in another pass, within 2 cm, shares it), or the oldest slot (started from the model's
//          own shape).
//   step:  one group per slot seen this frame, one thread per particle: verlet in substeps, the pole row pinned to this
//          frame's pole, gravity, the wind on the cloth's face (pressure along the normal plus a little drag) with gusts,
//          then stretch / shear / bend links relaxed.
static const char flag_cs_hlsl[] =
    FS_DEFS
    // sim0: gathered count, dt, time, frame; tune0: wind speed scale (m/s at strength 1), air pressure, skin drag, still-air
    // damping; tune1: turbulence, shear, bending, gust size (flagsim.c fs_tune); tune2: substeps (near), lift, gust field
    // size (m), eddy size (m) (fs_tune2); sim1.x: 1 when the game's camera block is bound (detail by distance)
    "cbuffer sim : register(b0) { float4 sim0; float4 tune0; float4 tune1; float4 tune2; float4 sim1; };\n"
    "cbuffer wind_VS : register(b1) { float4 wind0; float4 wind1; };\n"
    "cbuffer camera_VS_PS : register(b2) { float4 camb[27]; };\n"   // the game's camera block: camera_position in c0
    "ByteAddressBuffer fs_gather : register(t0);\n"
    "StructuredBuffer<float4> fs_gparam : register(t1);\n"
    "RWStructuredBuffer<float4> slot : register(u0);\n"
    "RWStructuredBuffer<float4> grid : register(u1);\n"
    // match, one thread per gathered flag instance (was one thread for all: 0.44 ms for 88): each finds the nearest slot of
    // its model within 1.5 m and claims it (the nearest claimer wins; the same flag drawn again in another pass, within
    // 2 cm of the winner, shares it); new flags get the oldest unclaimed slot, one at a time (rare after the first
    // frame). The flags simulated this frame go into the lookup buckets after the header (by 2 m cell of the ground).
    "groupshared uint mclaim[FS_SLOTS];\n"
    "groupshared int mslot[FS_GATHER];\n"
    // up to 12 flags a 2 m cell (4 left lancers' pennants out of the lookup in a tight formation: they flipped between
    // their cloth and the wave from frame to frame); counters for the log: new claims, flags left out of the lookup
    // bids doubles as each gathered flag's size and side|pass while the claims are sorted out (cleared after);
    // alist doubles as the list of free slots once the search is done
    "groupshared uint bcnt[FS_NB], bids[FS_NB * 6], nfresh, nover, alist[FS_SLOTS], nact, nfree, nneed, fbits[FS_SLOTS / 32];\n"
    "[numthreads(FS_GATHER, 1, 1)] void match(uint g : SV_GroupIndex) {\n"
    "  float frame = sim0.w; uint ng = min((uint)sim0.x, (uint)FS_GATHER);\n"
    "  if (g < FS_SLOTS) mclaim[g] = 0xFFFFFFFFu;\n"
    "  if (g == 0) { nfresh = 0; nover = 0; nact = 0; nfree = 0; nneed = 0; }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    // the slots in use (seen in the last 30 frames), listed once: each flag then checks those, not all FS_SLOTS
    "  if (g < FS_SLOTS) { float sw = slot[g * FS_SLOT4].w; if (sw > 0.0 && sw >= frame - 30.0) { uint ia; InterlockedAdd(nact, 1, ia); alist[ia] = g; } }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  bool act = g < ng;\n"
    "  float4 r0 = 0.0, r1 = 0.0, r2 = 0.0, q0 = 0.0, q1 = 0.0; float3 O = 0.0; int best = -1; float bd = 1.5;\n"
    "  if (act) {\n"
    "    r0 = asfloat(fs_gather.Load4(g * 112)); r1 = asfloat(fs_gather.Load4(g * 112 + 16)); r2 = asfloat(fs_gather.Load4(g * 112 + 32));\n"
    "    O = float3(r0.w, r1.w, r2.w); q0 = fs_gparam[2 * g]; q1 = fs_gparam[2 * g + 1];\n"
    "    uint na = nact;\n"
    "    [loop] for (uint ka = 0; ka < na; ka++) {\n"
    "      int s = (int)alist[ka];\n"
    "      float4 s0 = slot[s * FS_SLOT4], s4 = slot[s * FS_SLOT4 + 4];\n"
    // the same flag in every LOD and pass (the shadow pass often draws another LOD than the main view; 2026-11-12: one
    // banner got two cloths and jumped between them): by size (a quarter-metre step either way) and side, not the
    // exact x range each LOD mesh measured
    "      if (s0.w < frame - 30.0 || abs(s4.z - q0.z) > 0.3 || s4.w != q0.w) continue;\n"
    // where the flag should be now (its last pole moved on at its speed): a galloping rider is 13 cm on, and a passing
    // neighbour's last pole could be nearer than its own (2026-11-12, flagglitch: cloths swapped and restarted)
    "      float d = min(length(O - s0.xyz), length(O - (s0.xyz + slot[s * FS_SLOT4 + 6].xyz * (frame - s0.w))));\n"
    "      if (d < bd) { bd = d; best = s; }\n"
    "    }\n"
    "  }\n"
    "  uint mpass = q1.z > 0.5 ? 1u : 0u;\n"
    "  mslot[g] = -1;\n"
    "  if (g < FS_GATHER) { bids[2 * g] = asuint(q0.z); bids[2 * g + 1] = (asuint(q0.w) & ~1u) | mpass; }\n"   // size; side (its low mantissa bit) | pass
    "  GroupMemoryBarrierWithGroupSync();\n"
    // the claims: the main view's draw of a flag beats its shadow-pass draw (2026-11-12: the shadow pass can draw a moving
    // flag a frame behind; when it won, or both claimed, a charging army's pennants swapped cloths and restarted)
    "  uint key = act && best >= 0 ? (mpass << 31) | (min((uint)(bd * 1000.0), 0x1FFFFFu) << 10) | g : 0xFFFFFFFFu;\n"
    "  if (key != 0xFFFFFFFFu) InterlockedMin(mclaim[best], key);\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  if (key != 0xFFFFFFFFu && mclaim[best] == key) mslot[g] = best;\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    // a draw without a cloth of its own: the same flag drawn by another pass or LOD (same size and side, within 0.35 m of
    // a claimer) shares that cloth; else it needs a new one, unless another draw of it does (the main view's, then the first)
    "  bool need = false;\n"
    "  if (act && mslot[g] < 0) {\n"
    "    uint sz = bids[2 * g], sd = bids[2 * g + 1] & ~1u; bool share = false;\n"
    "    [loop] for (uint h = 0; h < ng && !share; h++) {\n"
    "      if (h == g || abs(asfloat(bids[2 * h]) - asfloat(sz)) > 0.3 || (bids[2 * h + 1] & ~1u) != sd) continue;\n"
    "      float3 Oh = float3(asfloat(fs_gather.Load(h * 112 + 12)), asfloat(fs_gather.Load(h * 112 + 28)), asfloat(fs_gather.Load(h * 112 + 44)));\n"
    "      if (length(Oh - O) > 0.35) continue;\n"
    "      if (mslot[h] >= 0) share = true;\n"
    "      else { uint ph = bids[2 * h + 1] & 1u; if (ph < mpass || (ph == mpass && h < g)) share = true; }\n"
    "    }\n"
    // only a flag in view gets a new cloth: the shadow pass draws the whole army (2026-11-12 recording: over 512 flags,
    // every cloth taken in the first second and none left for the flags coming into view)
    "    if (share) mslot[g] = -2; else need = mpass == 0;\n"
    "  }\n"
    // the free slots: not in view (no main-view draw) for over 10 frames, and not claimed by a main-view draw this frame
    // (a shadow draw keeping it may lose it). In slot order, in parallel: a bit per free slot, each one's rank = the free
    // slots below it (the old one-thread claim loop over every slot per new flag took 2.5 ms for 100 new flags)
    "  bool isfree = g < FS_SLOTS && (mclaim[g] == 0xFFFFFFFFu || ((mclaim[g] >> 31) != 0 && mclaim[g] != 0xFFFFFFFEu)) && slot[g * FS_SLOT4 + 7].x < frame - 10.5;\n"
    "  if (g < FS_SLOTS / 32) fbits[g] = 0;\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  if (isfree) InterlockedOr(fbits[g >> 5], 1u << (g & 31));\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  if (isfree) { uint rk = countbits(fbits[g >> 5] & ((1u << (g & 31)) - 1u)); for (uint wq = 0; wq < (g >> 5); wq++) rk += countbits(fbits[wq]); alist[rk] = g; }\n"
    "  if (g == 0) { uint nf = 0; for (uint wq = 0; wq < FS_SLOTS / 32; wq++) nf += countbits(fbits[wq]); nfree = nf; }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  if (need) { uint ni; InterlockedAdd(nneed, 1, ni); if (ni < nfree) { mclaim[alist[ni]] = 0xFFFFFFFEu; mslot[g] = (int)(alist[ni] | 0x1000); } else mslot[g] = -2; }\n"   // all in use: the wave
    "  if (g < FS_NB) { bcnt[g] = 0; [unroll] for (int w6 = 0; w6 < 6; w6++) bids[6 * g + w6] = 0; }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    // a shadow draw whose cloth just went to a flag in view: the wave in the shadow, this frame
    "  if (act && mslot[g] >= 0 && (mslot[g] & 0x1000) == 0 && mclaim[mslot[g]] == 0xFFFFFFFEu) mslot[g] = -2;   // (claim keys never reach that value)\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  int ms = act ? mslot[g] : -2;\n"
    "  if (ms >= 0) {\n"
    "    uint s = (uint)ms & 0xFFF;\n"
    "    float init = (ms & 0x1000) ? 1.0 : slot[s * FS_SLOT4 + 5].z;\n"
    "    float4 so = slot[s * FS_SLOT4];\n"
    "    float3 vel = (ms & 0x1000) ? 0.0 : (O - so.xyz) / max(frame - so.w, 1.0);\n"
    "    slot[s * FS_SLOT4 + 6] = float4(length(vel) < 0.5 ? vel : 0.0, 0.0);\n"   // its speed per frame (none over 30 m/s: a jump, not a ride)
    "    slot[s * FS_SLOT4] = float4(O, frame);\n"
    // the cloth is simulated on the pole's height smoothed over ~0.2 s: a cantering horse moves the lance up and down 2-8 cm
    // a frame, and the cloth swung 2.5x that instead of streaming back (2026-11-13 recording, ridetest). Our shader draws
    // it back through these rows onto the real pole, so it still rides the lance's bounce as one piece.
    "    float4 o7 = slot[s * FS_SLOT4 + 7];\n"
    "    float ys = (ms & 0x1000) || abs(O.y - o7.y) > 1.0 ? O.y : lerp(o7.y, O.y, 1.0 - exp(-sim0.y * max(frame - so.w, 1.0) / 0.2));\n"
    "    slot[s * FS_SLOT4 + 1] = r0; slot[s * FS_SLOT4 + 2] = float4(r1.xyz, ys); slot[s * FS_SLOT4 + 3] = r2;\n"
    // the cloth's shape numbers stay those it started with (another LOD's x range must not shift it from frame to frame)
    "    bool fresh = (ms & 0x1000) != 0;\n"
    "    if (fresh) slot[s * FS_SLOT4 + 4] = q0;\n"
    "    slot[s * FS_SLOT4 + 5] = float4(fresh ? q1.xy : slot[s * FS_SLOT4 + 5].xy, init, frame);\n"
    "    slot[s * FS_SLOT4 + 7] = float4(mpass == 0 ? frame : o7.x, ys, 0.0, 0.0);\n"   // last in view; the smoothed height
    "    uint b = fs_bucket((int2)floor(O.xz * 0.5)), pos;\n"
    "    InterlockedAdd(bcnt[b], 1, pos);\n"
    "    if (pos < 12) InterlockedOr(bids[6 * b + pos / 2], s << (16 * (pos & 1))); else InterlockedAdd(nover, 1);\n"
    "    if (fresh) InterlockedAdd(nfresh, 1);\n"
    "  }\n"
    // a flag not gathered this frame but seen in the last 10 (a draw missed, a pass skipped): still in the lookup, at its
    // last pole, so the shader keeps it on its cloth instead of falling back to the wave
    "  if (g < FS_SLOTS && mclaim[g] == 0xFFFFFFFFu) {\n"
    "    float4 s0 = slot[g * FS_SLOT4];\n"
    "    if (s0.w > 0.0 && s0.w >= frame - 10.0) {\n"
    "      uint b = fs_bucket((int2)floor(s0.xz * 0.5)), pos;\n"
    "      InterlockedAdd(bcnt[b], 1, pos);\n"
    "      if (pos < 12) InterlockedOr(bids[6 * b + pos / 2], g << (16 * (pos & 1))); else InterlockedAdd(nover, 1);\n"
    "    }\n"
    "  }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  if (g < FS_NB) {\n"
    "    slot[FS_SLOTS * FS_SLOT4 + 1 + 2 * g] = float4(asfloat(bcnt[g]), asfloat(bids[6 * g]), asfloat(bids[6 * g + 1]), asfloat(bids[6 * g + 2]));\n"
    "    slot[FS_SLOTS * FS_SLOT4 + 2 + 2 * g] = float4(asfloat(bids[6 * g + 3]), asfloat(bids[6 * g + 4]), asfloat(bids[6 * g + 5]), 0.0);\n"
    "  }\n"
    // header: the frame, then running totals of new claims and flags left out of the lookup (the plugin logs them)
    "  if (g == 0) { float4 hd = slot[FS_SLOTS * FS_SLOT4]; slot[FS_SLOTS * FS_SLOT4] = float4(frame, asfloat(asuint(hd.y) + nfresh), asfloat(asuint(hd.z) + nover), 0.0); }\n"
    "}\n"
    "groupshared float3 gp[FS_N];\n"
    "groupshared float4 gs0, gs5;\n"   // the slot's first row via groupshared: what decides the group's flow must be uniform
    // value noise in 3D (hash of the lattice, smooth interpolation), -1..1
    "float fs_hash(float3 p) { p = frac(p * 0.3183099 + 0.1); p *= 17.0; return frac(p.x * p.y * p.z * (p.x + p.y + p.z)); }\n"
    "float fs_noise(float3 x) {\n"
    "  float3 i = floor(x), f = frac(x); f = f * f * (3.0 - 2.0 * f);\n"
    "  return 2.0 * lerp(lerp(lerp(fs_hash(i), fs_hash(i + float3(1, 0, 0)), f.x), lerp(fs_hash(i + float3(0, 1, 0)), fs_hash(i + float3(1, 1, 0)), f.x), f.y),\n"
    "                    lerp(lerp(fs_hash(i + float3(0, 0, 1)), fs_hash(i + float3(1, 0, 1)), f.x), lerp(fs_hash(i + float3(0, 1, 1)), fs_hash(i + float3(1, 1, 1)), f.x), f.y), f.z) - 1.0; }\n"
    // curl of a noise vector potential (finite differences): swirling, divergence-free turbulence
    "float3 fs_curl(float3 p) {\n"
    "  const float e = 0.15;\n"
    "  float3 o1 = float3(31.4, 0, 0), o2 = float3(0, 47.2, 0), o3 = float3(0, 0, 19.7);\n"
    "  float ax = fs_noise(p + o1), ay = fs_noise(p + o2), az = fs_noise(p + o3);\n"
    "  float ax_y = fs_noise(p + o1 + float3(0, e, 0)), ax_z = fs_noise(p + o1 + float3(0, 0, e));\n"
    "  float ay_x = fs_noise(p + o2 + float3(e, 0, 0)), ay_z = fs_noise(p + o2 + float3(0, 0, e));\n"
    "  float az_x = fs_noise(p + o3 + float3(e, 0, 0)), az_y = fs_noise(p + o3 + float3(0, e, 0));\n"
    "  return float3((az_y - az) - (ay_z - ay), (ax_z - ax) - (az_x - az), (ay_x - ay) - (ax_y - ax)) / e; }\n"
    // Real flags (notes/banners.md): limp and folded in still air; above a low wind speed they flutter, a wave running
    // from the pole to the tip, faster as the wind rises (frequency ~ St U / L). v8 (2026-11-12, the user's "do 1-6"):
    //  - many small steps, one constraint pass each (Macklin et al. 2019, "Small Steps"), the stretch links solved in
    //    four non-touching groups one after the other (graph-coloured Gauss-Seidel: a grid needs 2 colours per direction),
    //    shear and bend (soft) in one parallel pass;
    //  - the wind: the game's direction; its strength varies through a world-space noise field carried downwind at the
    //    mean wind speed (gusts roll across the army, Ghost of Tsushima's approach), plus curl-noise eddies a few metres
    //    across (Bridson et al. 2007), both sampled once a frame per particle;
    //  - air: pressure along the cloth's normal (0.5 rho U^2 / cloth mass: ~240 m/s^2 face-on at 10 m/s), lift across the
    //    flow (billowing), skin drag, still-air damping of slow cloth;
    //  - detail by distance from the game's camera: every flag steps every frame (skipping frames far away froze and
    //    jumped them, for 0.06 ms saved), with 16 substeps within 100 m, 10 to 250 m, 6 beyond.
    "[numthreads(FS_N, 1, 1)] void step(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {\n"
    "  uint s = gid.x; float frame = sim0.w;\n"
    "  if (tid == 0) { gs0 = slot[s * FS_SLOT4]; gs5 = slot[s * FS_SLOT4 + 5]; }\n"
    "  GroupMemoryBarrierWithGroupSync();\n"
    "  float4 s0 = gs0;\n"
    "  if (s0.w <= 0.0 || s0.w < frame - 10.0) return;\n"   // the whole group: not seen in the last 10 frames
    "  float4 r0 = slot[s * FS_SLOT4 + 1], r1 = slot[s * FS_SLOT4 + 2], r2 = slot[s * FS_SLOT4 + 3];\n"
    "  float4 p4 = slot[s * FS_SLOT4 + 4], p5 = slot[s * FS_SLOT4 + 5];\n"
    "  int i = (int)(tid % FS_GX), j = (int)(tid / FS_GX);\n"
    "  float dx = (p4.y - p4.x) / (FS_GX - 1), dy = p4.z / (FS_GY - 1);\n"
    // the model's own shape: along the pole (x), out along y (side +-1) or z (side +-2: lance pennants)
    "  bool az = abs(p4.w) > 1.5; float sd = p4.w < 0.0 ? -1.0 : 1.0;\n"
    "  float4 L = az ? float4(p4.x + dx * i, p5.x, p5.y + sd * dy * j, 1.0) : float4(p4.x + dx * i, p5.x + sd * dy * j, p5.y, 1.0);\n"
    "  float3 rest = float3(dot(r0, L), dot(r1, L), dot(r2, L));\n"
    "  float rx = dx * length(float3(r0.x, r1.x, r2.x)), ry = dy * (az ? length(float3(r0.z, r1.z, r2.z)) : length(float3(r0.y, r1.y, r2.y))), rd = sqrt(rx * rx + ry * ry);\n"
    "  uint gi = s * FS_N + tid, gv = FS_SLOTS * FS_N + gi;\n"
    "  float3 pos = grid[gi].xyz, prev = grid[gv].xyz;\n"
    // a new flag starts from the model's own shape, a couple of millimetres uneven (a perfectly flat sheet never buckles)
    "  if (p5.z != 0.0) { pos = rest + (j > 0 ? float3(sin(tid * 1.7), cos(tid * 2.9), cos(tid * 2.3)) * 0.002 : 0.0); prev = pos; }\n"
    // v7 cloth (setting D; notes/banners.md): restored 2026-11-12 after the user compared - v8's rolling gusts, curl
    // eddies and lift were too much for 0.5 m lance pennants; the v8 fixes around it (matching, rigid follow) stay
    "  const int SUB = 6;\n"
    "  float h = sim0.y / SUB, t = sim0.z, ph = dot(s0.xyz, float3(0.13, 0.0, 0.17));\n"
    "  float wl = length(wind0.xy);\n"
    "  float3 wdir = wl > 1e-4 ? float3(wind0.x, 0.0, wind0.y) / wl : 0.0;\n"
    "  float3 wside = float3(-wdir.z, 0.0, wdir.x);\n"
    "  float str = saturate(wind1.x) * saturate(wl * 4.0);\n"
    "  [loop] for (int sub = 0; sub < SUB; sub++) {\n"
    "    float tt = t + h * sub;\n"
    // gusts change the wind's speed, 60-140 %, over several seconds and quicker, different for every flag
    "    float gust = 1.0 + tune1.w * (sin(tt * 0.8 + ph) + 0.4 * sin(tt * 1.9 + ph * 1.7) + 0.2 * sin(tt * 4.7 + ph * 2.3));\n"
    "    float U = tune0.x * str * gust;\n"
    // turbulence: small sideways and up-down swirls travelling with the wind (they start the flutter)
    "    float sw = tt * (2.0 + U * 0.6) - (j * dy) * 2.5 + ph;\n"
    "    float3 wv = wdir * U + (wside * sin(sw + i * 0.4) + float3(0.0, 0.6, 0.0) * sin(sw * 1.3 + 1.1)) * U * tune1.x;\n"
    "    gp[tid] = pos;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    int il = i > 0 ? tid - 1 : tid, ir = i < FS_GX - 1 ? tid + 1 : tid, jd = j > 0 ? tid - FS_GX : tid, ju = j < FS_GY - 1 ? tid + FS_GX : tid;\n"
    "    float3 n = cross(gp[ir] - gp[il], gp[ju] - gp[jd]);\n"
    "    float nl = length(n); n = nl > 1e-6 ? n / nl : float3(0.0, 1.0, 0.0);\n"
    "    float3 vr = wv - (pos - prev) / h;\n"
    "    float vm = length(vr);\n"
    "    float3 acc = float3(0.0, -9.81, 0.0) + n * dot(vr, n) * vm * tune0.y + vr * (vm * tune0.z + tune0.w * (1.0 + 4.0 * saturate(1.0 - U)));\n"   // + skin drag, and still-air damping of slow cloth (up to five times as much under 1 m/s of wind: a calm banner settles)
    "    float3 np = pos + (pos - prev) * 0.998 + acc * h * h;\n"
    "    prev = j == 0 ? rest : pos;\n"
    "    pos = j == 0 ? rest : np;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    [loop] for (int it = 0; it < 6; it++) {\n"
    "      gp[tid] = pos;\n"
    "      GroupMemoryBarrierWithGroupSync();\n"
    "      float3 c = 0.0; float cnt = 0.0;\n"
    "      [unroll] for (int k = 0; k < 12; k++) {\n"
    "        int2 o = k < 4 ? int2(k == 0 ? 1 : k == 1 ? -1 : 0, k == 2 ? 1 : k == 3 ? -1 : 0)\n"
    "               : k < 8 ? int2((k & 1) ? 1 : -1, (k & 2) ? 1 : -1)\n"
    "               : int2(k == 8 ? 2 : k == 9 ? -2 : 0, k == 10 ? 2 : k == 11 ? -2 : 0);\n"
    "        float rl = k < 4 ? (o.x != 0 ? rx : ry) : k < 8 ? rd : (o.x != 0 ? 2.0 * rx : 2.0 * ry);\n"
    "        float wgt = k < 4 ? 1.0 : k < 8 ? tune1.y : tune1.z;\n"   // stretch links firm, shear soft, bending very soft
    "        int ni = i + o.x, nj = j + o.y;\n"
    "        if (ni < 0 || ni >= FS_GX || nj < 0 || nj >= FS_GY) continue;\n"
    "        float3 dd = gp[nj * FS_GX + ni] - pos; float len = length(dd);\n"
    "        if (len < 1e-6) continue;\n"
    // cloth resists stretching, hardly squashing (so it can crumple and hang); a pinned neighbour does not move: the whole
    // correction here
    "        c += dd * ((len - rl) / len) * (len < rl ? 0.05 : 1.0) * (nj == 0 ? 1.0 : 0.5) * wgt; cnt += wgt;\n"
    "      }\n"
    "      if (j > 0 && cnt > 0.0) pos += c / cnt;\n"
    "      GroupMemoryBarrierWithGroupSync();\n"
    "    }\n"
    // (a follow-the-leader no-stretch pass was tried: it kept a calm flag swinging round the pole, with or without
    // Mueller's damping; without it the links hold full-wind stretch to about 2 %)
    "  }\n"
    "  grid[gi] = float4(pos, 0.0); grid[gv] = float4(prev, 0.0);\n"
    "  if (tid == 0 && p5.z != 0.0) slot[s * FS_SLOT4 + 5].z = 0.0;\n"
    "  if (tid == 0) { slot[s * FS_SLOT4 + 8] = r0; slot[s * FS_SLOT4 + 9] = r1; slot[s * FS_SLOT4 + 10] = r2; }\n"
    "}\n";
