// Per-object motion vectors for the frame bridge (dev only; DLSS_PLAN "Per-object motion vectors", AGENT_LOG 97-99).
// Attila's animated models (units, horses, campaign armies / agents) are drawn instanced by three main-view vertex shaders
// that skin from an animation texture and a 120-byte instance record per model (world matrix rows at 0-47, bone base at
// 48). The motion pass draws those models again, after the camera-motion pass and before the HUD-less copy, with our
// shaders (motionvs.h: the game's skinning repeated for this frame and the previous one, bit-identical to the game's,
// skintest.c) into the motion vector target, depth test EQUAL against the game's depth through the same (jittered)
// viewport. What it needs from the previous frame:
// - the animation textures: big battles use several (the model's texture index is item+0x5C), all copied at the frame's
//   end into the slices of one texture array;
// - each model's previous world matrix, bone base and texture index, kept per pose (item+0x60) from the frame before;
// - the camera: a GPU copy of the game's camera_VS_PS block of the first animated draw of each frame.
// Costs (battle run, AGENT_LOG 99): the game copies 22,000-48,000 records per frame for all its passes, so the copy hook
// (FUN_117dab80's memcpy at +0x17DABAF, any thread) only notes which item went to which record slot; the main-view draws
// queue their items, and one worker thread builds the previous records while the game renders the rest of the frame.
// Swaying trees and grass (AGENT_LOG 178): drawn again the same way with windvs.h, the game's wind at this frame's and
// the previous frame's time_in_sec (in the camera block copies) - no history of their own; the wind block is copied once a
// frame. Their draws come from the main view and its depth prepasses (bridgeprobe.c), with the game's buffers as they are.
#define COBJMACROS
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <intrin.h>
#include "motionvs.h"
#include "windvs.h"

static const GUID iid_device1 = {0xa04bfb29, 0x08ef, 0x43d6, {0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86}};
static const GUID iid_context1 = {0xbb2c6faa, 0xb5fb, 0x4082, {0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1}};

static ID3D11Device *m_dev;
static ID3D11DeviceContext *m_ctx;
static ID3D11DeviceContext1 *m_ctx1;
static ID3DDeviceContextState *m_state, *m_game_state;
static void (*m_log)(const char *fmt, ...);

static int m_on;
static volatile int m_enabled = 1;   // the F11 panel's switch (the copy hook stays, returning at once when off)
void fgm_set_enabled(int on) { m_enabled = on; }
// models and trees whose own motion this frame stays under px render pixels are not redrawn, and triangles whose three
// corners all moved less are dropped (0: all drawn; the shaders' cull_px). Rome 2 measured 0.93-1.23 -> 0.52 ms per pass.
static float m_cull_px = 0.125f;
void fgm_set_cull(float px) { m_cull_px = px < 0 ? 0 : px; }
float fgm_cull(void) { return m_cull_px; }

// ---- the skinned layouts (CreateInputLayout with one of the three shaders' bytecode) ----
// the game's layout is kept referenced (AddRef): a freed one's address could otherwise come back as another layout and
// match an old entry, drawing an unrelated model with our skin or wind shader (ultrareview 2026-11-02); 256 entries, a
// full table logged once (new models then get the camera's motion only)
#define M_MAX_LAYOUTS 256
static struct { const void *game; ID3D11InputLayout *ours; int skin4, wind; } m_il[M_MAX_LAYOUTS];   // wind: 1 / 2 trees, 3 grass
static volatile LONG m_nil;
static ID3D11VertexShader *m_vs[2];
static ID3D11PixelShader *m_ps;
static ID3DBlob *m_vsb[2];
static ID3D11VertexShader *m_wvs[3];   // windvs.h: tree_a, tree_b, grass
static ID3DBlob *m_wvsb[3];

// the game's vertex shaders whose models sway (windvs.h; the same code under several hashes)
static int wind_kind(unsigned long long sig) {
    if (sig == 0xd9228831741dc68eull || sig == 0x6cd77149cd64a265ull || sig == 0x8b29150b6008e5dfull) return 1;
    if (sig == 0x0099406c17c7aff8ull || sig == 0x25b8fe1fa9738fe2ull || sig == 0xe8899e58144778f2ull) return 2;
    if (sig == 0xc272bdade21308b2ull || sig == 0x20bbbad818a85073ull) return 3;
    return 0;
}
static void layouts_full(void) {
    static volatile LONG said;
    if (!InterlockedExchange(&said, 1)) m_log("motion pass: %d input layouts recorded - models loaded from now get the camera's motion only", M_MAX_LAYOUTS);
}
void fgm_layout_created(const void *il, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, unsigned long long sig) {
    int wind = wind_kind(sig);
    if (wind) {   // the game's elements as they are: the wind needs no history of its own
        ID3D11InputLayout *ours = NULL;
        if (!m_on || n > 16 || FAILED(ID3D11Device_CreateInputLayout(m_dev, e, n, ID3D10Blob_GetBufferPointer(m_wvsb[wind - 1]),
                                                                       ID3D10Blob_GetBufferSize(m_wvsb[wind - 1]), &ours))) {
            if (m_on) m_log("motion pass: input layout for shader %016llx (wind) not created", sig);
            return;
        }
        LONG k = InterlockedIncrement(&m_nil) - 1;
        if (k >= M_MAX_LAYOUTS) { ID3D11InputLayout_Release(ours); layouts_full(); return; }
        ID3D11InputLayout_AddRef((ID3D11InputLayout *)il);
        m_il[k].ours = ours; m_il[k].skin4 = 0; m_il[k].wind = wind; m_il[k].game = il;
        return;
    }
    // 4b23a601: a51431eb with dismemberment, whose cut factors only reach the pixel shader (skintest: bit-identical)
    int skin4 = sig == 0x520dbe181fbd767aull || sig == 0x3d52a1037cfa96abull;
    if (!m_on || (!skin4 && sig != 0xa51431ebf7d15a22ull && sig != 0x4b23a601f0f7b19bull) || n > 16) return;
    D3D11_INPUT_ELEMENT_DESC el[20];
    memcpy(el, e, n * sizeof *e);
    static const char prev[] = "PREV";
    for (UINT k = 0; k < 4; k++) {
        D3D11_INPUT_ELEMENT_DESC p = {prev, k, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 16 * k, D3D11_INPUT_PER_INSTANCE_DATA, 1};
        el[n + k] = p;
    }
    ID3D11InputLayout *ours = NULL;
    if (FAILED(ID3D11Device_CreateInputLayout(m_dev, el, n + 4, ID3D10Blob_GetBufferPointer(m_vsb[skin4]), ID3D10Blob_GetBufferSize(m_vsb[skin4]),
                                              &ours))) {
        m_log("motion pass: input layout for shader %016llx not created", sig);
        return;
    }
    LONG k = InterlockedIncrement(&m_nil) - 1;
    if (k >= M_MAX_LAYOUTS) { ID3D11InputLayout_Release(ours); layouts_full(); return; }
    ID3D11InputLayout_AddRef((ID3D11InputLayout *)il);
    m_il[k].ours = ours; m_il[k].skin4 = skin4; m_il[k].wind = 0; m_il[k].game = il;
}

