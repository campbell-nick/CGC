#ifndef CGC_PACK_H
#define CGC_PACK_H

#include "util.h"

typedef struct {
    uint32_t objects;
    uint32_t deltas;
} PackStats;

typedef void (*pack_delta_fn)(void *ud, uint32_t done, uint32_t total);

/* Parse a packfile and write every object it contains into the loose
 * object database at <gitdir>/objects. Resolves ofs/ref deltas. */
int pack_unpack(const unsigned char *pack, size_t len, const char *gitdir,
                PackStats *stats, pack_delta_fn on_delta, void *ud);

#endif
