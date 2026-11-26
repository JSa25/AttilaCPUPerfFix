// ---- Background threads of the start window (redo, 2026-11-25). The window opens and stays quick; these report back with
// window messages and never touch g_mods themselves.
//   Steam thread: connects to Steam quietly (no "start Steam" box: shim_steam.c steam_connect_quiet), then watches the
//                 Workshop items the window shows (state and download progress) and subscribes when asked.
//   File thread:  small mod pictures, the file lists of the ticked packs (conflict hints), the engine fix's version.
enum { WM_P_STEAM = WM_APP + 20, WM_P_THUMB, WM_P_CLASH, WM_P_INFO };
static HWND g_win;
static volatile LONG g_quit_work;

// ---- Steam
int steam_connect_quiet(void);   // shim_steam.c: 1 = connected (the game's own connect later reuses it)
void *steam_api_fn(const char *name);   // shim_steam.c: a function of steam_api64.dll
enum { WS_SUBSCRIBED = 1, WS_INSTALLED = 4, WS_NEEDS_UPDATE = 8, WS_DOWNLOADING = 16, WS_PENDING = 32 };   // Steam's EItemState
typedef struct { unsigned long long id, done, total; unsigned state; int asked; } WsItem;
#define MAX_WATCH 2100
static WsItem g_ws_items[MAX_WATCH];
static int g_ws_n;
static SRWLOCK g_ws_lock = SRWLOCK_INIT;
static volatile LONG g_steam;          // 0 = trying, 1 = connected, -1 = no Steam
static HANDLE g_steam_wake, g_steam_thread;
static unsigned long long g_sub_queue[64];
static int g_sub_n;
static const char *g_fake_steam;       // ATTILA_PICKER_FAKE_STEAM (tests): "id:state:done:total,..." instead of Steam

static WsItem *ws_find(unsigned long long id) {   // under the lock
    for (int i = 0; i < g_ws_n; i++) if (g_ws_items[i].id == id) return &g_ws_items[i];
    return NULL;
}
static void ws_watch(unsigned long long id) {   // main thread: show this item's state from now on
    if (!id) return;
    AcquireSRWLockExclusive(&g_ws_lock);
    if (!ws_find(id) && g_ws_n < MAX_WATCH) { memset(&g_ws_items[g_ws_n], 0, sizeof g_ws_items[0]); g_ws_items[g_ws_n++].id = id; }
    ReleaseSRWLockExclusive(&g_ws_lock);
}
// main thread: 1 = known; state and bytes so far / in all
static int ws_get(unsigned long long id, unsigned *state, unsigned long long *done, unsigned long long *total) {
    int ok = 0;
    AcquireSRWLockShared(&g_ws_lock);
    WsItem *w = ws_find(id);
    if (w && g_steam > 0) { *state = w->state; *done = w->done; *total = w->total; ok = 1; }
    ReleaseSRWLockShared(&g_ws_lock);
    return ok;
}
static void ws_subscribe(unsigned long long id) {   // main thread
    ws_watch(id);
    AcquireSRWLockExclusive(&g_ws_lock);
    WsItem *w = ws_find(id);
    if (w) w->asked = 1;
    if (g_sub_n < 64) g_sub_queue[g_sub_n++] = id;
    ReleaseSRWLockExclusive(&g_ws_lock);
    SetEvent(g_steam_wake);
}

static void fake_state(unsigned long long id, WsItem *out) {   // tests: the state given in ATTILA_PICKER_FAKE_STEAM
    for (const char *p = g_fake_steam; p && *p;) {
        unsigned long long i = 0, d = 0, t = 0;
        unsigned s = 0;
        if (sscanf(p, "%llu:%u:%llu:%llu", &i, &s, &d, &t) >= 2 && i == id) { out->state = s; out->done = d; out->total = t; return; }
        p = strchr(p, ',');
        if (p) p++;
    }
    out->state = out->asked ? WS_SUBSCRIBED | WS_DOWNLOADING : 0;
    out->total = 100; out->done = out->asked ? 37 : 0;
}

