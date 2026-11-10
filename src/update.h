#ifndef CGC_UPDATE_H
#define CGC_UPDATE_H

/* Fetch remote updates for the repository containing start_dir. When pull is
 * nonzero, fast-forward the current branch (or branch, when given) and update
 * the working tree. Returns 0 on success, -1 on error. */
int update_run(const char *start_dir, const char *branch, int pull);

#endif
