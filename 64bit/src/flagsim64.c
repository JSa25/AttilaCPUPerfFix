// flagsim64: the cloth simulation of Flag Wave (heapfix/flagsim.c, unchanged, compiled as part of this file) plus the
// one thing the 64-bit loader needs that the 32-bit game never did: forgetting the flag meshes found so far. They are
// remembered by vertex buffer, and when the game makes a new device every buffer of the old one is gone; a buffer of
// the new device can be handed the same address, and a prop drawn from it must not be taken for a flag.
#include "../../shared/flagsim.c"

void fs64_forget_models(void) {
    AcquireSRWLockExclusive(&fsg_lock);
    memset(fsg_models, 0, sizeof fsg_models);
    memset((void *)fsg_bufs, 0, sizeof fsg_bufs);   // the buffers that hold flags (flagsim.c since 2026-11-21)
    fsg_nmodels = 0;
    ReleaseSRWLockExclusive(&fsg_lock);
}
