#include "odb.h"
#include "pack.h"

#include <dirent.h>
#include <errno.h>
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

typedef struct {
    Buf idx;
    Buf pack;
    const unsigned char *fanout;
    const unsigned char *oids;
    const unsigned char *crcs;
    const unsigned char *offsets;
    const unsigned char *large;
    uint32_t count;
    uint32_t large_count;
} PackFile;

typedef struct {
    char *gitdir;
    PackFile *packs;
    size_t count;
    int loaded;
} PackCache;

static PackCache g_pack_cache;

static void pack_file_free(PackFile *pf)
{
    buf_free(&pf->idx);
    buf_free(&pf->pack);
    memset(pf, 0, sizeof(*pf));
}

static void pack_cache_clear(void)
{
    for (size_t i = 0; i < g_pack_cache.count; i++)
        pack_file_free(&g_pack_cache.packs[i]);
    free(g_pack_cache.packs);
    free(g_pack_cache.gitdir);
    memset(&g_pack_cache, 0, sizeof(g_pack_cache));
}

void odb_cache_clear(void)
{
    pack_cache_clear();
}

static int pack_file_load(PackFile *pf, const char *idxpath,
                          const char *packpath)
{
    memset(pf, 0, sizeof(*pf));
    if (read_file(idxpath, &pf->idx) != 0)
        return -1;
    if (pf->idx.len < 8 + 256 * 4 + 2 * OID_RAW)
        goto fail;
    if (memcmp(pf->idx.data, "\377tOc", 4) != 0)
        goto fail;
    if (get_be32(pf->idx.data + 4) != 2)
        goto fail;

    const unsigned char *fanout = pf->idx.data + 8;
    uint32_t count = get_be32(fanout + 255 * 4);
    size_t base = 8 + 256 * 4;
    size_t need = base + (size_t)count * OID_RAW + (size_t)count * 4 +
                  (size_t)count * 4 + 2 * OID_RAW;
    if (need > pf->idx.len)
        goto fail;

    const unsigned char *offsets = pf->idx.data + base +
                                   (size_t)count * OID_RAW +
                                   (size_t)count * 4;
    uint32_t large_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = get_be32(offsets + (size_t)i * 4);
        if (v & 0x80000000u) {
            uint32_t n = v & 0x7fffffffu;
            if (n + 1 > large_count)
                large_count = n + 1;
        }
    }
    if (need + (size_t)large_count * 8 != pf->idx.len)
        goto fail;

    unsigned char digest[OID_RAW];
    sha1_buf(pf->idx.data, pf->idx.len - OID_RAW, digest);
    if (memcmp(digest, pf->idx.data + pf->idx.len - OID_RAW, OID_RAW) != 0)
        goto fail;

    if (read_file(packpath, &pf->pack) != 0)
        goto fail;
    if (pf->pack.len < 12 + OID_RAW ||
        memcmp(pf->pack.data, "PACK", 4) != 0)
        goto fail;
    uint32_t version = get_be32(pf->pack.data + 4);
    if (version != 2 && version != 3)
        goto fail;
    if (get_be32(pf->pack.data + 8) != count)
        goto fail;
    if (memcmp(pf->pack.data + pf->pack.len - OID_RAW,
               pf->idx.data + pf->idx.len - 2 * OID_RAW, OID_RAW) != 0)
        goto fail;

    pf->fanout = fanout;
    pf->oids = pf->idx.data + base;
    pf->crcs = pf->oids + (size_t)count * OID_RAW;
    pf->offsets = pf->crcs + (size_t)count * 4;
    pf->large = pf->offsets + (size_t)count * 4;
    pf->count = count;
    pf->large_count = large_count;
    return 0;

fail:
    pack_file_free(pf);
    return -1;
}

static int pack_cache_load(const char *gitdir)
{
    if (g_pack_cache.loaded && g_pack_cache.gitdir &&
        strcmp(g_pack_cache.gitdir, gitdir) == 0)
        return 0;

    pack_cache_clear();
    g_pack_cache.gitdir = xstrdup(gitdir);
    g_pack_cache.loaded = 1;

    char *dir = path_join(gitdir, "objects/pack");
    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return 0;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n < 5 || strcmp(de->d_name + n - 4, ".idx") != 0)
            continue;

        char *idxpath = path_join(dir, de->d_name);
        size_t idxlen = strlen(idxpath);
        char *packpath = xmalloc(idxlen + 2);
        memcpy(packpath, idxpath, idxlen - 4);
        memcpy(packpath + idxlen - 4, ".pack", 6);

        PackFile pf;
        if (pack_file_load(&pf, idxpath, packpath) == 0) {
            g_pack_cache.packs = xrealloc(g_pack_cache.packs,
                                          (g_pack_cache.count + 1) *
                                          sizeof(*g_pack_cache.packs));
            g_pack_cache.packs[g_pack_cache.count++] = pf;
        }
        free(idxpath);
        free(packpath);
    }
    closedir(d);
    free(dir);
    return 0;
}

