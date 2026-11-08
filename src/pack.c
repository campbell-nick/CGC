#include "pack.h"
#include "odb.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t offset;
    uint64_t size;
    int type;
    unsigned char *data;
    unsigned char base_oid[OID_RAW];
    uint64_t base_offset;
    unsigned char oid[OID_RAW];
    int resolved;
} PackEntry;

typedef struct {
    uint32_t *slots; /* index + 1; 0 means empty */
    size_t cap;
} OidMap;

static uint64_t oid_hash(const unsigned char oid[OID_RAW])
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < OID_RAW; i++) {
        h ^= oid[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void map_init(OidMap *m, size_t n)
{
    size_t cap = 16;
    while (cap < (n + 1) * 2)
        cap <<= 1;
    m->slots = xcalloc(cap, sizeof(uint32_t));
    m->cap = cap;
}

static void map_put(OidMap *m, const unsigned char oid[OID_RAW], uint32_t idx)
{
    size_t i = (size_t)(oid_hash(oid) & (m->cap - 1));
    while (m->slots[i])
        i = (i + 1) & (m->cap - 1);
    m->slots[i] = idx + 1;
}

/* Callers must compare the oid, since slots can collide. */
static int map_find(const OidMap *m, const PackEntry *entries,
                    const unsigned char oid[OID_RAW])
{
    size_t i = (size_t)(oid_hash(oid) & (m->cap - 1));
    while (m->slots[i]) {
        uint32_t idx = m->slots[i] - 1;
        if (memcmp(entries[idx].oid, oid, OID_RAW) == 0)
            return (int)idx;
        i = (i + 1) & (m->cap - 1);
    }
    return -1;
}

static int find_by_offset(const PackEntry *entries, uint32_t count,
                          uint64_t offset)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (entries[mid].offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < count && entries[lo].offset == offset)
        return (int)lo;
    return -1;
}

static int read_varint(const unsigned char **pp, const unsigned char *end,
                       uint64_t *out)
{
    const unsigned char *p = *pp;
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
        if (p >= end || shift > 63)
            return -1;
        unsigned char c = *p++;
        v |= (uint64_t)(c & 0x7f) << shift;
        shift += 7;
        if (!(c & 0x80))
            break;
    }
    *pp = p;
    *out = v;
    return 0;
}

static int delta_apply(const unsigned char *base, size_t baselen,
                       const unsigned char *delta, size_t deltalen,
                       unsigned char **out, size_t *outlen)
{
    const unsigned char *p = delta;
    const unsigned char *end = delta + deltalen;

    uint64_t base_size, result_size;
    if (read_varint(&p, end, &base_size) != 0 ||
        read_varint(&p, end, &result_size) != 0)
        return -1;
    if (base_size != baselen)
        return -1;
    if (result_size > (uint64_t)SIZE_MAX)
        return -1;

    Buf b;
    buf_init(&b);
    buf_reserve(&b, (size_t)result_size);

    while (p < end) {
        unsigned char op = *p++;
        if (op & 0x80) {
            uint64_t off = 0, size = 0;
            if (op & 0x01) { if (p >= end) goto fail; off |= (uint64_t)*p++; }
            if (op & 0x02) { if (p >= end) goto fail; off |= (uint64_t)*p++ << 8; }
            if (op & 0x04) { if (p >= end) goto fail; off |= (uint64_t)*p++ << 16; }
            if (op & 0x08) { if (p >= end) goto fail; off |= (uint64_t)*p++ << 24; }
            if (op & 0x10) { if (p >= end) goto fail; size |= (uint64_t)*p++; }
            if (op & 0x20) { if (p >= end) goto fail; size |= (uint64_t)*p++ << 8; }
            if (op & 0x40) { if (p >= end) goto fail; size |= (uint64_t)*p++ << 16; }
            if (size == 0)
                size = 0x10000;
            if (off > baselen || size > baselen - off)
                goto fail;
            if (b.len + size > (size_t)result_size)
                goto fail;
            buf_append(&b, base + off, (size_t)size);
        } else if (op != 0) {
            if ((size_t)op > (size_t)(end - p))
                goto fail;
            if (b.len + op > (size_t)result_size)
                goto fail;
            buf_append(&b, p, op);
            p += op;
        } else {
            goto fail;
        }
    }

    if (b.len != (size_t)result_size)
        goto fail;
    *out = b.data;
    *outlen = b.len;
    return 0;

fail:
    buf_free(&b);
    return -1;
}