// ---- instance buffers (Map hook) and, per 120-byte record slot, the item whose record the game copied there ----
// campaign run (AGENT_LOG 100): after a battle the list was full of the battle's freed buffers, so every campaign buffer
// went untracked -> 32 entries, and a full list gives the entry mapped longest ago to the new buffer
#define M_MAPS 32
// A slot is trusted only if written this frame and since the buffer's last DISCARD map (gen). 6.4 players' crashes in
// fgm_draw (+0x16915 / +0x16AE2 / +0x16AF4, settlement battles at the end of loading or the first camera move): a slot
// the game did not refill through the copy hook kept an item pointer from an earlier frame, whose unit had been freed.
typedef struct { const void *item; unsigned gen; LONG frame; } MSlot;
static LONG m_frame = 1;   // the frame being built (fgm_frame_end counts it)
// A table holds its slot count: the copy hook bounds its index by the table it read, not by the entry's size, which a
// swap to a smaller buffer may have changed in between (ultrareview 2026-11-02: heap overflow). Replaced tables are
// freed M_RETIRE_FRAMES frames later, when no copy on a job thread can still be writing to them (they leaked before).
typedef struct { UINT n; MSlot s[]; } MTable;
typedef struct { ID3D11Resource *res; unsigned char *base; UINT size; MTable *tab; unsigned gen; } MMap;
#define M_RETIRED 64
#define M_RETIRE_FRAMES 4
static struct { MTable *t; LONG frame; } m_retired[M_RETIRED];   // main thread only (Map hook, frame end)
static unsigned m_retire_next, m_retire_leaked;
static void retire(MTable *t) {
    unsigned i = m_retire_next++ % M_RETIRED;
    if (m_retired[i].t) {   // the ring is full: free the oldest if it is old enough, else leave it (counted)
        if (m_retired[i].frame + M_RETIRE_FRAMES <= m_frame) free(m_retired[i].t);
        else m_retire_leaked++;
    }
    m_retired[i].t = t;
    m_retired[i].frame = m_frame;
}
static void retire_free(void) {
    for (unsigned i = 0; i < M_RETIRED; i++)
        if (m_retired[i].t && m_retired[i].frame + M_RETIRE_FRAMES <= m_frame) { free(m_retired[i].t); m_retired[i].t = NULL; }
}
static MMap m_maps[M_MAPS];
static unsigned long long m_map_last[M_MAPS];   // apart from m_maps: written at every Map, which the copy hook's threads read
static CRITICAL_SECTION m_map_lock;
static unsigned m_maps_full;
static volatile LONG m_copies, m_copies_unmapped, m_copies_misaligned;

void fgm_mapped(ID3D11Resource *r, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE *m) {
    if (!m_on || !m || !m->pData) return;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &bd);
    if (!(bd.BindFlags & D3D11_BIND_VERTEX_BUFFER) || bd.Usage != D3D11_USAGE_DYNAMIC || bd.ByteWidth < (64u << 10)) return;
    static unsigned long long clock;
    EnterCriticalSection(&m_map_lock);
    int k = -1;
    for (int i = 0; i < M_MAPS; i++) if (m_maps[i].res == r || (k < 0 && !m_maps[i].res)) { k = i; if (m_maps[i].res == r) break; }
    if (k < 0) {   // full: the entry mapped longest ago
        m_maps_full++;
        k = 0;
        for (int i = 1; i < M_MAPS; i++) if (m_map_last[i] < m_map_last[k]) k = i;
        m_maps[k].base = NULL;   // the copy hook stops matching it before it changes
    }
    // a buffer of another size: a new table; the old one retires (a copy on a worker may still be writing to it)
    if (m_maps[k].tab && m_maps[k].size != bd.ByteWidth) { retire(m_maps[k].tab); m_maps[k].tab = NULL; }
    if (!m_maps[k].tab) {
        UINT n = bd.ByteWidth / 120 + 1;
        MTable *t = calloc(1, sizeof *t + n * sizeof(MSlot));
        if (t) t->n = n;
        __atomic_store_n(&m_maps[k].tab, t, __ATOMIC_RELEASE);
    }
    if (type == D3D11_MAP_WRITE_DISCARD || m_maps[k].res != r) m_maps[k].gen++;   // the buffer's earlier contents are gone
    if (m_maps[k].size != bd.ByteWidth) m_maps[k].size = bd.ByteWidth;   // stores only on a change: the copy hook's threads read these
    if (m_maps[k].res != r) m_maps[k].res = r;
    m_map_last[k] = ++clock;
    if (m_maps[k].tab) m_maps[k].base = m->pData;   // last: the copy hook looks for base
    LeaveCriticalSection(&m_map_lock);
}

// The copy counters are per thread, added to the shared ones every 1024 copies: the game's job threads all run this (~30,000
// calls per frame on a clump), and a locked increment of one shared counter per call was 5-7% of the main thread alone
// (AGENT_LOG 109: the cache line bounced between the threads)
static __thread LONG t_copies, t_misaligned, t_unmapped;
static void count(volatile LONG *shared, LONG *mine) {
    if (++*mine == 1024) { InterlockedExchangeAdd(shared, 1024); *mine = 0; }
}
// the game's record copy (FUN_117dab80's memcpy call at +0x17DABAF), any thread: only note the item
void *__cdecl fgm_copy(void *dst, const void *src, size_t n) {
    memcpy(dst, src, n);
    if (!m_on || !m_enabled || n != 120) return dst;
    count(&m_copies, &t_copies);
    for (int i = 0; i < M_MAPS; i++) {
        MMap *m = &m_maps[i];
        if (m->base && (unsigned char *)dst >= m->base && (unsigned char *)dst + 120 <= m->base + m->size) {
            UINT off = (UINT)((unsigned char *)dst - m->base);   // records start anywhere (64% not at a multiple of 120)
            if (off % 120) count(&m_copies_misaligned, &t_misaligned);
            MTable *t = __atomic_load_n(&m->tab, __ATOMIC_ACQUIRE);
            if (t && off / 120 < t->n) {   // records never overlap: one slot each; bounded by the table read here
                t->s[off / 120].item = (const unsigned char *)src - 0x74;
                t->s[off / 120].gen = m->gen;
                t->s[off / 120].frame = m_frame;
            }
            return dst;
        }
    }
    count(&m_copies_unmapped, &t_unmapped);
    return dst;
}

