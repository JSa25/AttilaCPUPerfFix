// Tests uikeep.c (AGENT_LOG 75) against a fake picture table with the game's queue-step delete (FUN_1174cc20): a
// queued picture is kept, not deleted; reused and queued again it is not kept twice; past KEEP the oldest unused is
// deleted, one in use or already gone is left alone; switched off, the game's delete runs and everything kept is
// deleted. build.sh also builds it from a broken copy (deletes pictures in use) and expects a failure.
#define UIKEEP_TEST
#include "../uikeep.c"
volatile DWORD g_uikeep_tid;  // diag.c in the DLL
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
#define N (KEEP + 10)

typedef struct { char pad[0x38]; int count, pinned; int live; } entry_t;  // count at +0x38, pinned at +0x3C
typedef struct { DWORD vt; gname name; char text[16]; } tex_t;            // name at +4
static entry_t E[N];
static tex_t T[N];
static int g_dels[N];
static char g_mgr_obj;

static int index_of(const gname *n) {
    for (int i = 0; i < N; i++)
        if (T[i].name.len == n->len && !memcmp(T[i].text, n->text, n->len)) return i;
    return -1;
}
static char *__attribute__((thiscall)) fake_refs(void *self, const gname *n) {
    (void)self;
    int i = index_of(n);
    return i >= 0 && E[i].live ? (char *)&E[i] : NULL;
}
static char *__attribute__((thiscall)) fake_tex(void *self, const gname *n) {
    int i = self == &g_mgr_obj ? index_of(n) : -1;
    return i >= 0 && E[i].live ? (char *)&T[i] : NULL;
}
static void __attribute__((thiscall)) fake_delete(void *mgr, const gname *n) {
    int i = mgr == &g_mgr_obj ? index_of(n) : -1;
    if (i >= 0 && E[i].live) { E[i].live = 0; g_dels[i]++; }
}
static void acquire(int i) {  // the game's acquire: a live picture counts up, a missing one is loaded
    if (!E[i].live) { E[i].live = 1; E[i].count = 0; }
    E[i].count++;
}
static void close_panel(int i) {  // release to 0, then the queue step deletes it (through uk_delete)
    if (--E[i].count <= 0 && !E[i].pinned) uk_delete(&g_mgr_obj, &T[i].name);
}

int main(void) {
    for (int i = 0; i < N; i++) {
        T[i].name.len = (DWORD)sprintf(T[i].text, "ui/tex%04d.png", i);
        T[i].name.text = T[i].text;
    }
    g_refs_find = fake_refs; g_tex_find = fake_tex; g_delete = fake_delete;
    g_uikeep_on = 1;
    // 1) closed: kept loaded, count 0, not deleted
    acquire(0); close_panel(0);
    CHECK(E[0].live && E[0].count == 0 && g_dels[0] == 0 && g_count == 1);
    // 2) opened again: the same picture counts up (no load); closed again: not kept twice
    acquire(0);
    CHECK(E[0].count == 1 && g_dels[0] == 0);
    close_panel(0);
    CHECK(E[0].live && g_again == 1 && g_count == 1);
    // 3) texture 1 kept, then in use again; texture 2 kept, then deleted by the game; texture 3 kept, then pinned
    acquire(1); close_panel(1); acquire(1);
    acquire(2); close_panel(2); E[2].live = 0;
    acquire(3); close_panel(3); E[3].pinned = 1;
    // 4) fill past KEEP: the oldest unused are deleted the game's way, the others left alone
    for (int i = 4; i < N; i++) { acquire(i); close_panel(i); }
    CHECK(g_count == KEEP);
    CHECK(g_dels[0] == 1 && !E[0].live);                    // oldest, unused: deleted
    CHECK(E[1].live && E[1].count == 1 && g_dels[1] == 0);  // in use: left alone
    CHECK(g_dels[2] == 0);                                  // gone already: not touched
    CHECK(E[3].live && g_dels[3] == 0);                     // pinned: left alone
    CHECK(g_dels[4] == 1 && g_dels[5] == 1 && E[N - 1].live && g_dels[N - 1] == 0);
    // 5) switched off: the game's delete for this one, and everything kept is deleted
    g_uikeep_on = 0;
    acquire(N - 1); close_panel(N - 1);
    CHECK(!E[N - 1].live && g_dels[N - 1] == 1 && g_count == 0 && !E[N - 2].live && E[1].live);
    printf("uikeep ok: %ld kept, %ld again, %ld deleted as oldest, %ld gone\n", g_kept, g_again, g_freed, g_gone);
    return 0;
}
