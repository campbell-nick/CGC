#ifndef CGC_ODB_H
#define CGC_ODB_H

#include "util.h"

#define OBJ_COMMIT 1
#define OBJ_TREE 2
#define OBJ_BLOB 3
#define OBJ_TAG 4
#define OBJ_OFS_DELTA 6
#define OBJ_REF_DELTA 7

const char *object_type_name(int type);
int object_type_from_name(const char *name);

void hash_object(int type, const void *data, size_t len,
                 unsigned char oid[OID_RAW]);

int odb_write(const char *gitdir, int type, const void *data, size_t len,
              unsigned char oid[OID_RAW]);

/* Returns 1 if found, 0 if absent, -1 if the object is corrupt. */
int odb_read(const char *gitdir, const unsigned char oid[OID_RAW],
             int *type, Buf *out);

#endif