// ---- history by position. Run 6 (AGENT_LOG 99): the items the GPU reads are the right ones within a frame (227 of 227
// checked), but neither items nor poses stay with one model from frame to frame (pooled). So a model's previous record
// is the one from the previous frame closest to it (< 0.35 m; a model moves < 0.2 m per frame even at a charge, models
// in formation stand >= 0.6 m apart) with the same skeleton size (bone count, pose+4: a rider never takes his horse's).
// Grid of 0.5 m cells over the previous frame's models, built by the worker when a new frame starts. ----
typedef struct { float x, y, z; int bones, tex, next; float rec[13]; } MEnt;
#define M_ENTS 65536
#define M_BUCKETS 65536
#define M_CELL 0.5f
#define M_RADIUS 0.35f
static MEnt *m_ent[2];        // [frame & 1] this frame's models, [(frame - 1) & 1] the previous frame's
static int m_nent[2];
static int *m_bucket;         // previous frame: first entry per cell hash
static LONG m_grid_frame;

static unsigned cell_hash(int x, int y, int z) { return ((unsigned)x * 73856093u ^ (unsigned)y * 19349663u ^ (unsigned)z * 83492791u) & (M_BUCKETS - 1); }
static int cell(float v) { return (int)floorf(v / M_CELL); }
static void build_grid(LONG f) {   // the previous frame's models into the grid
    MEnt *e = m_ent[(f - 1) & 1];
    int n = m_nent[(f - 1) & 1];
    memset(m_bucket, 0xff, M_BUCKETS * sizeof(int));
    for (int k = 0; k < n; k++) {
        unsigned h = cell_hash(cell(e[k].x), cell(e[k].y), cell(e[k].z));
        e[k].next = m_bucket[h];
        m_bucket[h] = k;
    }
    m_nent[f & 1] = 0;
    m_grid_frame = f;
}
static const MEnt *nearest(LONG f, float x, float y, float z, int bones) {
    const MEnt *e = m_ent[(f - 1) & 1], *best = NULL;
    float bd = M_RADIUS * M_RADIUS;
    int cx = cell(x), cy = cell(y), cz = cell(z);
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++)   // at most 16 per cell: models piled at one place (zeroed / NaN) would make it n^2
                for (int k = m_bucket[cell_hash(cx + dx, cy + dy, cz + dz)], c = 0; k >= 0 && c < 16; k = e[k].next, c++) {
                    if (e[k].bones != bones) continue;
                    float ax = e[k].x - x, ay = e[k].y - y, az = e[k].z - z, d = ax * ax + ay * ay + az * az;
                    if (d < bd) { bd = d; best = &e[k]; }
                }
    return best;
}

// ---- this frame's recorded draws and the worker's queue ----
#define M_MAX_DRAWS 2048
#define M_MAX_INST 65536   // per frame (the battle run had ~17,000 in the main view)
#define M_TEX 16           // animation textures (slices of the previous-frame array; run 7: indexes >= 4 in a big battle)
typedef struct {
    int li; ID3D11Buffer *vb0, *vb1, *ib; UINT st0, of0, of1, ib_off; DXGI_FORMAT ib_fmt;
    UINT idx, inst, start_idx; INT base_v; ID3D11ShaderResourceView *anim;   // anim: NULL for the wind
    UINT st1, start_inst;   // the wind's: its instance buffer's stride, the game's first instance
    UINT first;   // index of this draw's first instance in m_items / m_prev_cpu
    unsigned gen;   // its instance buffer's fill when recorded: refilled before the pass = other records there, skipped
} MDraw;
static unsigned m_skip_refill;
static MDraw m_draws[M_MAX_DRAWS];
static UINT m_ndraws;
typedef struct { float cur[16]; int tex, bones; } MItem;   // a model's record, texture index and skeleton size
static MItem m_items[M_MAX_INST];
static float (*m_prev_cpu)[16];   // M_MAX_INST previous records (64 bytes)
static volatile LONG m_queued, m_done;   // instances handed to / finished by the worker
static HANDLE m_wake, m_thread;
static unsigned m_hit, m_new, m_badtex, m_skip_item, m_skip_range, m_skip_tex, m_skip_other;
static int m_tex_max = -1;
static unsigned long long m_worker_ticks;

static ID3D11Buffer *m_prev_buf, *m_cb, *m_cam[2], *m_wind;
static int m_cam_have[2], m_wind_have;
static unsigned m_wind_draws, m_wind_inst;
static ID3D11Resource *m_anim_res[M_TEX];          // the game's animation texture of each index
static ID3D11Resource *m_anim_bad[M_TEX];          // one that does not fit (kept referenced): logged once, not at every draw
static ID3D11Texture2D *m_prev_anim;               // their previous-frame copies (array slices)
static ID3D11ShaderResourceView *m_prev_anim_srv;
static int m_prev_ok[M_TEX];
static unsigned m_frames, m_draws_total, m_inst_total, m_skipped, m_skipped_full;
static double m_pass_ms;
static unsigned long long m_draw_ticks;

static DWORD WINAPI worker(void *arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(m_wake, INFINITE);
        LONG q;
        while ((q = m_queued) != m_done) {
            unsigned long long t0 = __rdtsc();
            LONG f = m_frame;
            if (m_grid_frame != f) build_grid(f);
            MEnt *ce = m_ent[f & 1];
            for (LONG i = m_done; i < q; i++) {
                const MItem *it = &m_items[i];   // copied at the draw (fgm_draw): the game's own records may be gone by now
                int tex = it->tex, bones = it->bones;
                const float *cur = it->cur;
                float *out = m_prev_cpu[i];
                const MEnt *h = nearest(f, cur[3], cur[7], cur[11], bones);
                if (h && h->tex >= 0 && h->tex < M_TEX && m_prev_ok[h->tex]) {
                    memcpy(out, h->rec, 52);
                    out[13] = (float)h->tex;
                    out[14] = out[15] = 0;
                    m_hit++;
                } else {   // no history: the shader takes the current pose and place (bone base < 0), only the camera moves it
                    memcpy(out, cur, 64);
                    out[12] = -1;
                    if (h) m_badtex++; else m_new++;
                }
                if (m_nent[f & 1] < M_ENTS) {
                    MEnt *e = &ce[m_nent[f & 1]++];
                    e->x = cur[3]; e->y = cur[7]; e->z = cur[11]; e->bones = bones; e->tex = tex;
                    memcpy(e->rec, cur, 52);
                }
            }
            m_worker_ticks += __rdtsc() - t0;
            InterlockedExchange(&m_done, q);
        }
    }
    return 0;
}

static void verify_draw(ID3D11Buffer *vb1, UINT off, const MSlot *items, UINT inst);   // check (below)

static void wait_worker(void) {
    for (int i = 0; m_done != m_queued; i++) if (i > 64) Sleep(0); else YieldProcessor();
}

