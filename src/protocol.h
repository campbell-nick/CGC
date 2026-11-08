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
} RemoteRefs;

void remote_refs_free(RemoteRefs *r);

/* Discover capabilities and list refs via protocol v2. */
int protocol_ls_refs(const char *url, RemoteRefs *out);

/* Fetch the pack needed for all refs and unpack it into the object db. */
int protocol_fetch(const char *url, const RemoteRefs *refs, const char *gitdir);

#endif
