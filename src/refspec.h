#ifndef CGC_REFSPEC_H
#define CGC_REFSPEC_H

#include <stddef.h>

/* A single fetch/push refspec: [+][^]<src>[:<dst>].
 *
 * A leading '+' makes updates to the destination non-fast-forward-safe. A
 * leading '^' marks a negative refspec that excludes matching refs. A 'dst'
 * of NULL means the ref is fetched into FETCH_HEAD only. Both sides may
 * contain at most one '*' wildcard. */
typedef struct {
    char *src;
    char *dst;
    int force;
    int negative;
} Refspec;

typedef struct {
    Refspec *items;
    size_t count;
    size_t cap;
} RefspecSet;

void refspec_set_init(RefspecSet *set);
void refspec_set_free(RefspecSet *set);

/* Parse text and append it to the set. Returns 0 on success, -1 when the
 * refspec is malformed. */
int refspec_set_add(RefspecSet *set, const char *text);

/* Result codes for refspec_set_map(). */
#define REFSPEC_NO_MATCH 0
#define REFSPEC_MATCH 1
#define REFSPEC_EXCLUDED 2

/* Map a source ref name through the set. On REFSPEC_MATCH, *out_dst is set to
 * a heap-allocated destination (or NULL for a fetch-only refspec) and, when
 * out_force is non-NULL, *out_force reports whether the matching refspec was
 * forced. Returns REFSPEC_NO_MATCH, REFSPEC_MATCH, REFSPEC_EXCLUDED, or -1 on
 * allocation failure. The caller frees *out_dst. */
int refspec_set_map(const RefspecSet *set, const char *refname, char **out_dst,
                    int *out_force);

/* True when the refspec's destination pattern matches refname. */
int refspec_dst_matches(const Refspec *rs, const char *refname);

#endif
