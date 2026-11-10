#if defined(__APPLE__)
#undef _POSIX_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "checkout.h"
#include "odb.h"
#include "progress.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#define ST_CTIM(st) ((st)->st_ctimespec)
#define ST_MTIM(st) ((st)->st_mtimespec)
#else
#define ST_CTIM(st) ((st)->st_ctim)
#define ST_MTIM(st) ((st)->st_mtim)
#endif

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
                      const unsigned char oid[OID_RAW], uint32_t mode)
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
}

static int cmp_entry(const void *a, const void *b)
{
    const IndexEntry *ea = a;
    const IndexEntry *eb = b;
    return strcmp(ea->path, eb->path);
}

/* Reject duplicate paths and file/directory collisions (for example a symlink
 * "a" combined with a file "a/b"), which could otherwise redirect a write
 * outside the working tree. */
static int validate_index(Index *idx)
{
    if (idx->count > 1)
        qsort(idx->entries, idx->count, sizeof(IndexEntry), cmp_entry);

    for (size_t i = 0; i < idx->count; i++) {
        if (!path_is_safe_relative(idx->entries[i].path)) {
            warn_cgc("unsafe tree: invalid path '%s'",
                     idx->entries[i].path);
            return -1;
        }
    }

    for (size_t i = 1; i < idx->count; i++) {
        const char *prev = idx->entries[i - 1].path;
        const char *cur = idx->entries[i].path;
        size_t plen = strlen(prev);

        if (strcmp(prev, cur) == 0 ||
            (strncmp(cur, prev, plen) == 0 && cur[plen] == '/')) {
            warn_cgc("unsafe tree: path collision at '%s'", cur);
            return -1;
        }
    }
    return 0;
}

/* Verify that every existing parent component of rel is a real directory, not
 * a symlink or regular file. Missing components are left for mkdir_p. */
static int ensure_parents(const char *worktree, const char *rel)
{
    char *tmp = xstrdup(rel);
    int rc = 0;

    for (char *p = tmp; (p = strchr(p, '/')) != NULL; p++) {
        *p = 0;
        char *path = path_join(worktree, tmp);
        struct stat st;
        int missing = 0;

        if (lstat(path, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                warn_cgc("unsafe tree: refusing to write through '%s'", tmp);
                rc = -1;
            }
        } else if (errno == ENOENT) {
            missing = 1;
        } else {
            warn_cgc("cannot inspect '%s': %s", tmp, strerror(errno));
            rc = -1;
        }

        free(path);
        *p = '/';
        if (rc != 0 || missing)
            break;
    }

    free(tmp);
    return rc;
}

static void index_write(const char *gitdir, Index *idx)
{
    if (idx->count > 1)
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

        put_be32(&b, e->has_stat ? (uint32_t)ST_CTIM(st).tv_sec : zero);
        put_be32(&b, e->has_stat ? (uint32_t)ST_CTIM(st).tv_nsec : zero);
        put_be32(&b, e->has_stat ? (uint32_t)ST_MTIM(st).tv_sec : zero);
        put_be32(&b, e->has_stat ? (uint32_t)ST_MTIM(st).tv_nsec : zero);
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
    return path_is_safe_relative(name) && strchr(name, '/') == NULL;
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

/* Walk the tree once and collect every entry. Nothing is written yet, so the
 * total file count is known before checkout and can drive the progress meter. */
static int collect_tree(const char *gitdir, const unsigned char tree_oid[OID_RAW],
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
        if (cgc_interrupted) {
            buf_free(&tree);
            return -1;
        }
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
            if (collect_tree(gitdir, oid, rel, idx) != 0) {
                free(rel);
                buf_free(&tree);
                return -1;
            }
        } else if ((mode & 0170000) == 0160000) {
            index_add(idx, rel, oid, 0160000);
        } else if ((mode & 0170000) == 0120000) {
            index_add(idx, rel, oid, 0120000);
        } else {
            uint32_t imode = 0100000 | ((mode & 0111) ? 0755 : 0644);
            index_add(idx, rel, oid, imode);
        }
        free(rel);
    }

    buf_free(&tree);
    return 0;

corrupt:
    buf_free(&tree);
    return -1;
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

