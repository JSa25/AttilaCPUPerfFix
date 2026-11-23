// Part of bridgeprobe.c (included there, not compiled on its own): bloom_src: the bloom source pass with the jitter taken out (bpglue_test.c builds this file too).
// bloom_src: the game's shader (disassembled from data.pack) with j added (the jitter in the source's uv) and the samples
// kept inside the frame (maxuv: its last pixel's centre)
static const char bloom_src[] =
    "Texture2D<float4> t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "cbuffer cam : register(b0) { float4 cam[29]; }\n"   // camera_VS_PS: g_vpos_texel_offset at [28].x
    "cbuffer dims : register(b1) { float2 recip; }\n"    // render_target_dims_buffer: 1 / the target's size
    "cbuffer jit : register(b2) { float2 j, maxuv; }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 uv = (pos.xy + cam[28].x) * recip + j, d = recip * 0.25;\n"
    "  float3 a = 0;\n"
    "  [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) a += t.Sample(s, min(uv + float2(x, y) * d, maxuv)).rgb;\n"
    "  a *= 0.111111;\n"
    "  return float4(a, dot(a, float3(0.2126, 0.7152, 0.0722)));\n"
    "}\n";
