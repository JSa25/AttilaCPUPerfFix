// Differential test for fileio.c: the same random operations on one file through the fix (fio_*) and through plain
// Windows calls on a second handle; return values, bytes, data, positions and error codes must match at every step.
// Also: which opens are buffered or not, the .pack flag change, DuplicateHandle, close, threads, speed.
// usage: fileio_test.exe   (works in %TEMP%\fio_test)
#include <stdio.h>
#include <stdlib.h>
#include "../fileio.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (op %ld)\n", __LINE__, #c, g_op); exit(1); } } while (0)
static long g_op;
static unsigned rs = 12345;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static wchar_t dir[MAX_PATH], path[MAX_PATH];
enum { FSIZE = 3 * 1048576 + 1234 };

static LONGLONG pos_of(HANDLE h, int fix) {
    LARGE_INTEGER z = {0}, p;
    BOOL ok = fix ? fio_SetFilePointerEx(h, z, &p, FILE_CURRENT) : SetFilePointerEx(h, z, &p, FILE_CURRENT);
    CHECK(ok);
    return p.QuadPart;
}

static DWORD WINAPI hammer(LPVOID h) {
    static unsigned char b[64];
    DWORD g;
    for (int i = 0; i < 200000; i++) fio_ReadFile((HANDLE)h, b, 1 + i % 64, &g, NULL);
    return 0;
}