static DWORD WINAPI steam_thread(void *unused) {
    void *ugc = NULL;
    void *(*get_ugc)(void) = NULL;
    unsigned long long (*subscribe)(void *, unsigned long long) = NULL;
    unsigned (*get_state)(void *, unsigned long long) = NULL;
    BOOL (*get_dl)(void *, unsigned long long, unsigned long long *, unsigned long long *) = NULL;
    // Steam's answer to a subscribe (read without SteamAPI_RunCallbacks, which belongs to the game), and whether the
    // Steam client is online: a client logged off by Steam's servers (seen 2026-11-25: "Session Replaced") takes the
    // call and silently drops it
    void *user = NULL, *utils = NULL;
    unsigned char (*logged_on)(void *) = NULL;
    unsigned char (*call_done)(void *, unsigned long long, unsigned char *) = NULL;
    unsigned char (*call_result)(void *, unsigned long long, void *, int, int, unsigned char *) = NULL;
    struct { unsigned long long id, call; } pending[64];
    int npending = 0;
    int fake_offline = g_fake_steam && strstr(g_fake_steam, "offline") != NULL;
    while (!g_quit_work) {
        if (g_fake_steam) { g_steam = 1; break; }
        if (steam_connect_quiet()) {
            *(void **)&get_ugc = steam_api_fn("SteamAPI_SteamUGC_v017");
            *(void **)&subscribe = steam_api_fn("SteamAPI_ISteamUGC_SubscribeItem");
            *(void **)&get_state = steam_api_fn("SteamAPI_ISteamUGC_GetItemState");
            *(void **)&get_dl = steam_api_fn("SteamAPI_ISteamUGC_GetItemDownloadInfo");
            ugc = get_ugc ? get_ugc() : NULL;
            void *(*get_user)(void) = (void *(*)(void))steam_api_fn("SteamAPI_SteamUser_v023");
            void *(*get_utils)(void) = (void *(*)(void))steam_api_fn("SteamAPI_SteamUtils_v010");
            *(void **)&logged_on = steam_api_fn("SteamAPI_ISteamUser_BLoggedOn");
            *(void **)&call_done = steam_api_fn("SteamAPI_ISteamUtils_IsAPICallCompleted");
            *(void **)&call_result = steam_api_fn("SteamAPI_ISteamUtils_GetAPICallResult");
            user = get_user ? get_user() : NULL;
            utils = get_utils ? get_utils() : NULL;
            g_steam = ugc && subscribe && get_state && get_dl ? 1 : -1;
            break;
        }
        g_steam = -1;
        PostMessageA(g_win, WM_P_STEAM, 0, 0);
        WaitForSingleObject(g_steam_wake, INFINITE);   // a Subscribe press tries again (Steam may have been started since)
        g_steam = 0;
        AcquireSRWLockExclusive(&g_ws_lock);
        int asked = g_sub_n;
        ReleaseSRWLockExclusive(&g_ws_lock);
        if (!asked) continue;
        if (!g_quit_work && !steam_connect_quiet()) {   // still no Steam: the window opens the Workshop pages instead
            g_steam = -1;
            PostMessageA(g_win, WM_P_STEAM, 1, 0);
            AcquireSRWLockExclusive(&g_ws_lock);
            g_sub_n = 0;
            ReleaseSRWLockExclusive(&g_ws_lock);
        }
    }
    PostMessageA(g_win, WM_P_STEAM, 0, 0);
    while (!g_quit_work && g_steam > 0) {
        unsigned long long sub[64];
        int ns = 0;
        AcquireSRWLockExclusive(&g_ws_lock);
        memcpy(sub, g_sub_queue, sizeof(sub[0]) * (size_t)g_sub_n);
        ns = g_sub_n;
        g_sub_n = 0;
        int n = g_ws_n;
        ReleaseSRWLockExclusive(&g_ws_lock);
        for (int i = 0; i < ns; i++) {
            int online = g_fake_steam ? !fake_offline : !(user && logged_on) || logged_on(user);
            if (!online) {   // Steam would drop it: the window says why
                AcquireSRWLockExclusive(&g_ws_lock);
                WsItem *w = ws_find(sub[i]);
                if (w) w->asked = 0;
                ReleaseSRWLockExclusive(&g_ws_lock);
                PostMessageA(g_win, WM_P_STEAM, 2, 3 /* k_EResultNoConnection */);
                continue;
            }
            if (g_fake_steam) continue;
            unsigned long long call = subscribe(ugc, sub[i]);
            if (call && npending < 64) { pending[npending].id = sub[i]; pending[npending++].call = call; }
        }
        for (int i = 0; i < npending && utils && call_done && call_result; i++) {   // Steam's answers
            unsigned char failed = 0;
            if (!call_done(utils, pending[i].call, &failed)) continue;
            struct { int result; unsigned long long id; } r = {0, 0};   // RemoteStorageSubscribePublishedFileResult_t (1313)
            int ok = !failed && call_result(utils, pending[i].call, &r, (int)sizeof r, 1313, &failed) && !failed;
            if (!ok || r.result != 1 /* k_EResultOK */) {
                AcquireSRWLockExclusive(&g_ws_lock);
                WsItem *w = ws_find(pending[i].id);
                if (w) w->asked = 0;
                ReleaseSRWLockExclusive(&g_ws_lock);
                PostMessageA(g_win, WM_P_STEAM, 2, ok ? r.result : -1);
            }
            pending[i--] = pending[--npending];
        }
        int changed = 0;
        for (int i = 0; i < n && !g_quit_work; i++) {
            WsItem now;
            AcquireSRWLockShared(&g_ws_lock);
            now = g_ws_items[i];
            ReleaseSRWLockShared(&g_ws_lock);
            WsItem got = now;
            if (g_fake_steam) fake_state(now.id, &got);
            else {
                got.state = get_state(ugc, now.id);
                got.done = got.total = 0;
                if (got.state & (WS_DOWNLOADING | WS_PENDING | WS_NEEDS_UPDATE)) get_dl(ugc, now.id, &got.done, &got.total);
            }
            if (got.state != now.state || got.done != now.done || got.total != now.total) {
                AcquireSRWLockExclusive(&g_ws_lock);
                g_ws_items[i].state = got.state; g_ws_items[i].done = got.done; g_ws_items[i].total = got.total;
                ReleaseSRWLockExclusive(&g_ws_lock);
                changed = 1;
            }
        }
        if (changed) PostMessageA(g_win, WM_P_STEAM, 0, 0);
        WaitForSingleObject(g_steam_wake, 500);
    }
    return 0;
}

