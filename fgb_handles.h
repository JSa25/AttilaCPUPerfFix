/* Optional NT-handle transport; independent of FgbShared/protocol 4.
   Local\\fgb_<game pid>_handles. Publish before launching a helper and keep
   source handles alive until that helper exits. Values belong to the game;
   consumers must DuplicateHandle before importing them. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define FGB_HANDLES_VERSION 1u
#define FGB_HANDLE_COUNT 10u
typedef struct {
    uint32_t version, size, pid, count;
    uint64_t handles[FGB_HANDLE_COUNT]; /* game/SR/done fences, final, hudless,
                                         depth, mvec, srout0/1, optional react */
} FgbHandles;
#if defined(__cplusplus)
static_assert(sizeof(FgbHandles) == 96 && offsetof(FgbHandles, handles) == 16, "handle sidecar layout");
#else
_Static_assert(sizeof(FgbHandles) == 96 && offsetof(FgbHandles, handles) == 16, "handle sidecar layout");
#endif