static int materialize(const char *gitdir, const char *worktree, Index *idx)
{
    progress_checkout_begin(idx->count);

    for (size_t i = 0; i < idx->count; i++) {
        if (cgc_interrupted) {
            progress_clear();
            return -1;
        }
        IndexEntry *e = &idx->entries[i];
        if (ensure_parents(worktree, e->path) != 0) {
            progress_clear();
            return -1;
        }

        char *path = path_join(worktree, e->path);
        struct stat st;
        int exists = lstat(path, &st) == 0;

        if (e->mode == 0160000) {
            if (exists && !S_ISDIR(st.st_mode)) {
                if (unlink(path) != 0) {
                    warn_cgc("cannot replace '%s': %s", e->path,
                             strerror(errno));
                    free(path);
                    progress_clear();
                    return -1;
                }
                exists = 0;
            }
            if (!exists && mkdir_p(path, 0777) != 0) {
                warn_cgc("cannot create '%s': %s", e->path, strerror(errno));
                free(path);
                progress_clear();
                return -1;
            }
            free(path);
            progress_checkout_update(i + 1, idx->count);
            continue;
        }

        if (exists) {
            if (S_ISDIR(st.st_mode)) {
                warn_cgc("cannot replace directory '%s' with a file",
                         e->path);
                free(path);
                progress_clear();
                return -1;
            }
            if (unlink(path) != 0) {
                warn_cgc("cannot replace '%s': %s", e->path,
                         strerror(errno));
                free(path);
                progress_clear();
                return -1;
            }
        }

        char *dir = path_dirname(path);
        if (mkdir_p(dir, 0777) != 0) {
            warn_cgc("cannot create directory for '%s': %s", e->path,
                     strerror(errno));
            free(dir);
            free(path);
            progress_clear();
            return -1;
        }
        free(dir);

        int symlink_mode = (e->mode == 0120000);
        if (write_blob_to_worktree(gitdir, worktree, e->path, e->oid,
                                   e->mode, symlink_mode) != 0) {
            free(path);
            progress_clear();
            return -1;
        }

        if (symlink_mode) {
            if (lstat(path, &e->st) == 0)
                e->has_stat = 1;
        } else if (stat(path, &e->st) == 0) {
            e->has_stat = 1;
        }

        free(path);
        progress_checkout_update(i + 1, idx->count);
    }

    progress_checkout_end(idx->count);
    return 0;
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
    int rc = collect_tree(gitdir, tree_copy, "", &idx);
    if (rc == 0)
        rc = validate_index(&idx);
    if (rc == 0)
        rc = materialize(gitdir, worktree, &idx);
    if (rc == 0)
        index_write(gitdir, &idx);

    for (size_t i = 0; i < idx.count; i++)
        free(idx.entries[i].path);
    free(idx.entries);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Incremental worktree update                                               */
/* ------------------------------------------------------------------------- */

/* Binary search a validated (sorted) index. */
static IndexEntry *index_find(Index *idx, const char *path)
{
    size_t lo = 0;
    size_t hi = idx->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(idx->entries[mid].path, path);
        if (cmp == 0)
            return &idx->entries[mid];
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* Read .git/index (DIRC v2 or v3) so unchanged entries can keep their stat
 * cache and staged changes can be detected. Returns 0 on success, 1 when the
 * index is missing, -1 when it is malformed or an unsupported version. */
static int index_read(const char *gitdir, Index *idx)
{
    char *path = path_join(gitdir, "index");
    Buf b;
    buf_init(&b);
    int read_rc = read_file(path, &b);
    free(path);
    if (read_rc != 0) {
        buf_free(&b);
        return 1;
    }

    if (b.len < 12 + OID_RAW || memcmp(b.data, "DIRC", 4) != 0)
        goto corrupt;

    uint32_t version = get_be32(b.data + 4);
    if (version != 2 && version != 3)
        goto corrupt;

    uint32_t count = get_be32(b.data + 8);
    size_t end = b.len - OID_RAW;
    if (count > (end - 12) / 62)
        goto corrupt;

    unsigned char digest[OID_RAW];
    sha1_buf(b.data, end, digest);
    if (memcmp(digest, b.data + end, OID_RAW) != 0)
        goto corrupt;

    size_t pos = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 62 > end)
            goto corrupt;

        const unsigned char *e = b.data + pos;
        uint32_t mode = get_be32(e + 24);
        unsigned char oid[OID_RAW];
        memcpy(oid, e + 40, OID_RAW);
        uint16_t flags = (uint16_t)(((uint16_t)e[60] << 8) | e[61]);
        int extended = (flags & 0x4000) != 0;
        size_t fixed = 62 + (extended ? 2 : 0);
        if (pos + fixed > end)
            goto corrupt;

        const unsigned char *name = e + fixed;
        size_t max_name = end - (pos + fixed);
        size_t name_len = flags & 0x0FFF;
        if (name_len >= 0x0FFF) {
            name_len = 0;
            while (name_len < max_name && name[name_len] != 0)
                name_len++;
        }
        if (name_len >= max_name || name[name_len] != 0)
            goto corrupt;

        size_t entry_len = fixed + name_len + 1;
        entry_len = (entry_len + 7) & ~(size_t)7;
        if (pos + entry_len > end)
            goto corrupt;

        if (idx->count == idx->cap) {
            idx->cap = idx->cap ? idx->cap * 2 : 64;
            idx->entries =
                xrealloc(idx->entries, idx->cap * sizeof(IndexEntry));
        }
        IndexEntry *ie = &idx->entries[idx->count++];
        memset(ie, 0, sizeof(*ie));
        ie->path = xstrndup((char *)name, name_len);
        memcpy(ie->oid, oid, OID_RAW);
        ie->mode = mode;

        uint32_t ctime_s = get_be32(e + 0);
        uint32_t ctime_ns = get_be32(e + 4);
        uint32_t mtime_s = get_be32(e + 8);
        uint32_t mtime_ns = get_be32(e + 12);
        uint32_t dev = get_be32(e + 16);
        uint32_t ino = get_be32(e + 20);
        uint32_t uid = get_be32(e + 28);
        uint32_t gid = get_be32(e + 32);
        uint32_t size = get_be32(e + 36);
        if (ctime_s || ctime_ns || mtime_s || mtime_ns || dev || ino ||
            uid || gid || size) {
            ie->has_stat = 1;
            ST_CTIM(&ie->st).tv_sec = (time_t)ctime_s;
            ST_CTIM(&ie->st).tv_nsec = (long)ctime_ns;
            ST_MTIM(&ie->st).tv_sec = (time_t)mtime_s;
            ST_MTIM(&ie->st).tv_nsec = (long)mtime_ns;
            ie->st.st_dev = (dev_t)dev;
            ie->st.st_ino = (ino_t)ino;
            ie->st.st_mode = (mode_t)mode;
            ie->st.st_uid = (uid_t)uid;
            ie->st.st_gid = (gid_t)gid;
            ie->st.st_size = (off_t)size;
        }

        pos += entry_len;
    }

    buf_free(&b);
    return 0;

corrupt:
    for (size_t i = 0; i < idx->count; i++)
        free(idx->entries[i].path);
    free(idx->entries);
    memset(idx, 0, sizeof(*idx));
    buf_free(&b);
    return -1;
}

