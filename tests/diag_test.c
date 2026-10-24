// Test for diag.c: startup lines for a non-game process and for a "game" (this exe stands in for empire.retail.dll),
// then 10 deliberate access violations (recovered by a second handler): the log must get 8 EXCEPTION records with
// the right code, access type, address and a stack scan, and the program must keep running.
// usage: diag_test.exe   (writes diag_test.log next to the exe)
#include <stdarg.h>
#include "../diag.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static wchar_t g_path[MAX_PATH];
static void say(const char *fmt, ...) {
    FILE *f = _wfopen(g_path, L"a");
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

extern char fault_end[];
static LONG CALLBACK recover(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Eip = (DWORD)(UINT_PTR)fault_end;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static __attribute__((noinline)) void fault(void) {
    __asm__ volatile("movl $1, 0x00000000\n\t.globl _fault_end\n_fault_end:\n\t" ::: "memory");
}

int main(void) {
    GetModuleFileNameW(NULL, g_path, MAX_PATH);
    wcscpy(wcsrchr(g_path, L'\\') + 1, L"diag_test.log");
    DeleteFileW(g_path);
    diag_startup(NULL, g_path, say);                     // not the game: one explanation, no handler
    diag_startup(GetModuleHandleW(NULL), g_path, say);   // "the game": full startup lines + handler
    AddVectoredExceptionHandler(0, recover);             // after diag's, like the game's own handling
    for (int i = 0; i < 10; i++) fault();
    static char text[65536];
    FILE *f = _wfopen(g_path, L"rb");
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    fputs(text, stdout);
    int recs = 0;
    for (char *s = text; (s = strstr(s, "] EXCEPTION ")) != NULL; s++) recs++;
    CHECK(strstr(text, "nothing patched in this process"));
    CHECK(strstr(text, "system: Windows ") && strstr(text, "logical CPUs"));
    CHECK(strstr(text, "game: empire.retail.dll build"));
    CHECK(strstr(text, "other hook DLLs in the game folder:"));
    CHECK(recs == MAX_RECORDS);
    CHECK(strstr(text, "0xC0000005 access violation at exe+0x"));
    CHECK(strstr(text, "write to address 00000000"));
    CHECK(strstr(text, "main thread"));
    CHECK(strstr(text, "  stack: "));
    printf("\nall diag tests passed (%d records for 10 faults)\n", recs);
    return 0;
}
