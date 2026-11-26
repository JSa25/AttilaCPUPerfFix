// ---- Crash log and the quick-end question (1.01, 2026-11-26: players' Mod Manager closed by itself about a second after
// it started, and launcher.log showed nothing). The same file in the ATTILA and ROME II Mod Managers.
//   What each thread is doing: g_step (main thread, plus the window message), g_step_steam, g_step_files (with the picture
//   or pack it reads: g_step_path), set along the way (picker_work.inc). A crash writes one "CRASH ..." line to
//   launcher.log: the exception, where (module+offset), those steps, Windows' version, and this exe's return addresses on
//   the crashing thread's stack (the calls that led there; offsets into this exe, matched to functions with a map file).
//   Two catchers: Windows' unhandled-exception filter (taken back after Steam's overlay loads, which sets its own), and a
//   vectored handler for faults in this exe's own code (Windows can swallow those in window callbacks of a 32-bit program).
//   Quick ends: TotalWarModManager\start_check.txt counts the starts in a row that ended by themselves (no close, no Play)
//   in their first 10 seconds; after 2, the next start asks whether to go back to CA's launcher.
// Needs before it: g_home, g_game, g_args, joinp, exists, read_text_file, write_text_file, lg, launcher_version, STR, LAUNCHER_VERSION,
// launcher_choice_put, launcher_restore_ca, run_and_wait, g_steam_thread, g_io, APP_ID, the step globals.

static DWORD g_main_tid;
static char g_winver[48] = "?";
static volatile LONG g_crash_written;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;

static void winver_read(void) {   // Windows' real version (GetVersionEx answers for the manifest)
    OSVERSIONINFOW vi = {sizeof vi};
    LONG (WINAPI *get)(OSVERSIONINFOW *) = (LONG (WINAPI *)(OSVERSIONINFOW *))(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (get && !get(&vi)) snprintf(g_winver, sizeof g_winver, "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
}

static int crash_where(const void *a, char *out, int n) {   // "module+0xoffset"
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m)
        GetModuleFileNameA(m, path, sizeof path);
    const char *b = strrchr(path, '\\');
    return snprintf(out, n > 0 ? (size_t)n : 0, "%s+0x%lx", b ? b + 1 : path, (unsigned long)((const char *)a - (const char *)m));
}

static void crash_write(EXCEPTION_POINTERS *e, const char *kind) {
    if (InterlockedExchange(&g_crash_written, 1) || !g_home[0]) return;
    static char line[3072];
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    DWORD t = GetCurrentThreadId();
    const char *who = t == g_main_tid ? "main thread" : g_steam_thread && t == GetThreadId(g_steam_thread) ? "Steam thread" :
                      g_io.thread && t == GetThreadId(g_io.thread) ? "file thread" : "another thread";
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u CRASH (%s): exception 0x%08lX at ", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, kind, r->ExceptionCode);
    n += crash_where(r->ExceptionAddress, line + n, (int)sizeof line - n);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && n < (int)sizeof line)
        n += snprintf(line + n, sizeof line - n, " (%s 0x%08lX)", r->ExceptionInformation[0] == 1 ? "writing" : r->ExceptionInformation[0] == 8 ? "running" : "reading",
                      (unsigned long)r->ExceptionInformation[1]);
    if (n < (int)sizeof line)
        n += snprintf(line + n, sizeof line - n, " in the %s; main thread: %s (window message 0x%04X); Steam thread: %s; file thread: %s%s%s; "
                      "Windows %s; Total War Mod Manager " STR(LAUNCHER_VERSION) "; stack:", who, g_step, g_step_msg, g_step_steam, g_step_files,
                      g_step_path[0] ? " " : "", g_step_path, g_winver);
    // values on the crashing thread's stack that point into this exe's code (return addresses: the calls that led here,
    // nearest first; the image also holds the big static tables, so only the code section counts)
    HMODULE me = GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)me + ((const IMAGE_DOS_HEADER *)me)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    DWORD base = (DWORD)(uintptr_t)me, lo = 0, hi = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !hi; i++)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) { lo = base + sec[i].VirtualAddress; hi = lo + sec[i].Misc.VirtualSize; }
    const DWORD *sp = (const DWORD *)(uintptr_t)e->ContextRecord->Esp, *top = (const DWORD *)((NT_TIB *)NtCurrentTeb())->StackBase;
    for (int k = 0, got = 0; sp + k < top && k < 4096 && got < 20 && n < (int)sizeof line - 16; k++)
        if (sp[k] > lo && sp[k] < hi) { n += snprintf(line + n, sizeof line - n, " +0x%lx", (unsigned long)(sp[k] - base)); got++; }
    if (n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    char p[MAX_PATH];
    joinp(p, sizeof p, g_home, "launcher.log");
    HANDLE f = CreateFileA(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);   // no CRT: its heap may be the trouble
    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, line, (DWORD)n, &w, NULL); CloseHandle(f); }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    crash_write(e, "unhandled");
    return g_prev_filter ? g_prev_filter(e) : EXCEPTION_CONTINUE_SEARCH;   // Windows (or Steam's overlay) ends the program as before
}
static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *e) {   // a fault in this exe's own code is always a crash (no __try in it)
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_ILLEGAL_INSTRUCTION ||
        c == EXCEPTION_PRIV_INSTRUCTION || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || c == EXCEPTION_STACK_OVERFLOW) {
        HMODULE m = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)e->ExceptionRecord->ExceptionAddress, &m) &&
            m == GetModuleHandleA(NULL))
            crash_write(e, "in the Mod Manager's code");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void crash_catch(void) {   // at the start, and again after Steam's overlay has loaded
    LPTOP_LEVEL_EXCEPTION_FILTER p = SetUnhandledExceptionFilter(crash_filter);
    if (p != crash_filter) g_prev_filter = p;
    static int veh;
    if (!veh) { veh = 1; AddVectoredExceptionHandler(1, crash_veh); }
}