int pack_unpack(const unsigned char *pack, size_t len, const char *gitdir)
{
    if (len < 12 + OID_RAW || memcmp(pack, "PACK", 4) != 0)
        return -1;

    uint32_t version = get_be32(pack + 4);
    uint32_t count = get_be32(pack + 8);
    if (version != 2 && version != 3)
        return -1;

    /* Integrity: the last 20 bytes are the SHA-1 of the rest. */
    unsigned char digest[OID_RAW];
    sha1_buf(pack, len - OID_RAW, digest);
    if (memcmp(digest, pack + len - OID_RAW, OID_RAW) != 0) {
        warn_cgc("packfile checksum mismatch");
        return -1;
    }

    PackEntry *entries = xcalloc(count ? count : 1, sizeof(PackEntry));
    OidMap map = {NULL, 0};
    size_t pos = 12;

    for (uint32_t i = 0; i < count; i++) {
        PackEntry *e = &entries[i];
        e->offset = pos;
        if (pos >= len)
            goto fail;

        unsigned char c = pack[pos++];
        int type = (c >> 4) & 7;
        uint64_t size = c & 15;
        int shift = 4;
        while (c & 0x80) {
            if (pos >= len || shift >= 64)
                goto fail;
            c = pack[pos++];
            size |= (uint64_t)(c & 0x7f) << shift;
            shift += 7;
        }

        if (type == OBJ_OFS_DELTA) {
            if (pos >= len)
                goto fail;
            c = pack[pos++];
            uint64_t off = c & 0x7f;
            while (c & 0x80) {
                if (pos >= len)
                    goto fail;
                c = pack[pos++];
                off = ((off + 1) << 7) | (c & 0x7f);
            }
            if (off == 0 || off > e->offset)
                goto fail;
            e->base_offset = e->offset - off;
        } else if (type == OBJ_REF_DELTA) {
            if (len - pos < OID_RAW)
                goto fail;
            memcpy(e->base_oid, pack + pos, OID_RAW);
            pos += OID_RAW;
        } else if (type < OBJ_COMMIT || type > OBJ_TAG) {
            goto fail;
        }

        Buf inflated;
        buf_init(&inflated);
        size_t consumed = 0;
        if (zlib_inflate(pack + pos, len - pos, (size_t)size,
                         &inflated, &consumed) != 0 ||
            inflated.len != (size_t)size) {
            buf_free(&inflated);
            goto fail;
        }
        pos += consumed;
        e->type = type;
        e->size = size;
        e->data = inflated.data;
    }

    if (pos + OID_RAW != len) {
        warn_cgc("packfile has trailing garbage");
        goto fail;
    }

    map_init(&map, count);

    size_t unresolved = count;
    for (uint32_t pass = 0; unresolved > 0 && pass <= count; pass++) {
        size_t progressed = 0;

        for (uint32_t i = 0; i < count; i++) {
            PackEntry *e = &entries[i];
            if (e->resolved)
                continue;

            if (e->type >= OBJ_COMMIT && e->type <= OBJ_TAG) {
                hash_object(e->type, e->data, (size_t)e->size, e->oid);
                e->resolved = 1;
                map_put(&map, e->oid, i);
                unresolved--;
                progressed++;
                continue;
            }

            const unsigned char *base = NULL;
            size_t baselen = 0;
            int basetype = 0;
            Buf basebuf;
            int have_basebuf = 0;

            if (e->type == OBJ_OFS_DELTA) {
                int j = find_by_offset(entries, count, e->base_offset);
                if (j >= 0 && entries[j].resolved) {
                    base = entries[j].data;
                    baselen = (size_t)entries[j].size;
                    basetype = entries[j].type;
                }
            } else if (e->type == OBJ_REF_DELTA) {
                int j = map_find(&map, entries, e->base_oid);
                if (j >= 0) {
                    base = entries[j].data;
                    baselen = (size_t)entries[j].size;
                    basetype = entries[j].type;
                } else {
                    buf_init(&basebuf);
                    int r = odb_read(gitdir, e->base_oid, &basetype, &basebuf);
                    if (r > 0) {
                        base = basebuf.data;
                        baselen = basebuf.len;
                        have_basebuf = 1;
                    }
                }
            } else {
                goto fail;
            }

            if (!base)
                continue;

            unsigned char *result = NULL;
            size_t result_len = 0;
            if (delta_apply(base, baselen, e->data, (size_t)e->size,
                            &result, &result_len) != 0) {
                if (have_basebuf)
                    buf_free(&basebuf);
                warn_cgc("corrupt delta in packfile");
                goto fail;
            }
            if (have_basebuf)
                buf_free(&basebuf);

            free(e->data);
            e->data = result;
            e->size = result_len;
            e->type = basetype;
            hash_object(e->type, e->data, result_len, e->oid);
            e->resolved = 1;
            map_put(&map, e->oid, i);
            unresolved--;
            progressed++;
        }

        if (!progressed)
            break;
    }

    if (unresolved) {
        warn_cgc("could not resolve %zu delta object(s)", unresolved);
        goto fail;
    }

    for (uint32_t i = 0; i < count; i++) {
        PackEntry *e = &entries[i];
        if (odb_write(gitdir, e->type, e->data, (size_t)e->size, e->oid) != 0) {
            warn_cgc("failed to write object");
            goto fail;
        }
    }

    for (uint32_t i = 0; i < count; i++)
        free(entries[i].data);
    free(entries);
    free(map.slots);
    return 0;

fail:
    for (uint32_t i = 0; i < count; i++)
        free(entries[i].data);
    free(entries);
    free(map.slots);
    return -1;
}
