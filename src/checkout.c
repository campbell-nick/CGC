#include "checkout.h"
#include "odb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *path;
    unsigned char oid[OID_RAW];
    uint32_t mode;
    struct stat st;
    int has_stat;
} IndexEntry;

typedef struct {
    IndexEntry *entries;
    size_t count;
    size_t cap;
} Index;

static void index_add(Index *idx, const char *path,
                      const unsigned char oid[OID_RAW], uint32_t mode,
                      const struct stat *st)
{
    if (idx->count == idx->cap) {
        idx->cap = idx->cap ? idx->cap * 2 : 64;
        idx->entries = xrealloc(idx->entries, idx->cap * sizeof(IndexEntry));
    }
    IndexEntry *e = &idx->entries[idx->count++];
    memset(e, 0, sizeof(*e));
    e->path = xstrdup(path);
    memcpy(e->oid, oid, OID_RAW);
    e->mode = mode;
    if (st) {
        e->st = *st;
        e->has_stat = 1;
    }
}

static int cmp_entry(const void *a, const void *b)
{
    const IndexEntry *ea = a;
    const IndexEntry *eb = b;
    return strcmp(ea->path, eb->path);
}

static void index_write(const char *gitdir, Index *idx)
{
    qsort(idx->entries, idx->count, sizeof(IndexEntry), cmp_entry);

    Buf b;
    buf_init(&b);
    buf_append(&b, "DIRC", 4);
    put_be32(&b, 2);
    put_be32(&b, (uint32_t)idx->count);

    for (size_t i = 0; i < idx->count; i++) {
        IndexEntry *e = &idx->entries[i];
        const struct stat *st = &e->st;
        uint32_t zero = 0;

        put_be32(&b, e->has_stat ? (uint32_t)st->st_ctim.tv_sec : zero);
        put_be32(&b, e->has_stat ? (uint32_t)st->st_ctim.tv_nsec : zero);
        put_be32(&b, e->has_stat ? (uint32_t)st->st_mtim.tv_sec : zero);
        put_be32(&b, e->has_stat ? (uint32_t)st->st_mtim.tv_nsec : zero);
        put_be32(&b, e->has_stat ? (uint32_t)st->st_dev : zero);
        put_be32(&b, e->has_stat ? (uint32_t)st->st_ino : zero);
        put_be32(&b, e->mode);
        put_be32(&b, e->has_stat ? (uint32_t)st->st_uid : zero);
        put_be32(&b, e->has_stat ? (uint32_t)st->st_gid : zero);
        put_be32(&b, e->has_stat ? (uint32_t)st->st_size : zero);
        buf_append(&b, e->oid, OID_RAW);

        size_t namelen = strlen(e->path);
        uint16_t flags = (uint16_t)(namelen < 0xFFF ? namelen : 0xFFF);
        unsigned char fl[2] = {(unsigned char)(flags >> 8),
                               (unsigned char)flags};
        buf_append(&b, fl, 2);
        buf_append(&b, e->path, namelen);
        buf_putc(&b, 0);

        size_t entry_len = 62 + namelen + 1;
        while (entry_len % 8) {
            buf_putc(&b, 0);
            entry_len++;
        }
    }

    unsigned char digest[OID_RAW];
    sha1_buf(b.data, b.len, digest);
    buf_append(&b, digest, OID_RAW);

    char *path = path_join(gitdir, "index");
    write_file(path, b.data, b.len, 0644);
    free(path);
    buf_free(&b);
}

static int parse_mode(const unsigned char *s, size_t len, unsigned *mode)
{
    unsigned m = 0;
    if (len == 0)
        return -1;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '7')
            return -1;
        m = (m << 3) | (unsigned)(s[i] - '0');
    }
    *mode = m;
    return 0;
}

static int valid_name(const char *name)
{
    if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    if (strchr(name, '/') || strchr(name, '\\'))
        return 0;
    return 1;
}

static char *join_rel(const char *prefix, const char *name)
{
    if (!*prefix)
        return xstrdup(name);
    Buf b;
    buf_init(&b);
    buf_appendf(&b, "%s/%s", prefix, name);
    return (char *)b.data;
}

static int write_blob_to_worktree(const char *gitdir, const char *worktree,
                                  const char *relpath,
                                  const unsigned char oid[OID_RAW],
                                  unsigned file_mode, int symlink_mode)
{
    int type = 0;
    Buf data;
    buf_init(&data);
    int r = odb_read(gitdir, oid, &type, &data);
    if (r <= 0 || type != OBJ_BLOB) {
        buf_free(&data);
        warn_cgc("missing blob %s", relpath);
        return -1;
    }

    char *path = path_join(worktree, relpath);
    char *dir = path_dirname(path);
    if (mkdir_p(dir, 0777) != 0) {
        free(dir);
        free(path);
        buf_free(&data);
        return -1;
    }
    free(dir);

    int rc = 0;
    if (symlink_mode) {
        if (data.len >= data.cap)
            buf_reserve(&data, 1);
        data.data[data.len] = 0;
        unlink(path);
        if (symlink((char *)data.data, path) != 0)
            rc = -1;
    } else {
        if (write_file(path, data.data, data.len,
                       file_mode & 0111 ? 0755 : 0644) != 0)
            rc = -1;
    }
    free(path);
    buf_free(&data);
    return rc;
}

