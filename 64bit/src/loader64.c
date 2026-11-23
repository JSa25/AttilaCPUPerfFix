// loader64: what the plugin can tell about the loader it runs under, and the wording of what happened at the game
// addresses the loader may patch too.
// The loader's interface says little about its version (interface 1's version text reads "1.2" in loader 1.3.6), so
// the number comes from attila64.exe's own version resource. It is only reported: every fix decides by the game's
// bytes at its own addresses, never by the loader's version.
#include <stdio.h>
#include <string.h>
#include "perffix64.h"

// exe: the loader's module (or any module mapped for its resources). 1 = v[0..3] filled from its version resource.
int loader64_version(HMODULE exe, unsigned v[4]) {
    HRSRC r = FindResourceW(exe, MAKEINTRESOURCEW(1), (LPCWSTR)RT_VERSION);
    HGLOBAL g = r ? LoadResource(exe, r) : NULL;
    const unsigned char *p = g ? LockResource(g) : NULL;
    DWORD n = p ? SizeofResource(exe, r) : 0;
    for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= n; i += 4) {   // the fixed block starts on a 4-byte boundary
        VS_FIXEDFILEINFO f;
        memcpy(&f, p + i, sizeof f);
        if (f.dwSignature != 0xFEEF04BD) continue;
        v[0] = HIWORD(f.dwFileVersionMS); v[1] = LOWORD(f.dwFileVersionMS); v[2] = HIWORD(f.dwFileVersionLS); v[3] = LOWORD(f.dwFileVersionLS);
        return 1;
    }
    return 0;
}

// -1, 0, 1: v against the newest loader the fix was checked with (LOADER64_CHECKED in perffix64.h)
int loader64_compare(const unsigned v[4]) {
    static const unsigned checked[4] = {LOADER64_CHECKED};
    for (int i = 0; i < 4; i++) if (v[i] != checked[i]) return v[i] < checked[i] ? -1 : 1;
    return 0;
}

// Appends to `out` what became of the sites that are not ours, e.g.
//   "; left to the loader's own fast sort: soldiers, rigid parts; not replaced, code differs: decals"
// Nothing is appended when every site is ours. loader_what names the loader's patch of the same thing.
void loader64_site_notes(char *out, size_t cap, const int *state, const char *const *label, int count, const char *loader_what) {
    static const struct { int state; const char *lead; } kinds[4] = {
        {SITE_LOADER, NULL}, {SITE_DIFFERS, "not changed, code differs"}, {SITE_BUSY, "not changed, a thread was inside"},
        {SITE_FAILED, "not changed, the write failed"}};
    size_t n = strlen(out);
    for (int k = 0; k < 4; k++) {
        int first = 1;
        for (int i = 0; i < count; i++) {
            if (state[i] != kinds[k].state || n + 1 >= cap) continue;
            if (first && kinds[k].lead) n += (size_t)snprintf(out + n, cap - n, "; %s: %s", kinds[k].lead, label[i]);
            else if (first) n += (size_t)snprintf(out + n, cap - n, "; left to the loader's own %s: %s", loader_what, label[i]);
            else n += (size_t)snprintf(out + n, cap - n, ", %s", label[i]);
            if (n >= cap) n = cap - 1;
            first = 0;
        }
    }
}
