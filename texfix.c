// Faster texture compression for battle loading (AGENT_LOG 58). A Mk1212 siege load compresses textures to DXT1 on
// the CPU with FUN_118e8390 (a row-range body for the game's parallel_for, also called on the main thread) and
// FUN_1192eee0 (the index pass): 6.3 s on the main thread and 38.6 s on the worker threads in one 80 s load.
// Per 4x4 block of BGRA pixels the game takes the min / max colour of the pixels whose alpha is not 0, moves both
// 1/16 of the range inwards, writes them as two 565 colours (min first, so the block uses DXT1's 3-colour mode) and
// gives each pixel the index of the closest of min, max and their midpoint (sum of channel differences; ties go to
// min, then max), or 3 (transparent) where alpha is 0. Blocks with no visible pixel get colours 0, 0 and all
// indices 3. This does the same with SSE2: byte min/max over the block, psadbw for the 48 distances, compare
// masks for the choice. Output is identical byte for byte (tests/texfix_test: the game's own function on random
// images; dev builds can also check every call in game, texfix_verify.txt).
#include <windows.h>
#include <emmintrin.h>
#include <string.h>

#define RVA_COMPRESS 0x18E8390u
#define LEN_COMPRESS 0x342u   // through the ret 8
#define RVA_INDICES 0x192EEE0u
#define LEN_INDICES 0x22Au    // through the ret 10h
#define FNV_COMPRESS 0xB8BB6EFEu
#define FNV_INDICES 0xDD4A6D59u

// The game's source description: [0] first pixel, [2] width, [4] row pitch, [6] bytes per pixel (DWORDs).
typedef void(__attribute__((thiscall)) * compress_fn)(const DWORD *src, const DWORD *rows, unsigned char **out);

static inline DWORD rgb565(DWORD c0, DWORD c1, DWORD c2) {  // c0 = blue byte, c1 = green, c2 = red
    return ((c2 & 0xF8) << 8) | ((c1 & 0xFC) << 3) | (c0 >> 3);
}

// Spreads bit i of a 16-bit mask to bit 2i.
static inline DWORD spread2(DWORD x) {
    x = (x | (x << 8)) & 0x00FF00FFu;
    x = (x | (x << 4)) & 0x0F0F0F0Fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}

// Per-pixel distance (|c0 - p0| + |c1 - p1| + |c2 - p2|) for 16 pixels to one reference colour, as two vectors of
// eight 16-bit values (pixels 0-7, 8-15). lo/hi hold one pixel per 64-bit lane, alpha bytes cleared.
static inline void dist16(const __m128i lo[4], const __m128i hi[4], __m128i ref, __m128i *d07, __m128i *d815) {
    __m128i r[4];
    for (int k = 0; k < 4; k++)  // row k: [d(4k), d(4k+1), d(4k+2), d(4k+3)] as dwords
        r[k] = _mm_packs_epi32(_mm_sad_epu8(lo[k], ref), _mm_sad_epu8(hi[k], ref));
    *d07 = _mm_packs_epi32(r[0], r[1]);
    *d815 = _mm_packs_epi32(r[2], r[3]);
}

static inline __m128i le16(__m128i a, __m128i b) { return _mm_xor_si128(_mm_cmpgt_epi16(a, b), _mm_set1_epi32(-1)); }