// ---- Files
typedef struct { char name[160]; char path[MAX_PATH * 2]; int size; } ThumbJob;
typedef struct { char name[160]; int size; BYTE *px; void *unused; } ThumbDone;   // px: size x size, 32-bit premultiplied, NULL = no picture
typedef struct { char path[MAX_PATH * 2]; unsigned long long bytes; FILETIME time; } ClashIn;
typedef struct { int gen, k; char (*names)[160]; Clash *c; } ClashDone;
static struct {
    SRWLOCK lock;
    HANDLE wake, thread;
    ThumbJob thumbs[96];
    int nthumbs;
    ClashIn *clash_in;   // the ticked packs, in order (NULL = no new request)
    char (*clash_names)[160];
    int clash_k, clash_gen;
    int want_version;
} g_io = {SRWLOCK_INIT};
static int g_clash_gen;         // main thread: the newest request
static int g_perf_version;      // the engine fix's version number (750 = 7.5), 0 = unknown

static void io_thumb(const char *name, const char *path, int size) {   // main thread: newest asks go first
    AcquireSRWLockExclusive(&g_io.lock);
    int i;
    for (i = 0; i < g_io.nthumbs; i++) if (!_stricmp(g_io.thumbs[i].name, name) && g_io.thumbs[i].size == size) break;
    if (i == g_io.nthumbs) {
        if (g_io.nthumbs == 96) memmove(&g_io.thumbs[0], &g_io.thumbs[1], sizeof g_io.thumbs[0] * 95), g_io.nthumbs--;
        ThumbJob *j = &g_io.thumbs[g_io.nthumbs++];
        snprintf(j->name, sizeof j->name, "%s", name);
        snprintf(j->path, sizeof j->path, "%s", path);
        j->size = size;
    }
    ReleaseSRWLockExclusive(&g_io.lock);
    SetEvent(g_io.wake);
}

