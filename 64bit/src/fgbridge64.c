// heapfix/fgbridge.c (the upscaling add-on's frame bridge: shared textures and fences, motion vectors, the helper),
// unchanged, for x64: its watchdog reads a thread's position as Eip / Esp
#define Eip Rip
#define Esp Rsp
#include "fgbridge.c"
