#ifndef CGC_CHECKOUT_H
#define CGC_CHECKOUT_H

#include "util.h"

/* Check out the tree of a commit into worktree and write .git/index. */
int checkout_commit(const char *gitdir, const char *worktree,
                    const unsigned char commit_oid[OID_RAW]);

#endif