// The picture scaled to fill size x size (the rest cropped), as premultiplied pixels; NULL when it cannot be read.
static BYTE *thumb_make(const char *path, int size) {
    WCHAR w[MAX_PATH * 2], full[MAX_PATH * 2];
    if (!gp.dll || !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH * 2) || !GetFullPathNameW(w, MAX_PATH * 2, full, NULL)) return NULL;
    void *img = NULL, *bmp = NULL, *g = NULL;
    if (gp.load(full, &img) || !img) return NULL;
    UINT iw = 0, ih = 0;
    BYTE *px = NULL;
    if (!gp.width(img, &iw) && !gp.height(img, &ih) && iw && ih && (px = calloc((size_t)size * size, 4)) &&
        !gp.bitmap_scan0(size, size, size * 4, 0xE200B /* PixelFormat32bppPARGB */, px, &bmp) && !gp.image_graphics(bmp, &g)) {
        double sc = (double)size / iw > (double)size / ih ? (double)size / iw : (double)size / ih;
        int dw = (int)(iw * sc + 1), dh = (int)(ih * sc + 1);
        gp.interp(g, 7);
        gp.pixoff(g, 4);
        gp.draw(g, img, (size - dw) / 2, (size - dh) / 2, dw, dh);
    }
    if (g) gp.del_graphics(g);
    if (bmp) gp.dispose(bmp);
    gp.dispose(img);
    if (!g) { free(px); px = NULL; }
    return px;
}

int perffix_version(const char *path);   // plugins.inc

static DWORD WINAPI io_thread(void *unused) {
    // pack file lists already read, by file and its size and time (a pack Steam updates is read again)
    typedef struct { char path[MAX_PATH * 2]; unsigned long long bytes; FILETIME time; unsigned long long *h; int n; } Cached;
    Cached *cache = NULL;
    int ncache = 0, capcache = 0;
    while (!g_quit_work) {
        ThumbJob job;
        int have_thumb = 0, version = 0, gen = 0, k = 0;
        ClashIn *in = NULL;
        char (*names)[160] = NULL;
        AcquireSRWLockExclusive(&g_io.lock);
        if (g_io.nthumbs) { job = g_io.thumbs[--g_io.nthumbs]; have_thumb = 1; }
        else if (g_io.want_version) { g_io.want_version = 0; version = 1; }
        else if (g_io.clash_in) { in = g_io.clash_in; names = g_io.clash_names; k = g_io.clash_k; gen = g_io.clash_gen; g_io.clash_in = NULL; g_io.clash_names = NULL; }
        ReleaseSRWLockExclusive(&g_io.lock);
        if (have_thumb) {
            ThumbDone *d = calloc(1, sizeof *d);
            if (d) {
                snprintf(d->name, sizeof d->name, "%s", job.name);
                d->size = job.size;
                d->px = job.path[0] ? thumb_make(job.path, job.size) : NULL;
                if (!PostMessageA(g_win, WM_P_THUMB, 0, (LPARAM)d)) { free(d->px); free(d); }
            }
        } else if (version) {
            char p[MAX_PATH * 2];
            int v = 0;
            for (int i = 0; i < 2 && !v; i++) { perffix_path(i, p, sizeof p); v = perffix_version(p); }
            PostMessageA(g_win, WM_P_INFO, 0, v);
        } else if (in) {
            unsigned long long **h = calloc((size_t)k, sizeof *h);
            int *n = calloc((size_t)k, sizeof *n);
            Clash *c = calloc((size_t)k, sizeof *c);
            for (int i = 0; h && n && c && i < k && !g_quit_work; i++) {
                int j;
                for (j = 0; j < ncache; j++)
                    if (!_stricmp(cache[j].path, in[i].path) && cache[j].bytes == in[i].bytes && !CompareFileTime(&cache[j].time, &in[i].time)) break;
                if (j == ncache) {
                    if (ncache == capcache) {
                        Cached *nc = realloc(cache, sizeof *cache * (size_t)(capcache = capcache ? capcache * 2 : 32));
                        if (!nc) break;
                        cache = nc;
                    }
                    Cached *e = &cache[ncache++];
                    snprintf(e->path, sizeof e->path, "%s", in[i].path);
                    e->bytes = in[i].bytes;
                    e->time = in[i].time;
                    e->n = pack_hashes(e->path, &e->h);
                }
                h[i] = cache[j].h;
                n[i] = cache[j].n;
            }
            if (h && n && c && !g_quit_work) {
                clashes(h, n, k, c);
                ClashDone *d = malloc(sizeof *d);
                if (d) {
                    d->gen = gen; d->k = k; d->names = names; d->c = c;
                    if (PostMessageA(g_win, WM_P_CLASH, 0, (LPARAM)d)) names = NULL, c = NULL;
                    else free(d);
                }
            }
            free(h); free(n); free(c); free(in); free(names);
        } else WaitForSingleObject(g_io.wake, INFINITE);
    }
    for (int i = 0; i < ncache; i++) free(cache[i].h);
    free(cache);
    return 0;
}

