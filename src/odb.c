#include "odb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *object_type_name(int type)
{
    switch (type) {
    case OBJ_COMMIT: return "commit";
    case OBJ_TREE:   return "tree";
    case OBJ_BLOB:   return "blob";
    case OBJ_TAG:    return "tag";
    default:         return NULL;
    }
}

int object_type_from_name(const char *name)
{
    if (strcmp(name, "commit") == 0) return OBJ_COMMIT;
    if (strcmp(name, "tree") == 0)   return OBJ_TREE;
    if (strcmp(name, "blob") == 0)   return OBJ_BLOB;
    if (strcmp(name, "tag") == 0)    return OBJ_TAG;
    return -1;
}

void hash_object(int type, const void *data, size_t len,
                 unsigned char oid[OID_RAW])
{
    const char *name = object_type_name(type);
    if (!name)
        die("cannot hash unknown object type %d", type);

    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", name, len);
    if (hlen < 0 || (size_t)hlen >= sizeof(header))
        die("object header overflow");

    Buf b;
    buf_init(&b);
    buf_append(&b, header, (size_t)hlen + 1);
    buf_append(&b, data, len);
    sha1_buf(b.data, b.len, oid);
    buf_free(&b);
}

static char *object_path(const char *gitdir, const unsigned char oid[OID_RAW])
{
    char hex[OID_HEX + 1];
    oid_hex(oid, hex);
    Buf b;
    buf_init(&b);
    buf_appendf(&b, "%s/objects/%c%c/%s", gitdir, hex[0], hex[1], hex + 2);
    return (char *)b.data;
}

int odb_write(const char *gitdir, int type, const void *data, size_t len,
              unsigned char oid[OID_RAW])
{
    hash_object(type, data, len, oid);

    char hex[OID_HEX + 1];
    oid_hex(oid, hex);

    Buf dir;
    buf_init(&dir);
    buf_appendf(&dir, "%s/objects/%c%c", gitdir, hex[0], hex[1]);
    if (mkdir_p((char *)dir.data, 0777) != 0) {
        buf_free(&dir);
        return -1;
    }
    buf_free(&dir);

    char *path = object_path(gitdir, oid);
    Buf existing;
    buf_init(&existing);
    if (read_file(path, &existing) == 0) {
        buf_free(&existing);
        free(path);
        return 0;
    }

    const char *name = object_type_name(type);
    if (!name) {
        free(path);
        return -1;
    }

    Buf plain;
    buf_init(&plain);
    buf_appendf(&plain, "%s %zu", name, len);
    buf_putc(&plain, 0);
    buf_append(&plain, data, len);

    Buf compressed;
    buf_init(&compressed);
    if (zlib_deflate(plain.data, plain.len, &compressed) != 0) {
        buf_free(&plain);
        buf_free(&compressed);
        free(path);
        return -1;
    }

    /* Loose objects are read-only, mirroring git's behaviour. */
    int rc = write_file_atomic(path, compressed.data, compressed.len, 0444);

    buf_free(&plain);
    buf_free(&compressed);
    free(path);
    return rc;
}

int odb_read(const char *gitdir, const unsigned char oid[OID_RAW],
             int *type, Buf *out)
{
    char *path = object_path(gitdir, oid);
    Buf compressed;
    buf_init(&compressed);
    if (read_file(path, &compressed) != 0) {
        buf_free(&compressed);
        free(path);
        return 0;
    }
    free(path);

    Buf plain;
    buf_init(&plain);
    if (zlib_inflate(compressed.data, compressed.len, 0, &plain, NULL) != 0) {
        buf_free(&compressed);
        buf_free(&plain);
        return -1;
    }
    buf_free(&compressed);

    unsigned char *nul = memchr(plain.data, 0, plain.len);
    if (!nul) {
        buf_free(&plain);
        return -1;
    }
    size_t hdr_len = (size_t)(nul - plain.data);
    char *header = xstrndup((char *)plain.data, hdr_len);
    char *sp = strchr(header, ' ');
    if (!sp) {
        free(header);
        buf_free(&plain);
        return -1;
    }
    *sp = 0;
    int t = object_type_from_name(header);
    char *end = NULL;
    unsigned long long size = strtoull(sp + 1, &end, 10);
    int bad = (t < 0 || !end || *end != 0);
    free(header);
    if (bad) {
        buf_free(&plain);
        return -1;
    }

    size_t body_len = plain.len - hdr_len - 1;
    if (body_len != (size_t)size) {
        buf_free(&plain);
        return -1;
    }

    *type = t;
    if (out) {
        buf_init(out);
        buf_append(out, plain.data + hdr_len + 1, body_len);
    }
    buf_free(&plain);
    return 1;
}