static int pack_find_oid(const PackFile *pf, const unsigned char oid[OID_RAW])
{
    const unsigned char *fanout = pf->fanout;
    uint32_t lo = oid[0] ? get_be32(fanout + ((uint32_t)oid[0] - 1) * 4) : 0;
    uint32_t hi = get_be32(fanout + (uint32_t)oid[0] * 4);

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(oid, pf->oids + (size_t)mid * OID_RAW, OID_RAW);
        if (cmp == 0)
            return (int)mid;
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return -1;
}

static int pack_offset_at(const PackFile *pf, uint32_t idx, uint64_t *out)
{
    uint32_t v = get_be32(pf->offsets + (size_t)idx * 4);
    if (v & 0x80000000u) {
        uint32_t n = v & 0x7fffffffu;
        if (n >= pf->large_count)
            return -1;
        *out = get_be64(pf->large + (size_t)n * 8);
    } else {
        *out = v;
    }
    return 0;
}

static int odb_read_depth(const char *gitdir, const unsigned char oid[OID_RAW],
                          int *type, Buf *out, int depth);

static int pack_read_at(const char *gitdir, const PackFile *pf,
                        uint64_t offset, int depth, int *type, Buf *out)
{
    if (depth > 64 || offset >= pf->pack.len - OID_RAW)
        return -1;

    const unsigned char *pack = pf->pack.data;
    size_t pos = (size_t)offset;
    size_t limit = pf->pack.len - OID_RAW;

    unsigned char c = pack[pos++];
    int t = (c >> 4) & 7;
    uint64_t size = c & 15;
    int shift = 4;
    while (c & 0x80) {
        if (pos >= limit || shift >= 64)
            return -1;
        c = pack[pos++];
        size |= (uint64_t)(c & 0x7f) << shift;
        shift += 7;
    }
    if (size > SIZE_MAX)
        return -1;

    uint64_t base_offset = 0;
    unsigned char base_oid[OID_RAW];
    int have_base_offset = 0;
    int have_base_oid = 0;

    if (t == OBJ_OFS_DELTA) {
        if (pos >= limit)
            return -1;
        c = pack[pos++];
        uint64_t off = c & 0x7f;
        while (c & 0x80) {
            if (pos >= limit)
                return -1;
            c = pack[pos++];
            off = ((off + 1) << 7) | (c & 0x7f);
        }
        if (off == 0 || off > offset)
            return -1;
        base_offset = offset - off;
        have_base_offset = 1;
    } else if (t == OBJ_REF_DELTA) {
        if (limit - pos < OID_RAW)
            return -1;
        memcpy(base_oid, pack + pos, OID_RAW);
        pos += OID_RAW;
        have_base_oid = 1;
    } else if (t < OBJ_COMMIT || t > OBJ_TAG) {
        return -1;
    }

    Buf inflated;
    buf_init(&inflated);
    size_t consumed = 0;
    if (zlib_inflate(pack + pos, limit - pos, (size_t)size,
                     &inflated, &consumed) != 0 ||
        inflated.len != (size_t)size) {
        buf_free(&inflated);
        return -1;
    }

    if (t >= OBJ_COMMIT && t <= OBJ_TAG) {
        *type = t;
        *out = inflated;
        return 1;
    }

    Buf base;
    buf_init(&base);
    int base_type = 0;
    int found;
    if (have_base_offset) {
        found = pack_read_at(gitdir, pf, base_offset, depth + 1,
                             &base_type, &base);
    } else if (have_base_oid) {
        found = odb_read_depth(gitdir, base_oid, &base_type, &base, depth + 1);
    } else {
        found = -1;
    }

    if (found != 1) {
        buf_free(&base);
        buf_free(&inflated);
        return -1;
    }

    unsigned char *result = NULL;
    size_t result_len = 0;
    int rc = pack_delta_apply(base.data, base.len, inflated.data,
                              inflated.len, &result, &result_len);
    buf_free(&base);
    buf_free(&inflated);
    if (rc != 0)
        return -1;

    out->data = result;
    out->len = result_len;
    out->cap = result_len;
    *type = base_type;
    return 1;
}

static int odb_read_loose(const char *gitdir, const unsigned char oid[OID_RAW],
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

static int odb_read_depth(const char *gitdir, const unsigned char oid[OID_RAW],
                          int *type, Buf *out, int depth)
{
    int r = odb_read_loose(gitdir, oid, type, out);
    if (r != 0)
        return r;
    if (depth > 64)
        return -1;

    pack_cache_load(gitdir);
    int found = 0;
    for (size_t i = 0; i < g_pack_cache.count; i++) {
        PackFile *pf = &g_pack_cache.packs[i];
        int idx = pack_find_oid(pf, oid);
        if (idx < 0)
            continue;
        uint64_t offset;
        if (pack_offset_at(pf, (uint32_t)idx, &offset) != 0) {
            found = 1;
            continue;
        }
        int rc = pack_read_at(gitdir, pf, offset, depth, type, out);
        if (rc == 1)
            return 1;
        found = 1;
    }
    return found ? -1 : 0;
}

int odb_read(const char *gitdir, const unsigned char oid[OID_RAW],
             int *type, Buf *out)
{
    return odb_read_depth(gitdir, oid, type, out, 0);
}
