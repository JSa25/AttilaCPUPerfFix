// Flag cloth simulation, D3D11 side (flagsim.c)
#pragma once
#include <d3d11.h>
int fs_scan(const void *buf, const void *data, UINT size);                      // marker vertices in a new vertex buffer's data
void fs_forget(const void *buf);                                                  // a buffer was created at this address: what was known of it is gone
int fs_models(void);                                                              // flag meshes known
const float *fs_is_flag(const void *buf, UINT byte_off);                          // the model's 6 floats if a flag mesh starts there
int fs_buf_has_flags(const void *buf);                                            // the buffer holds a flag mesh somewhere
int fs_init(ID3D11Device *dev, char *err, int errlen);
int fs_cs_from_bytecode(void);                                                    // 1: fs_init took the build's compute bytecode, 0: it compiled the HLSL
void fs_gather(ID3D11DeviceContext *c, ID3D11Buffer *inst, UINT byte_off, UINT n, const float *prm);   // one flag draw's records
unsigned fs_run(ID3D11DeviceContext *c, ID3D11Buffer *wind, ID3D11Buffer *cam, float dt, float t);   // simulate; cam: the game's camera block or NULL
void fs_tune2(const float *four);                                                  // substeps, lift, gust size, eddy size
void fs_tune(const float *eight);                                                  // wind scale, pressure, drag, damping; turbulence, shear, bend, gusts
void fs_vs_views(ID3D11ShaderResourceView **two);                                 // for VS slots t14, t15
ID3D11Buffer *fs_debug_buffer(int which);                                         // 0 slots, 1 grid (tests)
int fs_stats(ID3D11DeviceContext *c, unsigned out[2]);                            // running totals: new claims, flags left out of the lookup
// What each pass asked for, frame by frame (flagsim.c). Separate per pass because they are not equivalent: the main
// view fills the gather list first and the shadow pass only gets what is left, so over[0] counts the frames where a
// flag ON SCREEN went without cloth, while over[1] / shadow_squeezed are frames where only shadows did.
enum { FS_DM_BUCKETS = 17, FS_DM_STEP = 128 };            // 0..127, 128..255, ... , 2048 and over
struct fs_demand {
    unsigned frames;
    unsigned hist[2][FS_DM_BUCKETS];                      // [0] main view, [1] shadow: frames by instances asked for
    unsigned peak[2], over[2];                            // busiest frame per pass; frames that pass asked for over FS_GATHER
    unsigned shadow_squeezed;                             // frames the main view left the shadow pass less than it asked
    unsigned peak_got_main;                               // most the main view ever got (FS_GATHER means it filled it)
    unsigned dropped;                                     // instances with no room, both passes, since the start
};
void fs_demand_stats(struct fs_demand *out);
int fs_record_start(const wchar_t *path, int frames);                                 // flight recorder: the next frames to a file (flagreplay.c)
int fs_recording(void);
extern int fs_shadow_gather;       // 1: shadow draws take gather slots too (the old behaviour, for an A/B)
extern int fs_pass;                                                                // 0 main view, 1 shadow pass (set before fs_gather)
extern const void *fs_cs_defines;                                                  // tests: macros that leave wind features out (cost)
extern ID3D11Query *fs_bench_ts[3];                                               // tests: GPU timestamps around the passes