static void cam_copy(ID3D11DeviceContext *c) {   // this frame's camera block, as the game's shader reads it
    if (m_cam_have[0]) return;
    ID3D11Buffer *cb = NULL;
    ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 1, &cb);
    if (cb) {
        D3D11_BUFFER_DESC bd;
        ID3D11Buffer_GetDesc(cb, &bd);
        if (bd.ByteWidth == 576) { ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)m_cam[0], (ID3D11Resource *)cb); m_cam_have[0] = 1; }
        ID3D11Buffer_Release(cb);
    }
}
// a swaying draw: its buffers as bound (the game's instance offsets and first instance kept), the wind block once a frame
static void wind_draw(ID3D11DeviceContext *c, int li, UINT idx, UINT inst, UINT start_idx, INT base_v, UINT start_inst) {
    if (m_ndraws >= M_MAX_DRAWS) { m_skipped_full++; return; }
    if (!m_wind_have) {
        ID3D11Buffer *cb = NULL;
        ID3D11DeviceContext_VSGetConstantBuffers(c, 1, 1, &cb);   // wind_VS: register b1 in all of them
        if (!cb) return;
        D3D11_BUFFER_DESC bd;
        ID3D11Buffer_GetDesc(cb, &bd);
        if (bd.ByteWidth >= 32) {
            D3D11_BOX box = {0, 0, 0, 32, 1, 1};
            ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)m_wind, 0, 0, 0, 0, (ID3D11Resource *)cb, 0, &box);
            m_wind_have = 1;
        }
        ID3D11Buffer_Release(cb);
        if (!m_wind_have) return;
    }
    cam_copy(c);
    if (!m_cam_have[0]) return;
    ID3D11Buffer *vb[2] = {0};
    UINT st[2], of[2];
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 2, vb, st, of);
    if (!vb[0] || !vb[1]) { if (vb[0]) ID3D11Buffer_Release(vb[0]); if (vb[1]) ID3D11Buffer_Release(vb[1]); m_skip_other++; return; }
    MDraw *d = &m_draws[m_ndraws++];
    memset(d, 0, sizeof *d);
    d->li = li; d->vb0 = vb[0]; d->st0 = st[0]; d->of0 = of[0]; d->vb1 = vb[1]; d->st1 = st[1]; d->of1 = of[1];
    ID3D11DeviceContext_IAGetIndexBuffer(c, &d->ib, &d->ib_fmt, &d->ib_off);
    d->idx = idx; d->inst = inst; d->start_idx = start_idx; d->base_v = base_v; d->start_inst = start_inst;
    m_wind_draws++;
    m_wind_inst += inst;
}

void fgm_draw(ID3D11DeviceContext *c, UINT idx, UINT inst, UINT start_idx, INT base_v, UINT start_inst) {
    if (!m_on || !m_enabled || c != m_ctx) return;
    unsigned long long t0 = __rdtsc();
    ID3D11InputLayout *il = NULL;
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    if (!il) return;
    int li = -1;
    for (LONG k = 0; k < m_nil && k < M_MAX_LAYOUTS; k++) if (m_il[k].game == il) { li = (int)k; break; }
    ID3D11InputLayout_Release(il);
    if (li < 0) return;
    if (m_il[li].wind) { wind_draw(c, li, idx, inst, start_idx, base_v, start_inst); m_draw_ticks += __rdtsc() - t0; return; }
    if (m_ndraws >= M_MAX_DRAWS || (UINT)m_queued + inst > M_MAX_INST) { m_skipped_full++; return; }
    ID3D11Buffer *vb[2] = {0};
    UINT st[2], of[2];
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 2, vb, st, of);
    const MSlot *items = NULL;
    UINT size = 0, off = of[1] + start_inst * 120;
    unsigned gen = 0;
    for (int i = 0; i < M_MAPS; i++)
        if (vb[1] && m_maps[i].res == (ID3D11Resource *)vb[1] && m_maps[i].tab) {
            items = m_maps[i].tab->s;
            size = m_maps[i].tab->n * 120 - 120 < m_maps[i].size ? m_maps[i].tab->n * 120 - 120 : m_maps[i].size;   // the table's own bound
            gen = m_maps[i].gen;
        }
    ID3D11ShaderResourceView *anim = NULL;
    ID3D11DeviceContext_VSGetShaderResources(c, 0, 1, &anim);
    int ok = vb[0] && vb[1] && st[1] == 120 && items && off + inst * 120 <= size && anim;
    if (!ok) m_skip_other++;
    else if (off + inst * 120 > size) m_skip_range++;
    for (UINT k = 0; ok && k < inst; k++) {   // every record's item, copied this frame into this fill of the buffer
        const MSlot *sl = &items[off / 120 + k];
        ok = sl->item && sl->gen == gen && sl->frame == m_frame;
    }
    if (!ok && items && off + inst * 120 <= size) m_skip_item++;
    int tex = ok ? *(const int *)((const unsigned char *)items[off / 120].item + 0x5C) : -1;
    if (ok && tex >= 0 && tex < M_TEX) {   // the texture of this index; a new one (another battle) starts without history
        ID3D11Resource *res = NULL;
        ID3D11ShaderResourceView_GetResource(anim, &res);
        if (res == m_anim_bad[tex]) tex = M_TEX;
        else if (res != m_anim_res[tex]) {
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
            m_log("motion pass: animation texture %d: %ux%u format %u%s", tex, td.Width, td.Height, td.Format, m_anim_res[tex] ? " (replaced)" : "");
            if (td.Width == 768 && td.Height == 512 && td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT && td.ArraySize == 1 && td.MipLevels == 1) {
                if (m_anim_res[tex]) ID3D11Resource_Release(m_anim_res[tex]);
                m_anim_res[tex] = res;
                res = NULL;
                m_prev_ok[tex] = 0;
            } else {   // does not fit the previous-frame slices: skipped
                if (m_anim_bad[tex]) ID3D11Resource_Release(m_anim_bad[tex]);
                m_anim_bad[tex] = res;
                res = NULL;
                tex = M_TEX;
            }
        }
        if (res) ID3D11Resource_Release(res);
    }
    if (ok && (tex < 0 || tex >= M_TEX)) m_skip_tex++;
    if (ok && tex > m_tex_max) m_tex_max = tex;
    if (!ok || tex < 0 || tex >= M_TEX) {
        m_skipped++;
        if (vb[0]) ID3D11Buffer_Release(vb[0]);
        if (vb[1]) ID3D11Buffer_Release(vb[1]);
        if (anim) ID3D11ShaderResourceView_Release(anim);
        m_draw_ticks += __rdtsc() - t0;
        return;
    }
    MDraw *d = &m_draws[m_ndraws++];
    d->li = li; d->vb0 = vb[0]; d->st0 = st[0]; d->of0 = of[0]; d->vb1 = vb[1]; d->of1 = off; d->anim = anim; d->gen = gen;
    ID3D11DeviceContext_IAGetIndexBuffer(c, &d->ib, &d->ib_fmt, &d->ib_off);
    d->idx = idx; d->inst = inst; d->start_idx = start_idx; d->base_v = base_v;
    d->first = (UINT)m_queued;
    for (UINT k = 0; k < inst; k++) {   // what the worker needs, copied now: the slots may be refilled later this frame, and
        // the game may free its records before the worker gets to them (6.3.2: end of a battle, the worker read a freed pose)
        const unsigned char *item = items[off / 120 + k].item, *pose = *(const unsigned char *const *)(item + 0x60);
        MItem *mi = &m_items[d->first + k];
        memcpy(mi->cur, item + 0x74, sizeof mi->cur);   // this frame's record, as the game copied it
        mi->tex = *(const int *)(item + 0x5C);
        // a pose field holding a small number, not a pointer (players' 6.4 crashes at +0x16AE2 read addresses 5, 0x204,
        // 6 on some maps): no skeleton, as for none. The lowest 64 KB of the address space is never mapped.
        mi->bones = (UINT_PTR)pose >= 0x10000 ? *(const int *)(pose + 4) : -1;
    }
    verify_draw(vb[1], off, &items[off / 120], inst);
    InterlockedExchangeAdd(&m_queued, (LONG)inst);
    SetEvent(m_wake);
    cam_copy(c);
    m_draws_total++;
    m_inst_total += inst;
    m_draw_ticks += __rdtsc() - t0;
}