static int read_commit_tree(const char *gitdir,
                            const unsigned char commit_oid[OID_RAW],
                            unsigned char tree_oid[OID_RAW])
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

    const unsigned char *tree = commit_tree(commit.data, commit.len);
    if (!tree) {
        buf_free(&commit);
        warn_cgc("malformed commit object");
        return -1;
    }
    memcpy(tree_oid, tree, OID_RAW);
    buf_free(&commit);
    return 0;
}

/* Hash a worktree file or symlink as a blob. Returns 1 on success. */
static int hash_worktree_blob(const char *path, int symlink_mode,
                              unsigned char oid[OID_RAW])
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;

    if (symlink_mode) {
        if (!S_ISLNK(st.st_mode))
            return 0;
        size_t cap = st.st_size > 0 ? (size_t)st.st_size + 1 : 256;
        char *target = xmalloc(cap);
        ssize_t n = readlink(path, target, cap);
        if (n < 0) {
            free(target);
            return 0;
        }
        hash_object(OBJ_BLOB, target, (size_t)n, oid);
        free(target);
        return 1;
    }

    if (!S_ISREG(st.st_mode))
        return 0;

    Buf data;
    buf_init(&data);
    if (read_file(path, &data) != 0) {
        buf_free(&data);
        return 0;
    }
    hash_object(OBJ_BLOB, data.data, data.len, oid);
    buf_free(&data);
    return 1;
}

