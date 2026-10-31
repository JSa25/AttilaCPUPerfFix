// The pools' heap checks are live. A block freed twice (too big for the per-thread cache) must stop the
// program, as the game's own heap would; with POOLS_DEBUG a block written past its end must too.
// usage: pools_check_test.exe double | overrun    (exit 3 = stopped by an abort, 4 = by an access fault; 0 = NOT caught)
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
int pools_init(void);
void *pool_alloc(size_t n);
void pool_free(void *p);
static void caught(int s) { (void)s; printf("caught (abort)\n"); fflush(stdout); _exit(3); }
static LONG WINAPI fault(EXCEPTION_POINTERS *e) {   // the block's pool tag is garbage after the error: reading the pool faults
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    printf("caught (access fault)\n");
    fflush(stdout);
    _exit(4);
}
int main(int argc, char **argv) {
    signal(SIGABRT, caught);
    AddVectoredExceptionHandler(1, fault);
    if (!pools_init()) return 1;
    if (argc > 1 && !strcmp(argv[1], "double")) {
        void *p = pool_alloc(4000);
        pool_free(p);
        pool_free(p);
    } else {
        unsigned char *p = pool_alloc(4000), *q = pool_alloc(4000);
        memset(p, 0x5A, 4000 + 64);   // over the next chunk's header
        pool_free(q);
        pool_free(p);
    }
    printf("NOT caught\n");
    return 0;
}