// ---- check (bridge capture): are the items we think fed a draw the ones whose records the GPU reads? For the first 16
// animated main-view draws of the captured frame: the GPU's instance records (copied at draw time) against the items'
// records (item+0x74) read at draw time and again at pass time (the worker reads them between the two). ----
#define M_VERIFY_DRAWS 16
#define M_VERIFY_INST 64
static struct { ID3D11Buffer *stg; UINT n; const void *items[M_VERIFY_INST]; float at_draw[M_VERIFY_INST][13], at_pass[M_VERIFY_INST][13]; } m_ver[M_VERIFY_DRAWS];
static int m_verify_left, m_nver, m_verify_report;
static void verify_draw(ID3D11Buffer *vb1, UINT off, const MSlot *items, UINT inst) {
    if (m_verify_left <= 0 || m_nver >= M_VERIFY_DRAWS) return;
    m_verify_left--;
    UINT n = inst < M_VERIFY_INST ? inst : M_VERIFY_INST;
    D3D11_BUFFER_DESC bd = {n * 120, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *stg = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(m_dev, &bd, NULL, &stg))) return;
    D3D11_BOX box = {off, 0, 0, off + n * 120, 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(m_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)vb1, 0, &box);
    m_ver[m_nver].stg = stg; m_ver[m_nver].n = n;
    for (UINT k = 0; k < n; k++) {
        m_ver[m_nver].items[k] = items[k].item;
        memcpy(m_ver[m_nver].at_draw[k], (const unsigned char *)items[k].item + 0x74, 52);
    }
    m_nver++;
}
static void verify_pass(void) {   // the same items' records when the pass runs
    for (int d = 0; d < m_nver; d++)
        for (UINT k = 0; k < m_ver[d].n; k++) memcpy(m_ver[d].at_pass[k], (const unsigned char *)m_ver[d].items[k] + 0x74, 52);
    if (m_nver) m_verify_report = 1;
}
static void verify_report(void) {
    unsigned all = 0, ok_draw = 0, ok_pass = 0, ok_draw_xyz = 0;
    for (int d = 0; d < m_nver; d++) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(m_ctx, (ID3D11Resource *)m_ver[d].stg, 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT k = 0; k < m_ver[d].n; k++) {
                const float *g = (const float *)((const unsigned char *)m.pData + k * 120);
                all++;
                ok_draw += !memcmp(g, m_ver[d].at_draw[k], 52);
                ok_pass += !memcmp(g, m_ver[d].at_pass[k], 52);
                ok_draw_xyz += g[3] == m_ver[d].at_draw[k][3] && g[7] == m_ver[d].at_draw[k][7] && g[11] == m_ver[d].at_draw[k][11];
            }
            ID3D11DeviceContext_Unmap(m_ctx, (ID3D11Resource *)m_ver[d].stg, 0);
        }
        ID3D11Buffer_Release(m_ver[d].stg);
    }
    m_log("motion pass check: %u instances in %d draws: the GPU's record equals the item's record read at draw time %u (position only %u), "
          "at pass time %u", all, m_nver, ok_draw, ok_draw_xyz, ok_pass);
    m_nver = 0;
    m_verify_report = 0;
}

static void release_draws(void) {
    wait_worker();
    for (UINT i = 0; i < m_ndraws; i++) {
        MDraw *d = &m_draws[i];
        ID3D11Buffer_Release(d->vb0); ID3D11Buffer_Release(d->vb1);
        if (d->ib) ID3D11Buffer_Release(d->ib);
        if (d->anim) ID3D11ShaderResourceView_Release(d->anim);
    }
    m_ndraws = 0;
    m_queued = m_done = 0;
}

static wchar_t m_dump[MAX_PATH];   // bridge capture: a picture of the motion vectors after the pass (below)
static void dump_mv(void);

// ---- the pass: after the camera-motion pass, before the HUD-less copy is published ----
static ID3D11RenderTargetView *m_mv_rtv;
static ID3D11Resource *m_depth;
static ID3D11DepthStencilView *m_dsv;
static ID3D11DepthStencilState *m_dss;
static ID3D11RasterizerState *m_rs;

