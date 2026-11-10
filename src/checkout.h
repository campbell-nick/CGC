#ifndef CGC_CHECKOUT_H
#define CGC_CHECKOUT_H

#include "util.h"

/* Check out the tree of a commit into worktree and write .git/index. */
int checkout_commit(const char *gitdir, const char *worktree,
                    const unsigned char commit_oid[OID_RAW]);

/* Move the worktree from old_commit to new_commit, preserving files that did
 * not change. Refuses (and changes nothing) when an affected file has local
 * modifications or when an untracked path would be overwritten. Writes the
 * new index on success. */
int checkout_update(const char *gitdir, const char *worktree,
                    const unsigned char old_commit[OID_RAW],
                    const unsigned char new_commit[OID_RAW]);

#endif
