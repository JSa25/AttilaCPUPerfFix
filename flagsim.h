// Flag cloth simulation, D3D11 side (flagsim.c)
#pragma once
#include <d3d11.h>
int fs_scan(const void *buf, const void *data, UINT size);                      // marker vertices in a new vertex buffer's data
int fs_models(void);                                                              // flag meshes known
const float *fs_is_flag(const void *buf, UINT byte_off);                          // the model's 6 floats if a flag mesh starts there
int fs_init(ID3D11Device *dev, char *err, int errlen);
void fs_gather(ID3D11DeviceContext *c, ID3D11Buffer *inst, UINT byte_off, UINT n, const float *prm);   // one flag draw's records
unsigned fs_run(ID3D11DeviceContext *c, ID3D11Buffer *wind, ID3D11Buffer *cam, float dt, float t);   // simulate; cam: the game's camera block or NULL
void fs_tune2(const float *four);                                                  // substeps, lift, gust size, eddy size
void fs_tune(const float *eight);                                                  // wind scale, pressure, drag, damping; turbulence, shear, bend, gusts
void fs_vs_views(ID3D11ShaderResourceView **two);                                 // for VS slots t14, t15
ID3D11Buffer *fs_debug_buffer(int which);                                         // 0 slots, 1 grid (tests)
int fs_stats(ID3D11DeviceContext *c, unsigned out[2]);                            // running totals: new claims, flags left out of the lookup
unsigned fs_dropped(unsigned *peak_wanted);                                       // instances FS_GATHER had no room for; the busiest frame's demand
int fs_record_start(const wchar_t *path, int frames);                                 // flight recorder: the next frames to a file (flagreplay.c)
int fs_recording(void);
extern int fs_pass;                                                                // 0 main view, 1 shadow pass (set before fs_gather)
extern const void *fs_cs_defines;                                                  // tests: macros that leave wind features out (cost)
extern ID3D11Query *fs_bench_ts[3];                                               // tests: GPU timestamps around the passes