// the pipeline state the pass changes (no separate D3D11.1 context state through the UI mod's device: 0x80070057)
typedef struct {
    ID3D11InputLayout *il; ID3D11Buffer *vb[3]; UINT st[3], of[3]; ID3D11Buffer *ib; DXGI_FORMAT ibf; UINT ibo; D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader *vs; ID3D11Buffer *vcb[4]; ID3D11ShaderResourceView *vsrv[2];
    ID3D11GeometryShader *gs; ID3D11HullShader *hs; ID3D11DomainShader *ds;
    ID3D11PixelShader *ps; ID3D11Buffer *pcb;
    D3D11_VIEWPORT vp[16]; UINT nvp; ID3D11RasterizerState *rs;
    ID3D11RenderTargetView *rtv[8]; ID3D11DepthStencilView *dsv; ID3D11DepthStencilState *dss; UINT sref;
    ID3D11BlendState *bs; FLOAT bf[4]; UINT mask;
} GameState;
static void save_state(GameState *g) {
    ID3D11DeviceContext *c = m_ctx;
    ID3D11DeviceContext_IAGetInputLayout(c, &g->il);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 3, g->vb, g->st, g->of);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &g->ib, &g->ibf, &g->ibo);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &g->topo);
    ID3D11DeviceContext_VSGetShader(c, &g->vs, NULL, NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 4, g->vcb);
    ID3D11DeviceContext_VSGetShaderResources(c, 0, 2, g->vsrv);
    ID3D11DeviceContext_GSGetShader(c, &g->gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &g->hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &g->ds, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &g->ps, NULL, NULL);
    ID3D11DeviceContext_PSGetConstantBuffers(c, 0, 1, &g->pcb);
    g->nvp = 16;
    ID3D11DeviceContext_RSGetViewports(c, &g->nvp, g->vp);
    ID3D11DeviceContext_RSGetState(c, &g->rs);
    ID3D11DeviceContext_OMGetRenderTargets(c, 8, g->rtv, &g->dsv);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &g->dss, &g->sref);
    ID3D11DeviceContext_OMGetBlendState(c, &g->bs, g->bf, &g->mask);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, 0xffffffff);
}
#define REL(x) do { if (x) IUnknown_Release((IUnknown *)(x)); } while (0)
static void restore_state(GameState *g) {
    ID3D11DeviceContext *c = m_ctx;
    ID3D11DeviceContext_IASetInputLayout(c, g->il);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 3, g->vb, g->st, g->of);
    ID3D11DeviceContext_IASetIndexBuffer(c, g->ib, g->ibf, g->ibo);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, g->topo);
    ID3D11DeviceContext_VSSetShader(c, g->vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 4, g->vcb);
    ID3D11DeviceContext_VSSetShaderResources(c, 0, 2, g->vsrv);
    ID3D11DeviceContext_GSSetShader(c, g->gs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, g->hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, g->ds, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, g->ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &g->pcb);
    ID3D11DeviceContext_RSSetViewports(c, g->nvp, g->vp);
    ID3D11DeviceContext_RSSetState(c, g->rs);
    ID3D11DeviceContext_OMSetRenderTargets(c, 8, g->rtv, g->dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(c, g->dss, g->sref);
    ID3D11DeviceContext_OMSetBlendState(c, g->bs, g->bf, g->mask);
    REL(g->il); REL(g->ib); REL(g->vs); REL(g->gs); REL(g->hs); REL(g->ds); REL(g->ps); REL(g->pcb); REL(g->rs); REL(g->dsv);
    REL(g->dss); REL(g->bs);
    for (int i = 0; i < 3; i++) REL(g->vb[i]);
    for (int i = 0; i < 4; i++) REL(g->vcb[i]);
    for (int i = 0; i < 2; i++) REL(g->vsrv[i]);
    for (int i = 0; i < 8; i++) REL(g->rtv[i]);
}

UINT fgb_render_w(void), fgb_render_h(void);   // fgbridge.c
// set while the pass sets its own state: bridgeprobe's context hooks pass those calls straight on instead of taking them
// for the game's (its saved viewport and PS constant buffer went stale on the D3D11.1 path, whose state swap back does
// not go through the hooks; ultrareview 2026-11-02)
volatile int g_fgm_in_pass;

