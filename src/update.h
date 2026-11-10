#ifndef CGC_UPDATE_H
#define CGC_UPDATE_H

typedef struct {
    int force_tags; /* overwrite conflicting local tags */
    int no_tags;    /* do not fetch or update tags */
    int prune;      /* delete stale refs under refs/remotes/origin */
} UpdateOptions;

/* Fetch remote updates for the repository containing start_dir. When pull is
 * nonzero, fast-forward the current branch (or branch, when given) and update
 * the working tree. Returns 0 on success, -1 on error. */
int update_run(const char *start_dir, const char *branch, int pull,
               const UpdateOptions *opts);

#endif
