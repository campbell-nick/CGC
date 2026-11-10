#ifndef CGC_LOCAL_H
#define CGC_LOCAL_H

#include "protocol.h"

/* Resolve a local repository path to its git directory. Supports working
 * trees with a .git directory or gitfile, and bare repositories. */
int local_repo_gitdir(const char *path, char **out_gitdir);

/* List refs from a local repository using the same RemoteRefs shape as the
 * smart HTTP client. */
int local_ls_refs(const char *path, RemoteRefs *out);

/* Copy all reachable-by-ref objects from a local repository into gitdir,
 * unpacking source packfiles into loose objects. depth must be 0. */
int local_fetch(const char *path, const char *gitdir, int depth);

#endif