int fgm_pass(ID3D11Resource *depth, float jx, float jy) {   // 1: models were drawn into the vectors
    if (!m_on || !m_ndraws) return 0;
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    if (depth != m_depth) {   // a read-only depth view of the game's depth buffer
        if (m_dsv) ID3D11DepthStencilView_Release(m_dsv);
        m_dsv = NULL;
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D,
                                             D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL, {{0}}};
        if (FAILED(ID3D11Device_CreateDepthStencilView(m_dev, depth, &dvd, &m_dsv))) { m_log("motion pass: no depth view - off"); m_on = 0; return 0; }
        m_depth = depth;
    }
    wait_worker();   // the previous records of this frame's models
    verify_pass();
    D3D11_BOX box = {0, 0, 0, (UINT)m_queued * 64, 1, 1};
    if (m_queued) ID3D11DeviceContext_UpdateSubresource(m_ctx, (ID3D11Resource *)m_prev_buf, 0, &box, m_prev_cpu, 0, 0);
    if (!m_cam_have[1]) { ID3D11DeviceContext_CopyResource(m_ctx, (ID3D11Resource *)m_cam[1], (ID3D11Resource *)m_cam[0]); m_cam_have[1] = 1; }
    UINT rw = fgb_render_w(), rh = fgb_render_h();   // the game's render size (below the screen with the lower DLSS presets)
    static LARGE_INTEGER last;   // the models' limb allowance: seconds since the last pass (0.1 at most)
    QueryPerformanceFrequency(&qf);
    float dt = last.QuadPart ? (float)(q0.QuadPart - last.QuadPart) / qf.QuadPart : 0.1f;
    last = q0;
    float prm[8] = {768, 768, (float)rw, (float)rh, dt > 0.1f ? 0.1f : dt, m_cull_px, 0, 0};   // animation texture width (the only size taken, see fgm_draw)
    ID3D11DeviceContext_UpdateSubresource(m_ctx, (ID3D11Resource *)m_cb, 0, NULL, prm, 0, 0);
    GameState gs;
    if (m_state) ID3D11DeviceContext1_SwapDeviceContextState(m_ctx1, m_state, &m_game_state);
    else save_state(&gs);
    g_fgm_in_pass = 1;
    D3D11_VIEWPORT v = {jx, jy, (float)rw, (float)rh, 0, 1};
    ID3D11Buffer *cbs[4] = {m_cb, m_cam[0], m_cam[1], m_wind};
    ID3D11DeviceContext_RSSetViewports(m_ctx, 1, &v);
    ID3D11DeviceContext_RSSetState(m_ctx, m_rs);
    ID3D11DeviceContext_OMSetDepthStencilState(m_ctx, m_dss, 0);
    ID3D11DeviceContext_OMSetRenderTargets(m_ctx, 1, &m_mv_rtv, m_dsv);
    ID3D11DeviceContext_IASetPrimitiveTopology(m_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetConstantBuffers(m_ctx, 0, 4, cbs);
    ID3D11DeviceContext_PSSetShader(m_ctx, m_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(m_ctx, 0, 1, &m_cb);
    for (UINT i = 0; i < m_ndraws; i++) {   // the swaying trees and grass first (the game's buffers and first instance as they were)
        MDraw *d = &m_draws[i];
        int w = m_il[d->li].wind;
        if (!w) continue;
        ID3D11Buffer *wvb[2] = {d->vb0, d->vb1};
        UINT wst[2] = {d->st0, d->st1}, wof[2] = {d->of0, d->of1};
        ID3D11DeviceContext_IASetInputLayout(m_ctx, m_il[d->li].ours);
        ID3D11DeviceContext_IASetVertexBuffers(m_ctx, 0, 2, wvb, wst, wof);
        ID3D11DeviceContext_IASetIndexBuffer(m_ctx, d->ib, d->ib_fmt, d->ib_off);
        ID3D11DeviceContext_VSSetShader(m_ctx, m_wvs[w - 1], NULL, 0);
        ID3D11DeviceContext_DrawIndexedInstanced(m_ctx, d->idx, d->inst, d->start_idx, d->base_v, d->start_inst);
    }
    // the hole fill's "before" copy after the wind: only units count as moving on their own, so swaying vegetation is
    // neither marked reactive nor spread to the ground around it (FSR Quality lost its detail in the trees, AGENT_LOG 183)
    void fgb_fill_begin(void);
    fgb_fill_begin();
    for (UINT i = 0; i < m_ndraws; i++) {
        MDraw *d = &m_draws[i];
        if (m_il[d->li].wind) continue;
        int refilled = 0;   // (ultrareview 2026-11-02)
        for (int k = 0; k < M_MAPS; k++)
            if (m_maps[k].res == (ID3D11Resource *)d->vb1) { refilled = m_maps[k].gen != d->gen; break; }
        if (refilled) { m_skip_refill++; continue; }
        ID3D11Buffer *vbs[3] = {d->vb0, d->vb1, m_prev_buf};
        UINT st[3] = {d->st0, 120, 64}, of[3] = {d->of0, d->of1, d->first * 64};
        ID3D11ShaderResourceView *srv[2] = {d->anim, m_prev_anim_srv};
        ID3D11DeviceContext_IASetInputLayout(m_ctx, m_il[d->li].ours);
        ID3D11DeviceContext_IASetVertexBuffers(m_ctx, 0, 3, vbs, st, of);
        ID3D11DeviceContext_IASetIndexBuffer(m_ctx, d->ib, d->ib_fmt, d->ib_off);
        ID3D11DeviceContext_VSSetShader(m_ctx, m_vs[m_il[d->li].skin4], NULL, 0);
        ID3D11DeviceContext_VSSetShaderResources(m_ctx, 0, 2, srv);
        ID3D11DeviceContext_DrawIndexedInstanced(m_ctx, d->idx, d->inst, d->start_idx, d->base_v, 0);
    }
    g_fgm_in_pass = 0;
    if (m_state) {
        ID3D11DeviceContext1_SwapDeviceContextState(m_ctx1, m_game_state, NULL);
        ID3DDeviceContextState_Release(m_game_state);
        m_game_state = NULL;
    } else restore_state(&gs);
    release_draws();
    if (m_dump[0]) dump_mv();
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    m_pass_ms += (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
    return 1;
}

// ---- a picture of the motion vectors after the pass (bridge capture): half size, RG16F pairs, header w, h ----
void fgm_dump_next(const wchar_t *path) { wcsncpy(m_dump, path, MAX_PATH - 1); m_verify_left = M_VERIFY_DRAWS; }
static void dump_mv(void) {
    ID3D11Resource *mv = NULL;
    ID3D11RenderTargetView_GetResource(m_mv_rtv, &mv);
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)mv, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(m_dev, &td, NULL, &stg))) {
        ID3D11DeviceContext_CopyResource(m_ctx, (ID3D11Resource *)stg, mv);
        if (SUCCEEDED(ID3D11DeviceContext_Map(m_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            FILE *f = _wfopen(m_dump, L"wb");
            if (f) {
                unsigned hw = td.Width / 2, hh = td.Height / 2, hdr[2] = {hw, hh};
                fwrite(hdr, 4, 2, f);
                for (unsigned y = 0; y < hh; y++) {
                    const unsigned *row = (const unsigned *)((const unsigned char *)m.pData + (size_t)y * 2 * m.RowPitch);
                    for (unsigned x = 0; x < hw; x++) fwrite(&row[x * 2], 4, 1, f);
                }
                fclose(f);
                m_log("motion pass: motion vectors written (%ux%u)", hw, hh);
            }
            ID3D11DeviceContext_Unmap(m_ctx, (ID3D11Resource *)stg, 0);
        }
        ID3D11Texture2D_Release(stg);
    }
    ID3D11Resource_Release(mv);
    m_dump[0] = 0;
}

// ---- frame end (Present): this frame becomes the previous one ----
void fgm_frame_end(void) {
    if (!m_on) return;
    release_draws();   // waits for the worker (also in a frame without a HUD-less point)
    if (m_verify_report) verify_report();
    if (m_grid_frame != m_frame) m_nent[m_frame & 1] = 0;   // no models this frame: the next one starts without history
    for (int t = 0; t < M_TEX && m_enabled; t++)
        if (m_anim_res[t]) {
            ID3D11DeviceContext_CopySubresourceRegion(m_ctx, (ID3D11Resource *)m_prev_anim, (UINT)t, 0, 0, 0, m_anim_res[t], 0, NULL);
            m_prev_ok[t] = 1;
        }
    if (m_cam_have[0]) { ID3D11DeviceContext_CopyResource(m_ctx, (ID3D11Resource *)m_cam[1], (ID3D11Resource *)m_cam[0]); m_cam_have[1] = 1; }
    m_cam_have[0] = 0;
    m_wind_have = 0;
    m_frame++;   // the worker is idle: the next frame's records go to the other table
    retire_free();
    m_frames++;
}

void fgm_report(void (*log)(const char *fmt, ...)) {
    if (!m_on) return;
    static LARGE_INTEGER qf, t0;
    static unsigned long long c0;
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    unsigned long long c1 = __rdtsc();
    double tick_ms = t0.QuadPart && c1 > c0 ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart / (double)(c1 - c0) : 0;
    t0 = t1; c0 = c1;
    unsigned all = m_hit + m_new + m_badtex;
    if (m_frames)
        log("  motion pass: %u frames, %u model draws (%u instances) redrawn, %u draws skipped (%u full, %u an unknown record, %u texture index, %u "
            "other; highest animation texture index %d); previous model found within 0.35 m %.1f%%, none %.1f%%, no texture copy %.1f%%; per frame: pass %.3f ms, draw recording %.3f ms (main thread), "
            "worker %.3f ms (own thread); records copied %ld (%ld outside a known buffer, %ld misaligned); %u instance buffer entries handed over (%u slot tables left unfreed); %u draws refilled before the pass, not redrawn",
            m_frames, m_draws_total, m_inst_total, m_skipped, m_skipped_full, m_skip_item, m_skip_tex, m_skip_other + m_skip_range, m_tex_max,
            all ? 100.0 * m_hit / all : 0, all ? 100.0 * m_new / all : 0, all ? 100.0 * m_badtex / all : 0, m_pass_ms / m_frames, m_draw_ticks * tick_ms / m_frames,
            m_worker_ticks * tick_ms / m_frames, (long)m_copies, (long)m_copies_unmapped, (long)m_copies_misaligned, m_maps_full, m_retire_leaked, m_skip_refill);
    m_skip_refill = 0;
    if (m_frames) log("  motion pass, swaying trees and grass: %.1f draws (%.0f instances) per frame redrawn with their wind", (double)m_wind_draws / m_frames,
                      (double)m_wind_inst / m_frames);
    m_wind_draws = m_wind_inst = 0;
    m_frames = m_draws_total = m_inst_total = m_skipped = m_skipped_full = 0;
    m_hit = m_new = m_badtex = m_skip_item = m_skip_range = m_skip_tex = m_skip_other = 0;
    m_pass_ms = 0;
    m_draw_ticks = m_worker_ticks = 0;
    m_copies = m_copies_unmapped = m_copies_misaligned = 0;
}

int fgm_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *mv, UINT w, UINT h, void (*log)(const char *fmt, ...)) {
    m_dev = dev; m_ctx = ctx; m_log = log; (void)w; (void)h;
    InitializeCriticalSection(&m_map_lock);
    pD3DCompile comp = (pD3DCompile)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    D3D_SHADER_MACRO defs[2] = {{"SKIN4", "1"}, {NULL, NULL}};
    ID3DBlob *psb = NULL, *err = NULL;
    for (int k = 0; k < 2; k++)
        if (!comp || FAILED(comp(motion_hlsl, strlen(motion_hlsl), "motion", k ? defs : NULL, NULL, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                 &m_vsb[k], &err)) ||
            FAILED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(m_vsb[k]), ID3D10Blob_GetBufferSize(m_vsb[k]), NULL, &m_vs[k]))) {
            log("motion pass: vertex shader not built - off");
            return 0;
        }
    static const char *const wind_entry[3] = {"tree_a", "tree_b", "grass"};
    for (int k = 0; k < 3; k++)
        if (FAILED(comp(wind_hlsl, strlen(wind_hlsl), "wind", NULL, NULL, wind_entry[k], "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &m_wvsb[k], &err)) ||
            FAILED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(m_wvsb[k]), ID3D10Blob_GetBufferSize(m_wvsb[k]), NULL, &m_wvs[k]))) {
            log("motion pass: wind vertex shader %s not built - off", wind_entry[k]);
            return 0;
        }
    if (FAILED(comp(motion_hlsl, strlen(motion_hlsl), "motion", NULL, NULL, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err)) ||
        FAILED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &m_ps))) {
        log("motion pass: pixel shader not built - off");
        return 0;
    }
    ID3D10Blob_Release(psb);
    D3D11_BUFFER_DESC bd = {M_MAX_INST * 64, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC cbd = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_BUFFER_DESC camd = {576, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_RENDER_TARGET_VIEW_DESC rvd = {DXGI_FORMAT_R16G16_FLOAT, D3D11_RTV_DIMENSION_TEXTURE2D, {{0}}};
    D3D11_DEPTH_STENCIL_DESC dsd = {TRUE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_EQUAL, FALSE, 0, 0, {0}, {0}};
    D3D11_RASTERIZER_DESC rsd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    D3D11_TEXTURE2D_DESC atd = {768, 512, 1, M_TEX, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC asd = {DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2DARRAY, {{0}}};
    asd.Texture2DArray.MipLevels = 1; asd.Texture2DArray.ArraySize = M_TEX;
    HRESULT hr;
#define STEP(what, call) if (FAILED(hr = (call))) { log("motion pass: %s failed (0x%08lx) - off", what, (unsigned long)hr); return 0; }
    STEP("previous record buffer", ID3D11Device_CreateBuffer(dev, &bd, NULL, &m_prev_buf));
    STEP("constant buffer", ID3D11Device_CreateBuffer(dev, &cbd, NULL, &m_cb));
    STEP("camera copy", ID3D11Device_CreateBuffer(dev, &camd, NULL, &m_cam[0]));
    STEP("camera copy", ID3D11Device_CreateBuffer(dev, &camd, NULL, &m_cam[1]));
    camd.ByteWidth = 32;
    STEP("wind copy", ID3D11Device_CreateBuffer(dev, &camd, NULL, &m_wind));
    STEP("motion vector render target view", ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)mv, &rvd, &m_mv_rtv));
    STEP("depth state", ID3D11Device_CreateDepthStencilState(dev, &dsd, &m_dss));
    STEP("rasterizer state", ID3D11Device_CreateRasterizerState(dev, &rsd, &m_rs));
    STEP("previous animation textures", ID3D11Device_CreateTexture2D(dev, &atd, NULL, &m_prev_anim));
    STEP("previous animation texture view", ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)m_prev_anim, &asd, &m_prev_anim_srv));
