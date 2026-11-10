#ifndef CGC_REPO_H
#define CGC_REPO_H

#include "protocol.h"
#include "refspec.h"

/* Create the basic .git layout. */
int repo_init(const char *gitdir);

/* Remote names are used in config sections and refs/remotes/<name>/. */
int repo_remote_name_is_valid(const char *name);

/* Write .git/config with the named remote and default branch. A bare
 * repository gets bare = true. A mirror gets a refs-wide fetch refspec and
 * mirror = true, and is always bare. */
int repo_write_config(const char *gitdir, const char *url,
                      const char *default_ref, const char *remote, int bare,
                      int mirror);

int repo_set_head(const char *gitdir, const char *default_ref);

/* Write a detached HEAD pointing directly at commit_oid. */
int repo_set_head_detached(const char *gitdir,
                           const unsigned char commit_oid[OID_RAW]);

/* Write a symbolic ref such as refs/remotes/origin/HEAD. */
int repo_write_symref(const char *gitdir, const char *name, const char *target);

typedef struct {
    /* Overwrite an existing tag whose object differs from the remote. */
    int force_tags;
    /* Skip tags entirely (fetch --no-tags). */
    int no_tags;
} RefWriteOptions;

/* A mapping from an advertised source ref to a local destination. dst is NULL
 * for a fetch-only refspec; such a mapping is used for FETCH_HEAD but is not
 * written. */
typedef struct {
    char *src;
    char *dst;
    char *symref;
    unsigned char oid[OID_RAW];
    int force;
} RefMapping;

void ref_mappings_free(RefMapping *maps, size_t count);

/* True when the refspec set selects refname. Tags are selected when
 * include_tags is set even without an explicit tag refspec. */
int repo_refspec_selects(const RefspecSet *set, const char *refname,
                         int include_tags);

/* Map every advertised ref through set. HEAD is skipped. When include_tags is
 * set, tags that no refspec selects are mapped to refs/tags/<name> with force
 * cleared so tag clobber protection still applies. */
int repo_map_refspecs(const RefspecSet *set, const RemoteRefs *refs,
                      int include_tags, RefMapping **out, size_t *out_count);

/* Write mapped refs. opts->force_tags and a mapping's force flag both bypass
 * tag clobber protection. Mappings with a NULL destination are ignored. */
int repo_write_mappings(const char *gitdir, const RefMapping *maps, size_t count,
                        const RefWriteOptions *opts);

/* Delete loose and packed refs under the wildcard destination namespaces in
 * set that are not present in maps. Returns 0 on success. */
int repo_prune_mappings(const char *gitdir, const RefspecSet *set,
                        const RefMapping *maps, size_t count);

/* Write local branches, tags and remote-tracking refs for remote, as a
 * non-mirror clone does. */
int repo_write_refs(const char *gitdir, const RemoteRefs *refs,
                    const char *remote);

/* Write local branches and tags only, as a bare clone does. */
int repo_write_bare_refs(const char *gitdir, const RemoteRefs *refs);

/* Write every advertised ref verbatim, as a mirror clone does. */
int repo_write_mirror_refs(const char *gitdir, const RemoteRefs *refs);

/* Write .git/FETCH_HEAD for the refs that were fetched. merge_ref, when not
 * NULL, names the branch that a following pull would merge. */
int repo_write_fetch_head(const char *gitdir, const RemoteRefs *refs,
                          const char *url, const char *merge_ref);

/* Write .git/ORIG_HEAD with the pre-update HEAD commit. */
int repo_write_orig_head(const char *gitdir,
                         const unsigned char oid[OID_RAW]);

/* Remote configuration read from .git/config. */
typedef struct {
    char *name;
    char *url;
    char **fetch;   /* [remote "<name>"] fetch lines, in config order */
    size_t nfetch;
    int mirror;
    int prune;
} RemoteConfig;

void remote_config_free(RemoteConfig *rc);

/* Read a named remote. Returns 0 on success, 1 when the remote is not
 * configured, -1 on a malformed config or I/O error. */
int repo_read_remote_config(const char *gitdir, const char *name,
                            RemoteConfig *out);

/* Find the remote to use for branch_ref (which may be NULL): the branch's
 * configured remote, then "origin", then the first configured remote. Returns
 * 0 with *out_name set, 1 when no remote is configured, -1 on error. */
int repo_find_remote(const char *gitdir, const char *branch_ref,
                     char **out_name);

/* List configured remote names in config order. The caller frees the array
 * and each name. */
int repo_list_remotes(const char *gitdir, char ***out_names, size_t *out_count);

/* Read branch.<name>.merge for a branch ref. Returns 0 on success, 1 when
 * absent, -1 on error. */
int repo_read_branch_merge(const char *gitdir, const char *branch_ref,
                           char **out_merge);

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