int main(void) {
    GetTempPathW(MAX_PATH, dir);
    wcscat(dir, L"fio_test\\");
    CreateDirectoryW(dir, NULL);
    fio_init(dir);
    swprintf(path, MAX_PATH, L"%lsdata.temp", dir);
    unsigned char *data = malloc(FSIZE);
    for (int i = 0; i < FSIZE; i++) data[i] = (unsigned char)rnd();
    HANDLE w = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    CHECK(w != INVALID_HANDLE_VALUE && WriteFile(w, data, FSIZE, &wr, NULL) && wr == FSIZE);
    CloseHandle(w);

    // which opens are buffered
    LONG before = g_fio_buffered_files;
    HANDLE a = fio_CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(a != INVALID_HANDLE_VALUE && g_fio_buffered_files == before + 1 && find(a));
    HANDLE b = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(b != INVALID_HANDLE_VALUE);
    HANDLE n1 = fio_CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(n1 == INVALID_HANDLE_VALUE || !find(n1));  // opened for writing: never buffered
    HANDLE n2 = fio_CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    CHECK(n2 != INVALID_HANDLE_VALUE && !find(n2));  // asynchronous: not buffered
    fio_CloseHandle(n1);
    fio_CloseHandle(n2);

    static unsigned char ra[300000], rb[300000];
    for (g_op = 0; g_op < 300000; g_op++) {
        unsigned k = rnd() % 100;
        if (k < 70) {  // read: mostly tiny, some medium, a few big, sometimes past the end
            DWORD n = k < 55 ? 1 + rnd() % 16 : k < 66 ? 17 + rnd() % 3000 : 60000 + rnd() % 200000;
            DWORD ga = 7, gb = 7;
            BOOL oa = fio_ReadFile(a, ra, n, &ga, NULL), ob = ReadFile(b, rb, n, &gb, NULL);
            CHECK(oa == ob && ga == gb && memcmp(ra, rb, ga) == 0);
        } else if (k < 80) {  // SetFilePointerEx, all methods, sometimes negative or past the end
            LARGE_INTEGER d, pa = {0}, pb = {0};
            DWORD m = rnd() % 3;
            d.QuadPart = (LONGLONG)(rnd() % (FSIZE + 200000)) - (m ? (LONGLONG)(rnd() % (FSIZE + 100)) : 0);
            BOOL oa = fio_SetFilePointerEx(a, d, &pa, m);
            DWORD ea = GetLastError();
            BOOL ob = SetFilePointerEx(b, d, &pb, m);
            DWORD eb = GetLastError();
            CHECK(oa == ob && (!oa || pa.QuadPart == pb.QuadPart) && (oa || ea == eb));
        } else if (k < 88) {  // SetFilePointer, with and without the high part
            LONG lo = (LONG)(rnd() % (FSIZE + 1000)) - (LONG)(rnd() % 2000), ha = 0, hb = 0;
            DWORD m = rnd() % 3;
            int usehi = rnd() & 1;
            SetLastError(1234);
            DWORD va = fio_SetFilePointer(a, lo, usehi ? &ha : NULL, m), ea = GetLastError();
            SetLastError(1234);
            DWORD vb = SetFilePointer(b, lo, usehi ? &hb : NULL, m), eb = GetLastError();
            CHECK(va == vb && ha == hb && (va != INVALID_SET_FILE_POINTER || ea == eb));
        } else if (k < 95) {  // read with an OVERLAPPED offset (synchronous handle)
            OVERLAPPED oa = {0}, ob = {0};
            DWORD off = rnd() % (FSIZE + 100), n = 1 + rnd() % 5000, ga = 0, gb = 0;
            oa.Offset = ob.Offset = off;
            BOOL ra_ok = fio_ReadFile(a, ra, n, &ga, &oa), rb_ok = ReadFile(b, rb, n, &gb, &ob);
            DWORD ea = GetLastError();
            CHECK(ra_ok == rb_ok && ga == gb && memcmp(ra, rb, ga) == 0 && (ra_ok || ea == GetLastError()));
        } else {  // compare positions
            CHECK(pos_of(a, 1) == pos_of(b, 0));
        }
    }
    printf("300000 random reads/seeks: identical results, data, positions and errors to plain Windows calls\n");
    printf("  (%ld reads through the fix, %ld refills from the file)\n", (long)g_fio_reads, (long)g_fio_refills);

    // DuplicateHandle: buffering stops for that handle and Windows' position is the logical one
    DWORD g1, g2;
    fio_ReadFile(a, ra, 5, &g1, NULL);
    ReadFile(b, rb, 5, &g2, NULL);
    HANDLE dup;
    CHECK(fio_DuplicateHandle(GetCurrentProcess(), a, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(!find(a));
    CHECK(ReadFile(dup, ra, 100, &g1, NULL) && ReadFile(b, rb, 100, &g2, NULL) && g1 == g2 && memcmp(ra, rb, g1) == 0);
    CloseHandle(dup);
    CHECK(fio_CloseHandle(a));
    CloseHandle(b);

    // The game's pattern: a writer handle stays open (write sharing) while a reader is buffered. Writes through the
    // fix must drop the reader's buffer, so the reader sees the new bytes exactly like a plain handle does.
    {
        HANDLE wh = fio_CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        HANDLE rh = fio_CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        HANDLE ph = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        CHECK(wh != INVALID_HANDLE_VALUE && rh != INVALID_HANDLE_VALUE && ph != INVALID_HANDLE_VALUE && find(rh) && !find(wh));
        for (int round = 0; round < 2000; round++) {
            DWORD ga, gb, n = 1 + rnd() % 40;
            CHECK(fio_ReadFile(rh, ra, n, &ga, NULL) && ReadFile(ph, rb, n, &gb, NULL) && ga == gb && memcmp(ra, rb, ga) == 0);
            if (rnd() % 4 == 0) {  // overwrite bytes around the current read position
                LARGE_INTEGER at = {.QuadPart = (LONGLONG)(rnd() % FSIZE)};
                unsigned char junk[100];
                for (int i = 0; i < 100; i++) junk[i] = (unsigned char)rnd();
                DWORD wr2;
                CHECK(SetFilePointerEx(wh, at, NULL, FILE_BEGIN) && fio_WriteFile(wh, junk, 100, &wr2, NULL) && wr2 == 100);
            }
            if (rnd() % 50 == 0) {  // jump somewhere
                LARGE_INTEGER to = {.QuadPart = (LONGLONG)(rnd() % FSIZE)};
                CHECK(fio_SetFilePointerEx(rh, to, NULL, FILE_BEGIN) && SetFilePointerEx(ph, to, NULL, FILE_BEGIN));
            }
        }
        // truncation through the fix: the reader must see the shorter file
        LARGE_INTEGER cut = {.QuadPart = FSIZE / 2}, back = {.QuadPart = FSIZE / 2 - 10};
        CHECK(SetFilePointerEx(wh, cut, NULL, FILE_BEGIN) && fio_SetEndOfFile(wh));
        CHECK(fio_SetFilePointerEx(rh, back, NULL, FILE_BEGIN) && SetFilePointerEx(ph, back, NULL, FILE_BEGIN));
        DWORD ga, gb;
        CHECK(fio_ReadFile(rh, ra, 100, &ga, NULL) && ReadFile(ph, rb, 100, &gb, NULL) && ga == 10 && gb == 10 && !memcmp(ra, rb, 10));
        fio_CloseHandle(rh);
        CloseHandle(ph);
        fio_CloseHandle(wh);
        printf("writer open next to a buffered reader (2,000 reads, ~500 overwrites, a truncation): reader always sees the new bytes\n");
    }

    // threads on one buffered handle: no crash, nothing left behind
    HANDLE c = fio_CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, hammer, c, 0, NULL);
    WaitForMultipleObjects(4, t, TRUE, INFINITE);
    CHECK(fio_CloseHandle(c) && g_open == 0);

    // .pack files: FILE_FLAG_NO_BUFFERING is dropped, so unaligned reads work
    wchar_t pack[MAX_PATH];
    GetTempPathW(MAX_PATH, pack);
    wcscat(pack, L"fio_test_x.pack");  // outside the user folder, like the game's data\*.pack
    CopyFileW(path, pack, FALSE);
    LONG packs = g_fio_unbuffered_packs;
    HANDLE p = fio_CreateFileW(pack, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    CHECK(p != INVALID_HANDLE_VALUE && g_fio_unbuffered_packs == packs + 1 && !find(p));  // packs are outside the user folder: not buffered
    CHECK(fio_ReadFile(p, ra, 3, &g1, NULL) && g1 == 3 && memcmp(ra, data, 3) == 0);
    fio_CloseHandle(p);
    HANDLE q = CreateFileW(pack, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    CHECK(q != INVALID_HANDLE_VALUE && !ReadFile(q, rb, 3, &g2, NULL));  // the game's flag really demands alignment
    CloseHandle(q);
    printf(".pack opened without FILE_FLAG_NO_BUFFERING (unaligned 3-byte read works; with the flag it fails)\n");

    // speed: 200,000 four-byte reads, as the save loader does
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    double ms[2];
    for (int v = 0; v < 2; v++) {
        HANDLE h = v ? fio_CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)
                     : CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 200000; i++) (v ? fio_ReadFile : ReadFile)(h, ra, 4, &g1, NULL);
        QueryPerformanceCounter(&t1);
        ms[v] = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        v ? fio_CloseHandle(h) : CloseHandle(h);
    }
    printf("200,000 reads of 4 bytes: Windows %.0f ms, through the fix %.0f ms (%.0fx)\n", ms[0], ms[1], ms[0] / ms[1]);
    DeleteFileW(pack);
    DeleteFileW(path);
    printf("all file I/O tests passed\n");
    return 0;
}