#undef STEP
    ID3D11Device1 *d1 = NULL;
    D3D_FEATURE_LEVEL fl = ID3D11Device_GetFeatureLevel(dev);
    if (FAILED(hr = ID3D11Device_QueryInterface(dev, &iid_device1, (void **)&d1)) ||
        FAILED(hr = ID3D11Device1_CreateDeviceContextState(d1, 0, &fl, 1, D3D11_SDK_VERSION, &iid_device1, NULL, &m_state)) ||
        FAILED(hr = ID3D11DeviceContext_QueryInterface(ctx, &iid_context1, (void **)&m_ctx1))) {
        log("motion pass: no separate D3D11.1 context state (0x%08lx): saving / restoring the game's state instead", (unsigned long)hr);
        if (m_state) { ID3DDeviceContextState_Release(m_state); m_state = NULL; }
        m_ctx1 = NULL;
    }
    if (d1) ID3D11Device1_Release(d1);
    m_ent[0] = calloc(M_ENTS, sizeof(MEnt));
    m_ent[1] = calloc(M_ENTS, sizeof(MEnt));
    m_bucket = malloc(M_BUCKETS * sizeof(int));
    m_prev_cpu = calloc(M_MAX_INST, 64);
    m_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!m_ent[0] || !m_ent[1] || !m_bucket || !m_prev_cpu || !m_wake || !(m_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL))) {
        log("motion pass: no memory / worker thread - off");
        return 0;
    }
    m_on = 1;
    log("motion pass on: animated models redrawn with their own motion, trees and grass with their wind (render %ux%u)", w, h);
    return 1;
}

int fgm_active(void) { return m_on; }

// The bridge is off for the session: the pass stops, and its views and copies go so the video memory is freed (its
// render target view holds the bridge's shared motion-vector texture). Round 2 R1.
void fgm_release(void) {
    if (!m_dev) return;
    m_on = 0;
    m_enabled = 0;
    if (m_mv_rtv) { ID3D11RenderTargetView_Release(m_mv_rtv); m_mv_rtv = NULL; }
    if (m_dsv) { ID3D11DepthStencilView_Release(m_dsv); m_dsv = NULL; }
    m_depth = NULL;
    if (m_prev_anim_srv) { ID3D11ShaderResourceView_Release(m_prev_anim_srv); m_prev_anim_srv = NULL; }
    if (m_prev_anim) { ID3D11Texture2D_Release(m_prev_anim); m_prev_anim = NULL; }
}