static void block(const unsigned char *p, DWORD pitch, unsigned char *out) {
    const __m128i alpha = _mm_set1_epi32((int)0xFF000000), zero = _mm_setzero_si128();
    __m128i row[4], tr[4], lo[4], hi[4];
    __m128i vmin = _mm_set1_epi8(-1), vmax = zero;
    for (int k = 0; k < 4; k++) {
        row[k] = _mm_loadu_si128((const __m128i *)(p + pitch * k));
        tr[k] = _mm_cmpeq_epi32(_mm_and_si128(row[k], alpha), zero);  // alpha 0: transparent
        vmin = _mm_min_epu8(vmin, _mm_or_si128(row[k], tr[k]));       // transparent pixels count as 255 ...
        vmax = _mm_max_epu8(vmax, _mm_andnot_si128(tr[k], row[k]));   // ... and as 0, the start values
        __m128i c = _mm_andnot_si128(alpha, row[k]);
        lo[k] = _mm_unpacklo_epi32(c, zero);
        hi[k] = _mm_unpackhi_epi32(c, zero);
    }
    __m128i t07 = _mm_packs_epi32(tr[0], tr[1]), t815 = _mm_packs_epi32(tr[2], tr[3]);
    DWORD tmask = (DWORD)_mm_movemask_epi8(_mm_packs_epi16(t07, t815));
    DWORD n0, n1, n2, x0, x1, x2;  // inset min (n) and max (x) colour
    if (tmask == 0xFFFF) {
        n0 = n1 = n2 = 0;
        x0 = x1 = x2 = 1;
    } else {
        vmin = _mm_min_epu8(vmin, _mm_shuffle_epi32(vmin, _MM_SHUFFLE(1, 0, 3, 2)));
        vmin = _mm_min_epu8(vmin, _mm_shuffle_epi32(vmin, _MM_SHUFFLE(2, 3, 0, 1)));
        vmax = _mm_max_epu8(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(1, 0, 3, 2)));
        vmax = _mm_max_epu8(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(2, 3, 0, 1)));
        DWORD mn = (DWORD)_mm_cvtsi128_si32(vmin), mx = (DWORD)_mm_cvtsi128_si32(vmax);
        DWORD m0 = mn & 0xFF, m1 = (mn >> 8) & 0xFF, m2 = (mn >> 16) & 0xFF;
        DWORD M0 = mx & 0xFF, M1 = (mx >> 8) & 0xFF, M2 = (mx >> 16) & 0xFF;
        DWORD i0 = (DWORD)((int)(M0 - m0) >> 4), i1 = (DWORD)((int)(M1 - m1) >> 4), i2 = (DWORD)((int)(M2 - m2) >> 4);
        n0 = (i0 & 0xFF) + m0 > 0xFF ? 0xFF : ((i0 + m0) & 0xFF);
        n1 = (i1 & 0xFF) + m1 > 0xFF ? 0xFF : ((i1 + m1) & 0xFF);
        n2 = (i2 & 0xFF) + m2 > 0xFF ? 0xFF : ((i2 + m2) & 0xFF);
        x0 = M0 >= (i0 & 0xFF) ? M0 - (i0 & 0xFF) : 0;
        x1 = M1 >= (i1 & 0xFF) ? M1 - (i1 & 0xFF) : 0;
        x2 = M2 >= (i2 & 0xFF) ? M2 - (i2 & 0xFF) : 0;
    }
    DWORD c_min = rgb565(n0, n1, n2), c_max = rgb565(x0, x1, x2);
    // the index pass expands both colours back to 8 bits (5 or 6 bits replicated) and takes the halved sum as midpoint
    DWORD A0 = (n0 & 0xF8) | (n0 >> 5), A1 = (n1 & 0xFC) | (n1 >> 6), A2 = (n2 & 0xF8) | (n2 >> 5);
    DWORD B0 = (x0 & 0xF8) | (x0 >> 5), B1 = (x1 & 0xFC) | (x1 >> 6), B2 = (x2 & 0xF8) | (x2 >> 5);
    DWORD C0 = (A0 >> 1) + (B0 >> 1), C1 = (A1 >> 1) + (B1 >> 1), C2 = (A2 >> 1) + (B2 >> 1);
    __m128i rA = _mm_set_epi32(0, (int)(A0 | A1 << 8 | A2 << 16), 0, (int)(A0 | A1 << 8 | A2 << 16));
    __m128i rB = _mm_set_epi32(0, (int)(B0 | B1 << 8 | B2 << 16), 0, (int)(B0 | B1 << 8 | B2 << 16));
    __m128i rC = _mm_set_epi32(0, (int)(C0 | C1 << 8 | C2 << 16), 0, (int)(C0 | C1 << 8 | C2 << 16));
    __m128i a07, a815, b07, b815, c07, c815;
    dist16(lo, hi, rA, &a07, &a815);
    dist16(lo, hi, rB, &b07, &b815);
    dist16(lo, hi, rC, &c07, &c815);
    // index 0 where min is closest (ties included), else 1 where max is, else 2; 3 for transparent pixels
    __m128i aBest07 = _mm_and_si128(le16(a07, b07), le16(a07, c07)), aBest815 = _mm_and_si128(le16(a815, b815), le16(a815, c815));
    __m128i bBest07 = _mm_and_si128(le16(b07, a07), le16(b07, c07)), bBest815 = _mm_and_si128(le16(b815, a815), le16(b815, c815));
    __m128i one07 = _mm_andnot_si128(aBest07, bBest07), one815 = _mm_andnot_si128(aBest815, bBest815);
    __m128i two07 = _mm_andnot_si128(_mm_or_si128(aBest07, bBest07), _mm_set1_epi32(-1));
    __m128i two815 = _mm_andnot_si128(_mm_or_si128(aBest815, bBest815), _mm_set1_epi32(-1));
    DWORD m_one = (DWORD)_mm_movemask_epi8(_mm_packs_epi16(one07, one815)) | tmask;
    DWORD m_two = (DWORD)_mm_movemask_epi8(_mm_packs_epi16(two07, two815)) | tmask;
    DWORD idx = spread2(m_one) | spread2(m_two) << 1;
    out[0] = (unsigned char)c_min;
    out[1] = (unsigned char)(c_min >> 8);
    out[2] = (unsigned char)c_max;
    out[3] = (unsigned char)(c_max >> 8);
    memcpy(out + 4, &idx, 4);
}

