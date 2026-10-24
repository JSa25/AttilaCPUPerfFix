// Test for update.c on throwaway folders: a fake game DLL that is held open (so renaming a DLL in use is covered) and a
// fake Workshop copy, each with a version marker. Newer -> installed, .old removed on the next start; same or older,
// no marker, not a PE file, or no Workshop copy -> nothing changes.
// usage: selfupd_test.exe   (works in a temp folder; a name with "update" in it would make Windows ask for admin rights)
#include <stdarg.h>
#include "../update.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static int g_logged;
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); g_logged++; }

static wchar_t g_dir[MAX_PATH], g_self[MAX_PATH], g_ws[MAX_PATH];

// A minimal fake DLL: "MZ", padding, then the version marker (0 = none), or `body` alone if given.
static void make(const wchar_t *path, int version, const char *body) {
    FILE *f = _wfopen(path, L"wb");
    if (body) {
        fputs(body, f);
    } else {
        static char pad[0x400] = "MZ";
        fwrite(pad, 1, sizeof pad, f);
        if (version) fprintf(f, "AttilaCPUPerfFix-version:%d", version);
    }
    fclose(f);
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

int main(void) {
    GetTempPathW(MAX_PATH, g_dir);
    wcscat(g_dir, L"update_test");
    CreateDirectoryW(g_dir, NULL);
    swprintf(g_self, MAX_PATH, L"%ls\\dinput8.dll", g_dir);
    swprintf(g_ws, MAX_PATH, L"%ls\\ws_dinput8.dll", g_dir);
    wchar_t old_[MAX_PATH];
    swprintf(old_, MAX_PATH, L"%ls.old", g_self);
    DeleteFileW(old_);

    CHECK(file_version(L"no such file") == 0);
    // the game folder copy named the way an earlier update saved it; update_check passes a lowercase path
    wchar_t upper[MAX_PATH];
    swprintf(upper, MAX_PATH, L"%ls\\DINPUT8.dll", g_dir);
    DeleteFileW(g_self);
    make(upper, 4, NULL);
    CHECK(file_version(g_self) == 4);
    // keep it open the way Windows holds a loaded DLL (shared for read and delete, so renaming works)
    HANDLE held = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);

    DeleteFileW(g_ws);
    CHECK(update_from(g_self, g_ws, 4, say) == 0);   // not subscribed
    make(g_ws, 4, NULL);
    CHECK(update_from(g_self, g_ws, 4, say) == 0);   // same version
    make(g_ws, 3, NULL);
    CHECK(update_from(g_self, g_ws, 4, say) == 0);   // older: never downgrade
    make(g_ws, 0, NULL);
    CHECK(update_from(g_self, g_ws, 4, say) == 0);   // no marker
    make(g_ws, 0, "AttilaCPUPerfFix-version:9 but not a PE file");
    CHECK(update_from(g_self, g_ws, 4, say) == 0);   // not a DLL
    CHECK(g_logged == 0 && file_version(g_self) == 4);

    make(g_ws, 5, NULL);
    CHECK(update_from(g_self, g_ws, 4, say) == 1);   // newer: installed while the old file is in use
    CHECK(file_version(g_self) == 5 && exists(old_) && file_version(old_) == 4);
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(g_self, &fd);
    CHECK(fh != INVALID_HANDLE_VALUE && wcscmp(fd.cFileName, L"dinput8.dll") == 0);  // saved lowercase
    FindClose(fh);
    CloseHandle(held);
    CHECK(update_from(g_self, g_ws, 5, say) == 0);   // next start: up to date, .old cleaned up
    CHECK(!exists(old_));

    // The real binary carries its own marker (also checks the bare prefix in it is skipped).
    wchar_t me[MAX_PATH];
    GetModuleFileNameW(NULL, me, MAX_PATH);
    CHECK(file_version(me) == MOD_VERSION_NUM);
    printf("all update tests passed\n");
    return 0;
}
