#ifndef CGC_REPO_H
#define CGC_REPO_H

#include "protocol.h"

/* Create the basic .git layout. */
int repo_init(const char *gitdir);

/* Write .git/config with the origin remote and default branch. */
int repo_write_config(const char *gitdir, const char *url,
                      const char *default_ref);

int repo_set_head(const char *gitdir, const char *default_ref);

/* Write local branches, tags and remote-tracking refs. */
int repo_write_refs(const char *gitdir, const RemoteRefs *refs);

#endif