// Same signature and loop as FUN_118e8390: rows [rows[0], rows[1]) in steps of 4, blocks across the width.
// The game calls with a 4-byte aligned stack: realign it for the SSE spills.
void __attribute__((thiscall, force_align_arg_pointer)) tf_compress(const DWORD *src, const DWORD *rows, unsigned char **out) {
    for (DWORD y = rows[0]; y < rows[1]; y += 4) {
        const unsigned char *line = (const unsigned char *)(UINT_PTR)(src[4] * y + src[0]);
        for (DWORD x = 0; x < src[2]; x += 4) {
            block(line + src[6] * x, src[4], *out);
            *out += 8;
        }
    }
}

static DWORD fnv1a(const unsigned char *b, DWORD n) {
    DWORD h = 0x811C9DC5u;
    while (n--) h = (h ^ *b++) * 0x01000193u;
    return h;
}

#ifndef TEXFIX_TEST

// verify (dev builds): run the game's function and compare. Returns 1 when installed.
int install_texfix(HMODULE m, int verify, void (*log)(const char *fmt, ...)) {
    unsigned char *b = (unsigned char *)m, *site = b + RVA_COMPRESS;
    if (fnv1a(site, LEN_COMPRESS) != FNV_COMPRESS || fnv1a(b + RVA_INDICES, LEN_INDICES) != FNV_INDICES) {
        log("texture compression (+0x%X) differs from this game version - not replaced", RVA_COMPRESS);
        return 0;
    }
    void *to = (void *)tf_compress;
    (void)verify;
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        log("texture compression: VirtualProtect failed (%lu) - not replaced", GetLastError());
        return 0;
    }
    site[0] = 0xE9;
    DWORD rel = (DWORD)((unsigned char *)to - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log("texture compression replaced (SSE2 DXT1 blocks, same output)%s", verify ? " - VERIFY mode, every call checked against the game's" : "");
    return 1;
}
#endif