/* True when the worktree still holds the old tree's content for this entry.
 * Gitlink (submodule) entries have no blob to compare, so they are accepted. */
static int worktree_matches(const char *worktree, const char *rel,
                            const unsigned char oid[OID_RAW], uint32_t mode)
{
    if ((mode & 0170000) == 0160000)
        return 1;

    char *path = path_join(worktree, rel);
    unsigned char got[OID_RAW];
    int symlink_mode = ((mode & 0170000) == 0120000);
    int ok = hash_worktree_blob(path, symlink_mode, got) &&
             memcmp(got, oid, OID_RAW) == 0;
    free(path);
    return ok;
}

static void remove_empty_parents(const char *worktree, const char *rel)
{
    char *tmp = xstrdup(rel);
    for (;;) {
        char *slash = strrchr(tmp, '/');
        if (!slash)
            break;
        *slash = '\0';
        char *dir = path_join(worktree, tmp);
        int rc = rmdir(dir);
        free(dir);
        if (rc != 0)
            break;
    }
    free(tmp);
}

static void stat_index_entry(const char *worktree, IndexEntry *e)
{
    e->has_stat = 0;
    if ((e->mode & 0170000) == 0160000)
        return;

    char *path = path_join(worktree, e->path);
    if ((e->mode & 0170000) == 0120000) {
        if (lstat(path, &e->st) == 0)
            e->has_stat = 1;
    } else if (stat(path, &e->st) == 0) {
        e->has_stat = 1;
    }
    free(path);
}

/* Write one entry, replacing whatever is currently at the path. */
static int write_tree_entry(const char *gitdir, const char *worktree,
                            IndexEntry *e)
{
    char *path = path_join(worktree, e->path);
    struct stat st;
    int exists = lstat(path, &st) == 0;

    if ((e->mode & 0170000) == 0160000) {
        if (exists && !S_ISDIR(st.st_mode)) {
            if (unlink(path) != 0) {
                warn_cgc("cannot replace '%s': %s", e->path, strerror(errno));
                free(path);
                return -1;
            }
            exists = 0;
        }
        if (!exists && mkdir_p(path, 0777) != 0) {
            warn_cgc("cannot create '%s': %s", e->path, strerror(errno));
            free(path);
            return -1;
        }
        free(path);
        return 0;
    }

    if (exists) {
        if (S_ISDIR(st.st_mode)) {
            if (rmdir(path) != 0) {
                warn_cgc("cannot replace directory '%s' with a file", e->path);
                free(path);
                return -1;
            }
        } else if (unlink(path) != 0) {
            warn_cgc("cannot replace '%s': %s", e->path, strerror(errno));
            free(path);
            return -1;
        }
    }

    if (ensure_parents(worktree, e->path) != 0) {
        free(path);
        return -1;
    }

    char *dir = path_dirname(path);
    if (mkdir_p(dir, 0777) != 0) {
        warn_cgc("cannot create directory for '%s': %s", e->path,
                 strerror(errno));
        free(dir);
        free(path);
        return -1;
    }
    free(dir);

    int symlink_mode = ((e->mode & 0170000) == 0120000);
    int rc = write_blob_to_worktree(gitdir, worktree, e->path, e->oid,
                                    e->mode, symlink_mode);
    free(path);
    return rc;
}

/* True when rel is a directory whose entire contents are tracked files that
 * this update deletes. Allows a tracked directory to become a file without
 * mistaking it for an untracked collision, while still refusing real untracked
 * content. */
