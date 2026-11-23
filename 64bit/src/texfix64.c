// texfix64: faster DXT1 compression of imposter textures (battle and siege loads), ported from heapfix/texfix.c.
// Engine: UTILITYLIB::IMAGE_COMPRESSION::TBB_DXT_COMPRESSOR<RGBA8_TO_DXT1_COMPRESSOR>::convert_dxt1 at 0x339a790
// (research/texfix.md): rdi = compressor (+0x00 pixels, +0x10 width u32, +0x18 pitch i32, +0x20 bytes/pixel u32),
// rsi = row range {begin, end}, rdx = output pointer, advanced 8 bytes per 4x4 block. Same scalar algorithm as the
// x86 game, so texfix's SSE2 block() gives the same bytes (tests/texfix64_test.c runs the engine's own code against it).
#include <emmintrin.h>
#include <string.h>
#include "perffix64.h"

#define TF_SITE 0x339a790u
#define TF_CHAIN_LEN 0xC6Au        // convert_dxt1 .. write_colour_indices end
#define TF_CHAIN_FNV 0x40236C7Fu
static const unsigned char TF_PROLOGUE[17] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};

static inline uint32_t rgb565(uint32_t c0, uint32_t c1, uint32_t c2) { return ((c2 & 0xF8) << 8) | ((c1 & 0xFC) << 3) | (c0 >> 3); }
static inline uint32_t spread2(uint32_t x) {   // bit i of a 16-bit mask to bit 2i
    x = (x | (x << 8)) & 0x00FF00FFu;
    x = (x | (x << 4)) & 0x0F0F0F0Fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}
static inline void dist16(const __m128i lo[4], const __m128i hi[4], __m128i ref, __m128i *d07, __m128i *d815) {
    __m128i r[4];
    for (int k = 0; k < 4; k++) r[k] = _mm_packs_epi32(_mm_sad_epu8(lo[k], ref), _mm_sad_epu8(hi[k], ref));
    *d07 = _mm_packs_epi32(r[0], r[1]);
    *d815 = _mm_packs_epi32(r[2], r[3]);
}
static inline __m128i le16(__m128i a, __m128i b) { return _mm_xor_si128(_mm_cmpgt_epi16(a, b), _mm_set1_epi32(-1)); }

