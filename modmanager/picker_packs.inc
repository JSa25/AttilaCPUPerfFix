// ---- Which ticked mods change the same files (start window redo, 2026-11-25). Only each pack's file index is read (the
// names of the files in it, never their data): one 64-bit hash per file path, upper and lower case and / \ the same.
// When two ticked packs hold the same file, the upper one wins, as in the game. Attila's packs are PFH4:
//   header 28 bytes: "PFH4", u32 type, u32 pack refs count, u32 pack refs size, u32 file count, u32 file index size,
//   u32 time; then the pack refs; then per file: u32 size, u32 time (only when type has 0x40), the name and a 0 byte.
// Type 0x80 (encrypted index) and other pack versions give no file list: such a pack is left out of the hints.
// Needs <windows.h>, <stdio.h>, <stdlib.h>, <string.h> before it.

static int cmp_u64(const void *a, const void *b) {
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}

// The sorted file hashes of a pack (the caller frees *out). Returns the number of files, -1 when there is no readable list.
static int pack_hashes(const char *path, unsigned long long **out) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char h[28];
    int n = -1;
    if (fread(h, 1, 28, f) == 28 && !memcmp(h, "PFH4", 4)) {
        unsigned type, refsize, count, isize;
        memcpy(&type, h + 4, 4); memcpy(&refsize, h + 12, 4); memcpy(&count, h + 16, 4); memcpy(&isize, h + 20, 4);
        unsigned stamp = (type & 0x40) ? 4 : 0;
        unsigned char *ix = NULL;
        unsigned long long *hs = NULL;
        if (!(type & 0x80) && isize <= (256u << 20) && count <= isize / (5 + stamp) && !_fseeki64(f, 28 + (long long)refsize, SEEK_SET) &&
            (ix = malloc((size_t)isize + 1)) && (hs = malloc(sizeof *hs * (count ? count : 1))) && fread(ix, 1, isize, f) == isize) {
            ix[isize] = 0;   // a last name without its 0 byte still ends
            size_t at = 0;
            unsigned i;
            for (i = 0; i < count && at + 4 + stamp < isize; i++) {
                at += 4 + stamp;
                unsigned long long v = 1469598103934665603ULL;
                for (; at < isize && ix[at]; at++) {
                    unsigned char c = ix[at];
                    if (c == '\\') c = '/';
                    else if (c >= 'A' && c <= 'Z') c += 32;
                    v ^= c;
                    v *= 1099511628211ULL;
                }
                at++;
                hs[i] = v;
            }
            if (i == count) {
                qsort(hs, count, sizeof *hs, cmp_u64);
                unsigned u = 0;   // names that differ only in case are one file (seen: 149 in 1212compbuild_v2.pack)
                for (unsigned j = 0; j < count; j++) if (!u || hs[j] != hs[u - 1]) hs[u++] = hs[j];
                *out = hs;
                hs = NULL;
                n = (int)u;
            }
        }
        free(ix);
        free(hs);
    }
    fclose(f);
    return n;
}

// For each ticked pack: how many of its files an upper pack replaces (under_n), and which upper pack replaces the most of
// them (under_by, an index into the list, -1 = none); how many files of lower packs it replaces (over_n), and the lower
// pack it replaces most of (over_of). Packs without a list (n[i] < 0) take no part.
typedef struct { int under_n, under_by, over_n, over_of; } Clash;

static void clashes(unsigned long long *const *h, const int *n, int k, Clash *out) {
    for (int i = 0; i < k; i++) out[i] = (Clash){0, -1, 0, -1};
    if (k < 2) return;
    enum { PAIRS = 512 };   // shortcut: who-replaces-whom is kept for the first 512 ticked packs; counts cover all of them
    int kp = k < PAIRS ? k : PAIRS;
    int *pair = calloc((size_t)kp * kp, sizeof *pair);   // pair[i * kp + j] = files of j that i replaces
    int *pos = calloc((size_t)k, sizeof *pos), *heap = malloc(sizeof *heap * (size_t)k), hn = 0;
    if (!pair || !pos || !heap) { free(pair); free(pos); free(heap); return; }
    // k-way merge of the sorted lists, smallest hash first (ties: the upper pack first)
#define KEY(p) h[p][pos[p]]
#define LESS(a, b) (KEY(a) < KEY(b) || (KEY(a) == KEY(b) && (a) < (b)))
    for (int p = 0; p < k; p++) {
        if (n[p] <= 0) continue;
        int c = hn++;
        heap[c] = p;
        while (c && LESS(heap[c], heap[(c - 1) / 2])) { int t = heap[c]; heap[c] = heap[(c - 1) / 2]; heap[(c - 1) / 2] = t; c = (c - 1) / 2; }
    }
    unsigned long long cur = 0;
    int winner = -1;
    while (hn) {
        int p = heap[0];
        unsigned long long v = KEY(p);
        if (winner < 0 || v != cur) { cur = v; winner = p; }   // the first pack with this file is the upper one
        else if (p != winner) {
            out[p].under_n++;
            out[winner].over_n++;
            if (p < kp && winner < kp) pair[winner * kp + p]++;
        }
        if (++pos[p] >= n[p]) heap[0] = heap[--hn];
        int c = 0;
        for (;;) {   // sift down
            int l = 2 * c + 1, r = l + 1, m = c;
            if (l < hn && LESS(heap[l], heap[m])) m = l;
            if (r < hn && LESS(heap[r], heap[m])) m = r;
            if (m == c) break;
            int t = heap[c]; heap[c] = heap[m]; heap[m] = t;
            c = m;
        }
    }
#undef LESS
#undef KEY
    for (int i = 0; i < kp; i++)
        for (int j = 0; j < kp; j++) {
            int c = pair[i * kp + j];
            if (c && (out[j].under_by < 0 || c > pair[out[j].under_by * kp + j])) out[j].under_by = i;
            if (c && (out[i].over_of < 0 || c > pair[i * kp + out[i].over_of])) out[i].over_of = j;
        }
    free(pair); free(pos); free(heap);
}
