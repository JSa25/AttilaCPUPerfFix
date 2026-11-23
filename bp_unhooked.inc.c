// Part of bridgeprobe.c (included there, not compiled on its own): unhooked(): DXGI's own function behind another mod's jump (fgbridge_test.c builds this file too).
// gave_up: fn could not be gone past; found once, not again with a file read and a log line every frame.
static void *unhooked(void *fn, void **cache, void **gave_up, const char *name) {
    if (*cache && *cache != fn) return *cache;
    if (*gave_up == fn) return fn;
    void *u = fgb_unhooked(fn);
    if (u && u != fn) g_log("frame bridge: DXGI's %s starts with a jump (another mod hooked it there) - the mod's own calls go past it", name);
    if (!u) {
        g_log("frame bridge: DXGI's %s starts with a jump the mod cannot go past - it calls the hooked function", name);
        *gave_up = fn;
    }
    return *cache = u ? u : fn;
}