static int checkout_tree(const char *gitdir, const char *worktree,
                         const unsigned char tree_oid[OID_RAW],
                         const char *prefix, Index *idx)
{
    int type = 0;
    Buf tree;
    buf_init(&tree);
    int r = odb_read(gitdir, tree_oid, &type, &tree);
    if (r <= 0 || type != OBJ_TREE) {
        buf_free(&tree);
        warn_cgc("missing tree object");
        return -1;
    }

    size_t pos = 0;
    while (pos < tree.len) {
        size_t space = pos;
        while (space < tree.len && tree.data[space] != ' ')
            space++;
        if (space >= tree.len)
            goto corrupt;

        unsigned mode;
        if (parse_mode(tree.data + pos, space - pos, &mode) != 0)
            goto corrupt;

        size_t name_start = space + 1;
        size_t nul = name_start;
        while (nul < tree.len && tree.data[nul] != 0)
            nul++;
        if (nul >= tree.len || nul + 1 + OID_RAW > tree.len)
            goto corrupt;

        char *name = xstrndup((char *)tree.data + name_start, nul - name_start);
        if (!valid_name(name)) {
            warn_cgc("refusing unsafe tree entry '%s'", name);
            free(name);
            goto corrupt;
        }
        const unsigned char *oid = tree.data + nul + 1;
        pos = nul + 1 + OID_RAW;

        char *rel = join_rel(prefix, name);
        free(name);

        if ((mode & 0170000) == 0040000) {
            char *path = path_join(worktree, rel);
            char *dir = path_dirname(path);
            mkdir_p(dir, 0777);
            free(dir);
            free(path);
            if (checkout_tree(gitdir, worktree, oid, rel, idx) != 0) {
                free(rel);
                buf_free(&tree);
                return -1;
            }
        } else if ((mode & 0170000) == 0160000) {
            /* Submodule: create the mount point, but do not fetch it. */
            char *path = path_join(worktree, rel);
            mkdir_p(path, 0777);
            free(path);
            index_add(idx, rel, oid, 0160000, NULL);
        } else if ((mode & 0170000) == 0120000) {
            if (write_blob_to_worktree(gitdir, worktree, rel, oid, mode, 1) != 0) {
                free(rel);
                buf_free(&tree);
                return -1;
            }
            char *path = path_join(worktree, rel);
            struct stat st;
            if (lstat(path, &st) == 0)
                index_add(idx, rel, oid, 0120000, &st);
            free(path);
        } else {
            if (write_blob_to_worktree(gitdir, worktree, rel, oid, mode, 0) != 0) {
                free(rel);
                buf_free(&tree);
                return -1;
            }
            char *path = path_join(worktree, rel);
            struct stat st;
            if (stat(path, &st) == 0) {
                uint32_t imode = 0100000 | ((mode & 0111) ? 0755 : 0644);
                index_add(idx, rel, oid, imode, &st);
            }
            free(path);
        }
        free(rel);
    }

    buf_free(&tree);
    return 0;

corrupt:
    buf_free(&tree);
    return -1;
}

static const unsigned char *commit_tree(const unsigned char *data, size_t len)
{
    static unsigned char oid[OID_RAW];
    if (len < 5 || memcmp(data, "tree ", 5) != 0)
        return NULL;
    if (len < 5 + OID_HEX || data[5 + OID_HEX] != '\n')
        return NULL;
    char hex[OID_HEX + 1];
    memcpy(hex, data + 5, OID_HEX);
    hex[OID_HEX] = 0;
    if (oid_parse(hex, oid) != 0)
        return NULL;
    return oid;
}

int checkout_commit(const char *gitdir, const char *worktree,
                    const unsigned char commit_oid[OID_RAW])
{
    int type = 0;
    Buf commit;
    buf_init(&commit);
    int r = odb_read(gitdir, commit_oid, &type, &commit);
    if (r <= 0 || type != OBJ_COMMIT) {
        buf_free(&commit);
        warn_cgc("missing commit object");
        return -1;
    }

    const unsigned char *tree_oid = commit_tree(commit.data, commit.len);
    if (!tree_oid) {
        buf_free(&commit);
        warn_cgc("malformed commit object");
        return -1;
    }

    unsigned char tree_copy[OID_RAW];
    memcpy(tree_copy, tree_oid, OID_RAW);
    buf_free(&commit);

    Index idx;
    memset(&idx, 0, sizeof(idx));
    int rc = checkout_tree(gitdir, worktree, tree_copy, "", &idx);
    if (rc == 0)
        index_write(gitdir, &idx);

    for (size_t i = 0; i < idx.count; i++)
        free(idx.entries[i].path);
    free(idx.entries);
    return rc;
}
