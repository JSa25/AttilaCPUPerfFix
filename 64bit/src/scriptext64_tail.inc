
// ---- the two hooks ----
typedef SV void (*create_fn)(void *out, void *parent);
typedef SV char (*release_fn)(void *state);
create_fn se_create_orig;
release_fn se_release_orig;

SV void se64_create(void *out, void *parent) {
    se_create_orig(out, parent);
    lua_State *T = *(lua_State **)out;
    if (!T) return;
    EnterCriticalSection(&g_envlock);
    g_lua_thread = GetCurrentThreadId();
    unsigned i = 0;
    while (i < g_nenv && g_env[i].env != out) i++;
    if (i < g_nenv) { g_env[i].T = T; g_env[i].frames = g_env[i].loaded = 0; }   // the same State object, made again
    else if (g_nenv < MAXENV) g_env[g_nenv++] = (typeof(g_env[0])){out, T, g_next_id++, 0, 0};
    LeaveCriticalSection(&g_envlock);
}
SV char se64_release(void *state) {
    EnterCriticalSection(&g_envlock);
    for (unsigned i = 0; i < g_nenv; i++)
        if (g_env[i].env == state) {
            if (g_env[i].loaded) outf("env %u closed", g_env[i].id);
            memmove(&g_env[i], &g_env[i + 1], (g_nenv - i - 1) * sizeof g_env[0]);
            g_nenv--;
            break;
        }
    LeaveCriticalSection(&g_envlock);
    return se_release_orig(state);
}

static int g_key_down;
void scriptext64_frame(void) {
    int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_OEM_3) & 0x8000);
    if (down && !g_key_down && our_window(GetForegroundWindow())) toggle_console();
    g_key_down = down;
    if (!g_lua_thread || GetCurrentThreadId() != g_lua_thread) return;   // Lua only on its own thread
    for (unsigned i = 0; i < g_nenv; i++) {
        if (g_env[i].loaded || ++g_env[i].frames < 2 || l_status(g_env[i].T)) continue;
        lua_State *T = g_env[i].T;
        g_env[i].loaded = 1;
        int top = l_gettop(T);
        l_pushcclosure(T, setup_fn, 0);
        if (l_pcall(T, 0, 0, 0)) outf("env %u: ext_print could not be set: %s", g_env[i].id, to_text(T, l_gettop(T)));
        l_settop(T, top);
        outf("env %u opened%s", g_env[i].id, has_global(T, "scripting") ? " (campaign scripting)" : "");
        load_scripts(T);
    }
    while (g_qtail != g_qhead) {
        char line[1024];
        EnterCriticalSection(&g_qlock);
        strcpy(line, g_q[g_qtail % QN]);
        g_qtail++;
        LeaveCriticalSection(&g_qlock);
        command(line);
    }
}

// image: the engine's image from 0x400000 on (the game itself, or the test's copy of the file). 1 = every function
// starts with the bytes expected; with `set`, the function pointers are filled in.
int scriptext64_find(const uint8_t *image, int set) {
    for (unsigned i = 0; i < sizeof API / sizeof *API; i++) {
        if (memcmp(image + (API[i].va - 0x400000), API[i].b, 8)) return 0;
        if (set) *API[i].fn = (void *)(uintptr_t)API[i].va;
    }
    return !memcmp(image + (SE_CREATE - 0x400000), SE_CREATE_B, 10) && !memcmp(image + (SE_RELEASE - 0x400000), SE_RELEASE_B, 11);
}

void scriptext64_locks(void) { InitializeCriticalSection(&g_qlock); InitializeCriticalSection(&g_envlock); }

#ifndef SCRIPTEXT64_TEST
// scriptext64_prepare(dir): before the other threads are paused. dir: the attila64 folder. 1 = lua_ext is there.
static int g_se_wanted;
int scriptext64_prepare(const wchar_t *dir) {
    _snwprintf(g_dir, MAX_PATH, L"%ls\\lua_ext", dir);
    DWORD a = GetFileAttributesW(g_dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    scriptext64_locks();
    wchar_t lp[MAX_PATH + 16];
    _snwprintf(lp, MAX_PATH + 16, L"%ls\\lua_ext.log", g_dir);
    g_log = _wfopen(lp, L"w");
    return g_se_wanted = 1;
}
// code: 96 bytes of executable memory within +-2 GB of the engine. 1 on, 0 no lua_ext folder, -1 the engine's Lua is
// not where expected, no near memory, or a thread inside a site. Runs while every other thread is paused.
int install_scriptext64(uint8_t *code) {
    if (!g_se_wanted) return 0;
    if (!code || !scriptext64_find((const uint8_t *)0x400000, 1) || thread_inside(SE_CREATE, 5) || thread_inside(SE_RELEASE, 11)) return -1;
    uint64_t a;
    // relays: jmp [rip+0] -> our functions
    code[0] = 0xFF; code[1] = 0x25; memset(code + 2, 0, 4); a = (uint64_t)(uintptr_t)se64_create; memcpy(code + 6, &a, 8);
    code[16] = 0xFF; code[17] = 0x25; memset(code + 18, 0, 4); a = (uint64_t)(uintptr_t)se64_release; memcpy(code + 22, &a, 8);
    // CreateThread's first 5 bytes ; jmp [rip+0] -> +5
    uint8_t *t = code + 32;
    memcpy(t, SE_CREATE_B, 5); t[5] = 0xFF; t[6] = 0x25; memset(t + 7, 0, 4); a = SE_CREATE + 5; memcpy(t + 11, &a, 8);
    se_create_orig = (create_fn)(void *)t;
    // ReleaseThread's 3 pushes, its rip-relative load written with the absolute address, jmp [rip+0] -> +11
    t = code + 56;
    static const uint8_t rel[12] = {0x55, 0x41, 0x56, 0x53, 0x48, 0x8b, 0x0c, 0x25, 0x88, 0x86, 0x18, 0x05};   // ... mov rcx, [0x5188688]
    memcpy(t, rel, 12); t[12] = 0xFF; t[13] = 0x25; memset(t + 14, 0, 4); a = SE_RELEASE + 11; memcpy(t + 18, &a, 8);
    se_release_orig = (release_fn)(void *)t;
    uint8_t j[11] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    int32_t r = (int32_t)((int64_t)(uintptr_t)(code + 16) - ((int64_t)SE_RELEASE + 5));
    memcpy(j + 1, &r, 4);
    if (!patch_code((void *)(uintptr_t)SE_RELEASE, j, 11)) return -1;
    r = (int32_t)((int64_t)(uintptr_t)code - ((int64_t)SE_CREATE + 5));
    memcpy(j + 1, &r, 4);
    return patch_code((void *)(uintptr_t)SE_CREATE, j, 5) ? 1 : -1;
}
#endif
