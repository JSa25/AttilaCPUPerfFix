// The plugin's version is the engine fix's (heapfix/version.h): the 32-bit dinput8.dll and the 64-bit plugin ship in
// the same mod with the same number (the user, 2026-11-22). The Workshop self-update compares the marker written from it
// (heapfix/update.c); the 64-bit plugin looks only at the perffix64.dll in the Workshop item, so the shared number works.
// tools/gen_shared64.py copies heapfix/version.h to out/gen/version.h, beside the copies of fgpanel.c and update.c.
#pragma once
#include "../../shared/version.h"