// heapfix/texfix.c block(), unchanged except the row stride is signed 64-bit (the engine sign-extends its pitch)
static void block(const unsigned char *p, ptrdiff_t pitch, unsigned char *out) {
    const __m128i alpha = _mm_set1_epi32((int)0xFF000000), zero = _mm_setzero_si128();
    __m128i row[4], tr[4], lo[4], hi[4];
    __m128i vmin = _mm_set1_epi8(-1), vmax = zero;
    for (int k = 0; k < 4; k++) {
        row[k] = _mm_loadu_si128((const __m128i *)(p + pitch * k));
        tr[k] = _mm_cmpeq_epi32(_mm_and_si128(row[k], alpha), zero);
        vmin = _mm_min_epu8(vmin, _mm_or_si128(row[k], tr[k]));
        vmax = _mm_max_epu8(vmax, _mm_andnot_si128(tr[k], row[k]));
        __m128i c = _mm_andnot_si128(alpha, row[k]);
        lo[k] = _mm_unpacklo_epi32(c, zero);
        hi[k] = _mm_unpackhi_epi32(c, zero);
    }
    __m128i t07 = _mm_packs_epi32(tr[0], tr[1]), t815 = _mm_packs_epi32(tr[2], tr[3]);
    uint32_t tmask = (uint32_t)_mm_movemask_epi8(_mm_packs_epi16(t07, t815));
    uint32_t n0, n1, n2, x0, x1, x2;
    if (tmask == 0xFFFF) {
        n0 = n1 = n2 = 0;
        x0 = x1 = x2 = 1;
    } else {
        vmin = _mm_min_epu8(vmin, _mm_shuffle_epi32(vmin, _MM_SHUFFLE(1, 0, 3, 2)));
        vmin = _mm_min_epu8(vmin, _mm_shuffle_epi32(vmin, _MM_SHUFFLE(2, 3, 0, 1)));
        vmax = _mm_max_epu8(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(1, 0, 3, 2)));
        vmax = _mm_max_epu8(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(2, 3, 0, 1)));
        uint32_t mn = (uint32_t)_mm_cvtsi128_si32(vmin), mx = (uint32_t)_mm_cvtsi128_si32(vmax);
        uint32_t m0 = mn & 0xFF, m1 = (mn >> 8) & 0xFF, m2 = (mn >> 16) & 0xFF;
        uint32_t M0 = mx & 0xFF, M1 = (mx >> 8) & 0xFF, M2 = (mx >> 16) & 0xFF;
        uint32_t i0 = (uint32_t)((int)(M0 - m0) >> 4), i1 = (uint32_t)((int)(M1 - m1) >> 4), i2 = (uint32_t)((int)(M2 - m2) >> 4);
        n0 = (i0 & 0xFF) + m0 > 0xFF ? 0xFF : ((i0 + m0) & 0xFF);
        n1 = (i1 & 0xFF) + m1 > 0xFF ? 0xFF : ((i1 + m1) & 0xFF);
        n2 = (i2 & 0xFF) + m2 > 0xFF ? 0xFF : ((i2 + m2) & 0xFF);
        x0 = M0 >= (i0 & 0xFF) ? M0 - (i0 & 0xFF) : 0;
        x1 = M1 >= (i1 & 0xFF) ? M1 - (i1 & 0xFF) : 0;
        x2 = M2 >= (i2 & 0xFF) ? M2 - (i2 & 0xFF) : 0;
    }
    uint32_t c_min = rgb565(n0, n1, n2), c_max = rgb565(x0, x1, x2);
    uint32_t A0 = (n0 & 0xF8) | (n0 >> 5), A1 = (n1 & 0xFC) | (n1 >> 6), A2 = (n2 & 0xF8) | (n2 >> 5);
    uint32_t B0 = (x0 & 0xF8) | (x0 >> 5), B1 = (x1 & 0xFC) | (x1 >> 6), B2 = (x2 & 0xF8) | (x2 >> 5);
    uint32_t C0 = (A0 >> 1) + (B0 >> 1), C1 = (A1 >> 1) + (B1 >> 1), C2 = (A2 >> 1) + (B2 >> 1);
    __m128i rA = _mm_set_epi32(0, (int)(A0 | A1 << 8 | A2 << 16), 0, (int)(A0 | A1 << 8 | A2 << 16));
    __m128i rB = _mm_set_epi32(0, (int)(B0 | B1 << 8 | B2 << 16), 0, (int)(B0 | B1 << 8 | B2 << 16));
    __m128i rC = _mm_set_epi32(0, (int)(C0 | C1 << 8 | C2 << 16), 0, (int)(C0 | C1 << 8 | C2 << 16));
    __m128i a07, a815, b07, b815, c07, c815;
    dist16(lo, hi, rA, &a07, &a815);
    dist16(lo, hi, rB, &b07, &b815);
    dist16(lo, hi, rC, &c07, &c815);
    __m128i aBest07 = _mm_and_si128(le16(a07, b07), le16(a07, c07)), aBest815 = _mm_and_si128(le16(a815, b815), le16(a815, c815));
    __m128i bBest07 = _mm_and_si128(le16(b07, a07), le16(b07, c07)), bBest815 = _mm_and_si128(le16(b815, a815), le16(b815, c815));
    __m128i one07 = _mm_andnot_si128(aBest07, bBest07), one815 = _mm_andnot_si128(aBest815, bBest815);
    __m128i two07 = _mm_andnot_si128(_mm_or_si128(aBest07, bBest07), _mm_set1_epi32(-1));
    __m128i two815 = _mm_andnot_si128(_mm_or_si128(aBest815, bBest815), _mm_set1_epi32(-1));
    uint32_t m_one = (uint32_t)_mm_movemask_epi8(_mm_packs_epi16(one07, one815)) | tmask;
    uint32_t m_two = (uint32_t)_mm_movemask_epi8(_mm_packs_epi16(two07, two815)) | tmask;
    uint32_t idx = spread2(m_one) | spread2(m_two) << 1;
    out[0] = (unsigned char)c_min;
    out[1] = (unsigned char)(c_min >> 8);
    out[2] = (unsigned char)c_max;
    out[3] = (unsigned char)(c_max >> 8);
    memcpy(out + 4, &idx, 4);
}

// convert_dxt1's loop with the engine's address arithmetic: 32-bit row/column products, row offset sign-extended.
volatile LONG64 g_tf_calls, g_tf_blocks;   // logged by perffix64.c's stats thread
__attribute__((sysv_abi)) void tf_compress64(const unsigned char *self, const uint32_t *range, unsigned char **out) {
    const unsigned char *pix;
    unsigned char *const start = *out;
    uint32_t width, bpp; int32_t pitch;
    memcpy(&pix, self, 8); memcpy(&width, self + 0x10, 4); memcpy(&pitch, self + 0x18, 4); memcpy(&bpp, self + 0x20, 4);
    for (uint32_t y = range[0]; y < range[1]; y += 4) {
        const unsigned char *line = pix + (int64_t)(int32_t)((uint32_t)pitch * y);
        for (uint32_t x = 0; x < width; x += 4) {
            block(line + (uint32_t)(x * bpp), (ptrdiff_t)pitch, *out);
            *out += 8;
        }
    }
    InterlockedIncrement64(&g_tf_calls);
    InterlockedAdd64(&g_tf_blocks, (*out - start) / 8);
}

#ifndef TEXFIX64_TEST
int install_texfix64(void) {
    unsigned char *site = (unsigned char *)(uintptr_t)TF_SITE;
    if (memcmp(site, TF_PROLOGUE, sizeof TF_PROLOGUE) || fnv1a(site, TF_CHAIN_LEN) != TF_CHAIN_FNV) return 0;
    if (thread_inside(TF_SITE, sizeof TF_PROLOGUE)) return -1;
    unsigned char j[17] = {0xFF, 0x25, 0, 0, 0, 0};   // jmp [rip+0] ; abs64 ; int3 padding
    uint64_t to = (uint64_t)(uintptr_t)&tf_compress64;
    memcpy(j + 6, &to, 8);
    j[14] = j[15] = j[16] = 0xCC;
    return patch_code(site, j, sizeof j) ? 1 : -2;
}
#endif
