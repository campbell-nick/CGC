#ifndef CGC_PROTOCOL_H
#define CGC_PROTOCOL_H

#include "util.h"

typedef struct {
    char *name;
    unsigned char oid[OID_RAW];
    char *symref;
    unsigned char peeled[OID_RAW];
    int has_peeled;
} Ref;

typedef struct {
    Ref *refs;
    size_t count;
    char *default_ref;             /* e.g. "refs/heads/main" */
    unsigned char default_oid[OID_RAW];
    int has_default_oid;
    int empty;                     /* remote has no commits yet */

    /* Protocol v0 capability state, populated when the server does not
     * support v2. Unused for v2 and local repositories. */
    int protocol_v0;
    int sideband;
    int sideband_64k;
    int ofs_delta;
    int no_progress;
    int shallow;
} RemoteRefs;

void remote_refs_free(RemoteRefs *r);

/* Discover capabilities and list refs via protocol v2. */
int protocol_ls_refs(const char *url, RemoteRefs *out);

/* Fetch the pack needed for all refs and unpack it into the object db.
 * If depth > 0, perform a shallow fetch of that depth and write .git/shallow.
 * haves/nhaves are object ids the client already has; they let the server
 * omit objects reachable from them. Pass NULL/0 for a fresh clone. */
int protocol_fetch(const char *url, const RemoteRefs *refs, const char *gitdir,
                   int depth, const unsigned char (*haves)[OID_RAW],
                   size_t nhaves);

#endif