// ---- Quick ends
static char g_quick_file[MAX_PATH];
static void quick_done(void) {   // this start went well (10 seconds up, closed, or Play), or it hands over at once
    if (g_quick_file[0]) DeleteFileA(g_quick_file);
    g_quick_file[0] = 0;
}
// -1 = go on; else the exit code (the player went back to CA's launcher, which ran)
static int quick_check(void) {
    joinp(g_quick_file, sizeof g_quick_file, g_home, "start_check.txt");
    char *t = read_text_file(g_quick_file);
    int n = t ? atoi(t) : 0;
    free(t);
    if (n >= 2) {
        lg("quick ends: the last %d starts ended by themselves in their first seconds; asking about CA's launcher", n);
        const char *test = getenv("ATTILA_TEST_QUICK_ANSWER");
        int yes;
        if (test) yes = !strcmp(test, "yes");
        else if (getenv("ATTILA_PICKER_TEST")) yes = 0;   // the window tests never wait on a box
        else {
            wchar_t msg[700];
            swprintf(msg, 700, L"Total War Mod Manager closed by itself right after starting, the last %d times.\n\n"
                     L"Do you want to go back to CA's launcher?\n\n"
                     L"Yes: CA's launcher opens now, and Steam's Play button opens it from now on.\n"
                     L"No: try Total War Mod Manager again.\n\n"
                     L"What went wrong is written in TotalWarModManager\\launcher.log in the game folder. "
                     L"Posting that file on JGdaCaesar's Discord helps get it fixed.", n);
            yes = MessageBoxW(NULL, msg, L"Total War Mod Manager", MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
        }
        if (yes) {
            quick_done();
            launcher_choice_put(2, 0);   // the engine fix leaves CA's launcher in place from now on
            int back = launcher_restore_ca(g_game);   // CA's launcher.exe where Steam looks; this exe moves aside as launcher.exe.old
            char dir[MAX_PATH], ca[MAX_PATH];
            joinp(dir, sizeof dir, g_game, "launcher");
            joinp(ca, sizeof ca, dir, "launcher.exe");
            if (!back || !exists(ca) || launcher_version(ca)) joinp(ca, sizeof ca, dir, "launcher_ca.exe");
            lg(back ? "quick ends: CA's launcher is back on Steam's Play button (launcher=2)" : "quick ends: CA's launcher could not be put back; launcher=2 written");
            if (!exists(ca)) {
                MessageBoxW(NULL, L"CA's launcher file is missing. In Steam: right-click the game > Properties > Installed Files > "
                            L"\"Verify integrity of game files\". That puts CA's launcher back.", L"Total War Mod Manager", MB_ICONWARNING);
                return 1;
            }
            SetEnvironmentVariableA("SteamAppId", APP_ID);   // a start by hand: CA's launcher still knows the game
            SetEnvironmentVariableA("SteamGameId", APP_ID);
            return (int)run_and_wait(ca, g_args, dir, 0);
        }
        n = 0;   // No: counted again from this start
    }
    char v[16];
    snprintf(v, sizeof v, "%d", n + 1);
    write_text_file(g_quick_file, v);
    return -1;
}

// Tests: ATTILA_TEST_CRASH=ours (main thread, in this exe), other (main thread, in another module), files (file thread,
// in this exe, while it reads a picture), late (main thread, in another module, 10 seconds in: Steam connected by then)
static volatile int g_test_crash_late;
static void crash_test(void) {
    const char *c = getenv("ATTILA_TEST_CRASH");
    if (!c) return;
    if (!strcmp(c, "ours")) *(volatile int *)(uintptr_t)4 = 1;
    if (!strcmp(c, "other")) { void *(*volatile mc)(void *, const void *, size_t) = memcpy; mc((void *)(uintptr_t)8, "x", 1); }
    if (!strcmp(c, "files")) g_test_crash_files = 1;
    if (!strcmp(c, "late")) g_test_crash_late = 1;
}
static void crash_test_late(void) {
    if (g_test_crash_late) { void *(*volatile mc)(void *, const void *, size_t) = memcpy; mc((void *)(uintptr_t)8, "x", 1); }
}