static int dir_only_has_deleted_files(const char *worktree, const char *rel,
                                      Index *old_idx, Index *new_idx)
{
    char *dir = path_join(worktree, rel);
    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return 0;
    }

    int ok = 1;
    struct dirent *de;
    while (ok && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        Buf child_rel;
        buf_init(&child_rel);
        if (rel[0])
            buf_appendf(&child_rel, "%s/%s", rel, de->d_name);
        else
            buf_appendf(&child_rel, "%s", de->d_name);

        char *child = path_join(dir, de->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) {
            ok = 0;
        } else if (S_ISDIR(st.st_mode)) {
            /* A directory with no tracked entries is untracked. */
            size_t plen = child_rel.len;
            int tracked = 0;
            for (size_t i = 0; i < old_idx->count; i++) {
                const char *p = old_idx->entries[i].path;
                if (strncmp(p, (char *)child_rel.data, plen) == 0 &&
                    p[plen] == '/') {
                    tracked = 1;
                    break;
                }
            }
            if (!tracked ||
                !dir_only_has_deleted_files(worktree, (char *)child_rel.data,
                                            old_idx, new_idx))
                ok = 0;
        } else {
            IndexEntry *o = index_find(old_idx, (char *)child_rel.data);
            if (!o || index_find(new_idx, (char *)child_rel.data) ||
                !worktree_matches(worktree, (char *)child_rel.data, o->oid,
                                  o->mode))
                ok = 0;
        }
        free(child);
        buf_free(&child_rel);
    }

    closedir(d);
    free(dir);
    return ok;
}

