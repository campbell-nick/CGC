#ifndef CGC_PACK_H
#define CGC_PACK_H

#include "util.h"

typedef struct {
    uint32_t objects;
    uint32_t deltas;
} PackStats;

typedef void (*pack_delta_fn)(void *ud, uint32_t done, uint32_t total);

/* Parse a packfile and store every object it contains in <gitdir>/objects.
 * Self-contained packs are kept as a pack plus a v2 index; thin packs fall
 * back to loose objects. Resolves ofs/ref deltas. */
int pack_unpack(const unsigned char *pack, size_t len, const char *gitdir,
                PackStats *stats, pack_delta_fn on_delta, void *ud);

/* Apply a Git delta to a base object. Exposed for the pack reader. */
int pack_delta_apply(const unsigned char *base, size_t baselen,
                     const unsigned char *delta, size_t deltalen,
                     unsigned char **out, size_t *outlen);

#endif
