// bridge64_sites.h: the game's code and data the upscaling add-on's 64-bit glue uses (bridge64_tail.inc), in one place
// so that tests/bridge64_test.c checks the same numbers against the Linux game file (SHA-256 217e1435...8784).
#pragma once
#include <stdint.h>
#define B64_PROCESS 0x30ce1c0u    // WARSCAPE::SCENE_VIEW::process(DEVICE_CONTEXT&, SCENE_VIEWPORT const*, RENDER_TARGET&, faces, bool, bool)
#define B64_PROCESS_INTERNAL 0x30ce260u   // what it calls per index (vtable +0x1a8); the loader hooks this one, not process
#define B64_CAMERA 0x30c55c0u     // WARSCAPE::DEFERRED_PIPELINE_STATE::set_shared_camera(CAMERA const&, SHARED_CAMERA_SHADER&, SCENE_VIEW*)
#define B64_VIEWPROJ 0x7501a0u    // UTILITYLIB::CAMERA::view_projection() const (MATRIX_44 by hidden pointer)
#define B64_FRUSTUM 0x3011970u    // UTILITYLIB::CAMERA::frustum() const: the position at +0x60, the view-projection at +0x6C (bridge64_test)
#define B64_CAMERA_CTOR 0x300ed40u   // UTILITYLIB::CAMERA::CAMERA(VECTOR_3 const& x3, float x5) (bridge64_test)
#define B64_MEMCPY_PLT 0x409c60u
static const uint8_t B64_PRO[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // push rbp; push r15; push r14: both entries
static const uint8_t B64_VP_PRO[10] = {0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00};   // push r14; push rbx; sub rsp, 0x88
// FNV-1a of each function's bytes in the game file
#define B64_PROCESS_FNV 0x791a3702u   // 0x94 bytes
#define B64_CAMERA_FNV 0x92088c8cu    // first 0x40 bytes
#define B64_VIEWPROJ_FNV 0x02f2a373u  // first 0x40 bytes
// the record copies of WEIGHTED_INSTANCE (units, horses): call memcpy@plt, record = item + 0x88 (32-bit + 0x74)
static const struct { uint32_t call, fn, len, fnv; const char *what; } B64_COPY[3] = {
    {0x354ca53u, 0x354ca10u, 0x5d, 0xa9409052u, "TBB_RIGID_WRITE_MASTER_LIST_INSTANCE_DATA<WEIGHTED_INSTANCE> (the 32-bit FUN_117dab80)"},
    {0x353f088u, 0x353ef60u, 0x14b, 0xc1401467u, "INSTANCE_DATA_STREAM<WEIGHTED_INSTANCE>::fill_instance_data"},
    {0x353f1d8u, 0x353f0b0u, 0x14b, 0x3d4667f2u, "its this-adjusting copy"}};
// WEIGHTED_INSTANCE fields fgmotion64.c reads (tools/gen_bridge64.py): fill_animation_texture 0x3416d50 reads the pose
// pointer at +0x70 (mov r13, [rax+0x70] at 0x3416def) and writes the texture index at +0x6c (mov [rax+0x6c], esi at 0x3416e12)
#define B64_ANIM_POSE_AT 0x3416defu
#define B64_ANIM_TEX_AT 0x3416e12u
// the stage vtables the bridge tells apart, by class: vptr (vtable symbol + 0x10) and the name the game file carries.
// Order: main view, HUD-less point, overlay, the three depth prepasses, effects, after effects (bridge64_tail.inc's slots)
static const struct { uint32_t vptr; const char *mangled; } B64_VT[8] = {
    {0x3c3d910u, "N8WARSCAPE12GBUFFER_VIEWE"},                     // 32-bit +0x1CCE4E4 "GBUFFER"
    {0x3bec000u, "N8WARSCAPE23UI3D_ALPHA_OVERLAY_VIEWE"},           // +0x1C9A900: the HUD-less point
    {0x3bf4220u, "N8WARSCAPE3SEA19COLOUR_OVERLAY_VIEWE"},       // +0x1CA0858 "Sea::Colour Overlay"
    {0x3c3e240u, "N8WARSCAPE18GRASS_PREFILL_VIEWE"},             // +0x1CCE3E0
    {0x3c3df50u, "N8WARSCAPE10GRASS_VIEWE"},                     // +0x1CCE2E4
    {0x3bf0ef0u, "N8WARSCAPE13ZPREFILL_VIEWE"},                  // +0x1CA0404
    {0x3c42bd0u, "N8WARSCAPE19DEFERRED_ALPHA_VIEWE"},                // +0x1C99FB8
    {0x3bed9c0u, "N8WARSCAPE63SCENE_DEPTH_BUFFER_ANALYSIS_SHADOW_MAP_DOWNSIZE_STORE_DATA_VIEWE"}};   // +0x1C9D064
