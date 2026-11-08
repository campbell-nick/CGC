#ifndef CGC_PACK_H
#define CGC_PACK_H

#include "util.h"

/* Parse a packfile and write every object it contains into the loose
 * object database at <gitdir>/objects. Resolves ofs/ref deltas. */
int pack_unpack(const unsigned char *pack, size_t len, const char *gitdir);

#endif
