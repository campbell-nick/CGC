#ifndef CGC_REPO_H
#define CGC_REPO_H

#include "protocol.h"

/* Create the basic .git layout. */
int repo_init(const char *gitdir);

/* Write .git/config with the origin remote and default branch. */
int repo_write_config(const char *gitdir, const char *url,
                      const char *default_ref);

int repo_set_head(const char *gitdir, const char *default_ref);

/* Write a detached HEAD pointing directly at commit_oid. */
int repo_set_head_detached(const char *gitdir,
                           const unsigned char commit_oid[OID_RAW]);

typedef struct {
    /* Overwrite an existing tag whose object differs from the remote. */
    int force_tags;
    /* Skip tags entirely (fetch --no-tags). */
    int no_tags;
} RefWriteOptions;

/* Write local branches, tags and remote-tracking refs. */
int repo_write_refs(const char *gitdir, const RemoteRefs *refs);

/* Write only remote-tracking refs under refs/remotes/origin and tags, as a
 * fetch does. Local branches are left untouched. Existing tags are never
 * clobbered unless opts->force_tags is set; opts->no_tags skips tags. */
int repo_write_remote_refs(const char *gitdir, const RemoteRefs *refs,
                           const RefWriteOptions *opts);

/* Delete loose and packed refs under refs/remotes/origin that are not
 * advertised by refs. Returns 0 on success. */
int repo_prune_remote_refs(const char *gitdir, const RemoteRefs *refs);

/* Write .git/FETCH_HEAD for the refs that were fetched. merge_ref, when not
 * NULL, names the branch that a following pull would merge. */
int repo_write_fetch_head(const char *gitdir, const RemoteRefs *refs,
                          const char *url, const char *merge_ref);

/* Write .git/ORIG_HEAD with the pre-update HEAD commit. */
int repo_write_orig_head(const char *gitdir,
                         const unsigned char oid[OID_RAW]);

/* Read the origin URL from .git/config. Returns 0 on success, 1 if no origin
 * URL is configured, -1 on I/O or parse errors. */
int repo_read_origin_url(const char *gitdir, char **out_url);

/* Resolve a ref (following symrefs and packed-refs). Returns 0 if found,
 * 1 if missing, -1 on a malformed ref. */
int repo_read_ref(const char *gitdir, const char *name,
                  unsigned char oid[OID_RAW]);

/* Write a loose ref. Returns 0 on success. */
int repo_write_ref(const char *gitdir, const char *name,
                   const unsigned char oid[OID_RAW]);

/* Read HEAD. On success *out_ref is the symbolic target (or NULL for a
 * detached HEAD) and *has_oid reports whether it resolves to a commit. */
int repo_head(const char *gitdir, char **out_ref,
              unsigned char oid[OID_RAW], int *has_oid);

/* Collect the object ids of every local ref (including packed-refs and HEAD)
 * for use as `have` lines. The caller frees *oids. */
int repo_collect_local_oids(const char *gitdir,
                            unsigned char (**oids)[OID_RAW], size_t *count);

#endif