int checkout_update(const char *gitdir, const char *worktree,
                    const unsigned char old_commit[OID_RAW],
                    const unsigned char new_commit[OID_RAW])
{
    unsigned char old_tree[OID_RAW];
    unsigned char new_tree[OID_RAW];
    if (read_commit_tree(gitdir, old_commit, old_tree) != 0)
        return -1;
    if (read_commit_tree(gitdir, new_commit, new_tree) != 0)
        return -1;

    Index old_idx;
    Index new_idx;
    memset(&old_idx, 0, sizeof(old_idx));
    memset(&new_idx, 0, sizeof(new_idx));

    Index disk_idx;
    memset(&disk_idx, 0, sizeof(disk_idx));

    int rc = -1;
    if (collect_tree(gitdir, old_tree, "", &old_idx) != 0)
        goto done;
    if (collect_tree(gitdir, new_tree, "", &new_idx) != 0)
        goto done;
    if (validate_index(&old_idx) != 0 || validate_index(&new_idx) != 0)
        goto done;

    /* Read the on-disk index. It should describe the old tree; if it does not,
     * something is staged and pulling would silently mix it with the update. */
    int disk_rc = index_read(gitdir, &disk_idx);
    if (disk_rc < 0) {
        warn_cgc("cannot read .git/index (unsupported or corrupt index)");
        goto done;
    }
    if (disk_rc == 0) {
        for (size_t i = 0; i < disk_idx.count; i++) {
            IndexEntry *d = &disk_idx.entries[i];
            IndexEntry *o = index_find(&old_idx, d->path);
            if (!o || o->mode != d->mode ||
                memcmp(o->oid, d->oid, OID_RAW) != 0) {
                warn_cgc("staged changes to '%s' would be lost", d->path);
                goto done;
            }
        }
        for (size_t i = 0; i < old_idx.count; i++) {
            IndexEntry *o = &old_idx.entries[i];
            if (!index_find(&disk_idx, o->path)) {
                warn_cgc("staged deletion of '%s' would be lost", o->path);
                goto done;
            }
        }
    }

    size_t changes = 0;
    for (size_t i = 0; i < new_idx.count; i++) {
        IndexEntry *n = &new_idx.entries[i];
        IndexEntry *o = index_find(&old_idx, n->path);
        if (!o || o->mode != n->mode || memcmp(o->oid, n->oid, OID_RAW) != 0)
            changes++;
    }
    for (size_t i = 0; i < old_idx.count; i++) {
        if (!index_find(&new_idx, old_idx.entries[i].path))
            changes++;
    }

    /* Preflight every affected path before writing anything, so a refusal
     * leaves the working tree untouched. */
    for (size_t i = 0; i < old_idx.count; i++) {
        IndexEntry *o = &old_idx.entries[i];
        if (index_find(&new_idx, o->path))
            continue;
        if ((o->mode & 0170000) == 0160000)
            continue;
        char *path = path_join(worktree, o->path);
        struct stat st;
        if (lstat(path, &st) == 0 &&
            !worktree_matches(worktree, o->path, o->oid, o->mode)) {
            warn_cgc("local changes to '%s' would be overwritten", o->path);
            free(path);
            goto done;
        }
        free(path);
    }

    for (size_t i = 0; i < new_idx.count; i++) {
        IndexEntry *n = &new_idx.entries[i];
        IndexEntry *o = index_find(&old_idx, n->path);
        int changed = !o || o->mode != n->mode ||
                      memcmp(o->oid, n->oid, OID_RAW) != 0;
        if (!changed)
            continue;

        if (o) {
            if (!worktree_matches(worktree, o->path, o->oid, o->mode)) {
                warn_cgc("local changes to '%s' would be overwritten", n->path);
                goto done;
            }
            continue;
        }

        char *path = path_join(worktree, n->path);
        struct stat st;
        if (lstat(path, &st) == 0) {
            /* A tracked directory that this update empties may become a
             * file; anything else at the path is untracked. */
            if (S_ISDIR(st.st_mode) &&
                dir_only_has_deleted_files(worktree, n->path, &old_idx,
                                           &new_idx)) {
                free(path);
                continue;
            }
            warn_cgc("untracked path '%s' would be overwritten", n->path);
            free(path);
            goto done;
        }
        free(path);
    }

    progress_checkout_begin(changes);
    size_t done_count = 0;

    /* Delete paths that disappeared from the new tree. */
    for (size_t i = 0; i < old_idx.count; i++) {
        IndexEntry *o = &old_idx.entries[i];
        if (index_find(&new_idx, o->path))
            continue;
        if (cgc_interrupted)
            goto fail;

        char *path = path_join(worktree, o->path);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if ((o->mode & 0170000) == 0160000) {
                if (S_ISDIR(st.st_mode) && rmdir(path) != 0) {
                    warn_cgc("cannot remove submodule directory '%s'", o->path);
                    free(path);
                    goto fail;
                }
            } else {
                if (!worktree_matches(worktree, o->path, o->oid, o->mode)) {
                    warn_cgc("local changes to '%s' would be overwritten",
                             o->path);
                    free(path);
                    goto fail;
                }
                if (S_ISDIR(st.st_mode) || unlink(path) != 0) {
                    warn_cgc("cannot remove '%s': %s", o->path,
                             strerror(errno));
                    free(path);
                    goto fail;
                }
            }
        }
        free(path);
        remove_empty_parents(worktree, o->path);
        progress_checkout_update(++done_count, changes);
    }

    /* Write added and changed paths; leave unchanged files untouched. */
    for (size_t i = 0; i < new_idx.count; i++) {
        IndexEntry *n = &new_idx.entries[i];
        IndexEntry *o = index_find(&old_idx, n->path);
        int changed = !o || o->mode != n->mode ||
                      memcmp(o->oid, n->oid, OID_RAW) != 0;
        if (!changed) {
            /* Keep the old index's stat cache. Refreshing it here would make
             * Git believe a locally modified file still matches the index. */
            IndexEntry *d = disk_rc == 0 ? index_find(&disk_idx, n->path)
                                         : NULL;
            if (d && d->has_stat) {
                n->st = d->st;
                n->has_stat = 1;
            }
            continue;
        }
        if (cgc_interrupted)
            goto fail;

        if (o) {
            if (!worktree_matches(worktree, o->path, o->oid, o->mode)) {
                warn_cgc("local changes to '%s' would be overwritten", n->path);
                goto fail;
            }
        } else {
            char *path = path_join(worktree, n->path);
            struct stat st;
            if (lstat(path, &st) == 0) {
                warn_cgc("untracked path '%s' would be overwritten", n->path);
                free(path);
                goto fail;
            }
            free(path);
        }

        if (write_tree_entry(gitdir, worktree, n) != 0)
            goto fail;
        stat_index_entry(worktree, n);
        progress_checkout_update(++done_count, changes);
    }

    index_write(gitdir, &new_idx);
    progress_checkout_end(changes);
    rc = 0;
    goto done;

fail:
    progress_clear();
    rc = -1;

done:
    for (size_t i = 0; i < disk_idx.count; i++)
        free(disk_idx.entries[i].path);
    free(disk_idx.entries);
    for (size_t i = 0; i < old_idx.count; i++)
        free(old_idx.entries[i].path);
    free(old_idx.entries);
    for (size_t i = 0; i < new_idx.count; i++)
        free(new_idx.entries[i].path);
    free(new_idx.entries);
    return rc;
}