// Main thread: work out the conflict hints again for the ticked packs (after a tick, a move or a download).
static void io_clash_request(void) {
    int t = ticked_count();
    ClashIn *in = calloc((size_t)t + 1, sizeof *in);
    char (*names)[160] = calloc((size_t)t + 1, 160);
    if (!in || !names) { free(in); free(names); return; }
    int k = 0;
    for (int i = 0; i < t; i++) {
        if (g_mods[i].missing) continue;
        snprintf(in[k].path, sizeof in[k].path, "%s/%s", g_mods[i].dir[0] ? g_mods[i].dir : "data", g_mods[i].name);
        in[k].bytes = g_mods[i].size;
        in[k].time = g_mods[i].time;
        snprintf(names[k++], 160, "%s", g_mods[i].name);
    }
    AcquireSRWLockExclusive(&g_io.lock);
    free(g_io.clash_in);
    free(g_io.clash_names);
    g_io.clash_in = in;
    g_io.clash_names = names;
    g_io.clash_k = k;
    g_io.clash_gen = ++g_clash_gen;
    ReleaseSRWLockExclusive(&g_io.lock);
    SetEvent(g_io.wake);
}

static void work_start(void) {
    g_fake_steam = getenv("ATTILA_PICKER_FAKE_STEAM");
    g_steam_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_io.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_io.want_version = 1;
    g_steam_thread = CreateThread(NULL, 1 << 20, steam_thread, NULL, 0, NULL);
    g_io.thread = CreateThread(NULL, 1 << 20, io_thread, NULL, 0, NULL);
}
static void work_stop(void) {
    InterlockedExchange(&g_quit_work, 1);
    SetEvent(g_steam_wake);
    SetEvent(g_io.wake);
    // a pack list being read finishes first; Steam's first connect can take a moment
    if (g_io.thread) { WaitForSingleObject(g_io.thread, 5000); CloseHandle(g_io.thread); }
    if (g_steam_thread) { WaitForSingleObject(g_steam_thread, 5000); CloseHandle(g_steam_thread); }
    MSG m;   // results that came too late
    while (PeekMessageA(&m, g_win, WM_P_THUMB, WM_P_CLASH, PM_REMOVE)) {
        if (m.message == WM_P_THUMB) { ThumbDone *d = (ThumbDone *)m.lParam; free(d->px); free(d); }
        else { ClashDone *d = (ClashDone *)m.lParam; free(d->names); free(d->c); free(d); }
    }
}
