#include "repo.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int text_is_safe(const char *s)
{
    if (!s)
        return 1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f)
            return 0;
    }
    return 1;
}

int repo_init(const char *gitdir)
{
    const char *dirs[] = {
        "",
        "objects",
        "objects/info",
        "objects/pack",
        "refs",
        "refs/heads",
        "refs/tags",
        "refs/remotes",
        "refs/remotes/origin",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char *p = path_join(gitdir, dirs[i]);
        int rc = mkdir_p(p, 0777);
        free(p);
        if (rc != 0)
            return -1;
    }

    char *head = path_join(gitdir, "HEAD");
    const char *head_content = "ref: refs/heads/main\n";
    int rc = write_file(head, head_content, strlen(head_content), 0644);
    free(head);

    char *desc = path_join(gitdir, "description");
    const char *desc_content = "Unnamed repository; edit this file to name it.\n";
    write_file(desc, desc_content, strlen(desc_content), 0644);
    free(desc);

    return rc;
}

/* Write path through a sibling .lock file and rename it into place, matching
 * Git's ref update discipline. The caller must ensure the parent directory
 * exists. Returns 0 on success. */
static int write_locked_file(const char *path, const void *data, size_t len,
                             mode_t mode)
{
    Buf lock;
    buf_init(&lock);
    buf_appendf(&lock, "%s.lock", path);

    int fd = open((char *)lock.data, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) {
        buf_free(&lock);
        return -1;
    }

    int rc = -1;
    if (fchmod(fd, mode) != 0)
        goto fail;

    const unsigned char *p = data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto fail;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    if (fsync(fd) != 0)
        goto fail;
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;

    if (rename((char *)lock.data, path) != 0)
        goto fail;
    rc = 0;

fail:
    if (fd >= 0)
        close(fd);
    if (rc != 0)
        unlink((char *)lock.data);
    buf_free(&lock);
    return rc;
}

static int write_ref_file(const char *gitdir, const char *name,
                          const unsigned char oid[OID_RAW])
{
    if (!ref_name_is_valid(name))
        return -1;
    char hex[OID_HEX + 1];
    oid_hex(oid, hex);

    char *path = path_join(gitdir, name);
    char *dir = path_dirname(path);
    int rc = -1;
    if (mkdir_p(dir, 0777) == 0) {
        char line[OID_HEX + 2];
        memcpy(line, hex, OID_HEX);
        line[OID_HEX] = '\n';
        rc = write_locked_file(path, line, OID_HEX + 1, 0644);
    }
    free(dir);
    free(path);
    return rc;
}

static int write_symref_file(const char *gitdir, const char *name,
                             const char *target)
{
    if (!ref_name_is_valid(name) || !ref_name_is_valid(target))
        return -1;
    char *path = path_join(gitdir, name);
    char *dir = path_dirname(path);
    int rc = -1;
    if (mkdir_p(dir, 0777) == 0) {
        Buf b;
        buf_init(&b);
        buf_appendf(&b, "ref: %s\n", target);
        rc = write_locked_file(path, b.data, b.len, 0644);
        buf_free(&b);
    }
    free(dir);
    free(path);
    return rc;
}

int repo_write_refs(const char *gitdir, const RemoteRefs *refs)
{
    for (size_t i = 0; i < refs->count; i++) {
        const Ref *r = &refs->refs[i];
        if (strcmp(r->name, "HEAD") == 0)
            continue;

        int rc;
        if (strncmp(r->name, "refs/heads/", 11) == 0) {
            if (r->symref)
                rc = write_symref_file(gitdir, r->name, r->symref);
            else
                rc = write_ref_file(gitdir, r->name, r->oid);
            if (rc != 0)
                return -1;

            Buf remote;
            buf_init(&remote);
            buf_appendf(&remote, "refs/remotes/origin/%s", r->name + 11);
            if (r->symref)
                rc = write_symref_file(gitdir, (char *)remote.data, r->symref);
            else
                rc = write_ref_file(gitdir, (char *)remote.data, r->oid);
            buf_free(&remote);
            if (rc != 0)
                return -1;
        } else if (strncmp(r->name, "refs/tags/", 10) == 0) {
            if (r->symref)
                rc = write_symref_file(gitdir, r->name, r->symref);
            else
                rc = write_ref_file(gitdir, r->name, r->oid);
            if (rc != 0)
                return -1;
        }
    }

    if (refs->default_ref && strncmp(refs->default_ref, "refs/heads/", 11) == 0) {
        Buf target;
        buf_init(&target);
        buf_appendf(&target, "refs/remotes/origin/%s", refs->default_ref + 11);
        int rc = write_symref_file(gitdir, "refs/remotes/origin/HEAD",
                                   (char *)target.data);
        buf_free(&target);
        if (rc != 0)
            return -1;
    }
    return 0;
}

int repo_set_head(const char *gitdir, const char *default_ref)
{
    if (!ref_name_is_valid(default_ref))
        return -1;
    char *path = path_join(gitdir, "HEAD");
    Buf b;
    buf_init(&b);
    buf_appendf(&b, "ref: %s\n", default_ref);
    int rc = write_locked_file(path, b.data, b.len, 0644);
    buf_free(&b);
    free(path);
    return rc;
}

int repo_set_head_detached(const char *gitdir,
                           const unsigned char commit_oid[OID_RAW])
{
    char hex[OID_HEX + 1];
    oid_hex(commit_oid, hex);
    char *path = path_join(gitdir, "HEAD");
    char line[OID_HEX + 2];
    memcpy(line, hex, OID_HEX);
    line[OID_HEX] = '\n';
    int rc = write_locked_file(path, line, OID_HEX + 1, 0644);
    free(path);
    return rc;
}

int repo_write_config(const char *gitdir, const char *url,
                      const char *default_ref)
{
    if (!text_is_safe(url) || !text_is_safe(default_ref))
        return -1;

    Buf b;
    buf_init(&b);
    buf_appendf(&b,
                "[core]\n"
                "\trepositoryformatversion = 0\n"
                "\tfilemode = true\n"
                "\tbare = false\n"
                "\tlogallrefupdates = true\n"
                "[remote \"origin\"]\n"
                "\turl = %s\n"
                "\tfetch = +refs/heads/*:refs/remotes/origin/*\n",
                url);
    if (default_ref && strncmp(default_ref, "refs/heads/", 11) == 0)
        buf_appendf(&b,
                    "[branch \"%s\"]\n"
                    "\tremote = origin\n"
                    "\tmerge = %s\n",
                    default_ref + 11, default_ref);

    char *path = path_join(gitdir, "config");
    int rc = write_file(path, b.data, b.len, 0644);
    buf_free(&b);
    free(path);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Ref and config reading                                                    */
/* ------------------------------------------------------------------------- */

static char *trim_ws(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return s;
}

/* Look up a ref in .git/packed-refs. Returns 0 if found, 1 if absent, -1 on
 * a malformed entry. */
static int packed_ref_lookup(const char *gitdir, const char *name,
                             unsigned char oid[OID_RAW])
{
    char *path = path_join(gitdir, "packed-refs");
    Buf b;
    buf_init(&b);
    int rc = read_file(path, &b);
    free(path);
    if (rc != 0) {
        buf_free(&b);
        return 1;
    }

    rc = 1;
    char *save = NULL;
    for (char *line = strtok_r((char *)b.data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(line);
        while (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';
        if (len < OID_HEX + 2 || line[0] == '#' || line[0] == '^' ||
            line[OID_HEX] != ' ')
            continue;
        if (strcmp(line + OID_HEX + 1, name) != 0)
            continue;

        char hex[OID_HEX + 1];
        memcpy(hex, line, OID_HEX);
        hex[OID_HEX] = '\0';
        rc = oid_parse(hex, oid) == 0 ? 0 : -1;
        break;
    }
    buf_free(&b);
    return rc;
}

static int resolve_ref(const char *gitdir, const char *name,
                       unsigned char oid[OID_RAW], int depth)
{
    if (depth > 8)
        return -1;

    char *path = path_join(gitdir, name);
    Buf b;
    buf_init(&b);
    int read_rc = read_file(path, &b);
    free(path);
    if (read_rc != 0) {
        buf_free(&b);
        return packed_ref_lookup(gitdir, name, oid);
    }

    char *s = trim_ws((char *)b.data);
    int rc;
    if (strncmp(s, "ref: ", 5) == 0) {
        char *target = trim_ws(s + 5);
        if (!ref_name_is_valid(target))
            rc = -1;
        else
            rc = resolve_ref(gitdir, target, oid, depth + 1);
    } else {
        rc = oid_parse(s, oid) == 0 ? 0 : -1;
    }
    buf_free(&b);
    return rc;
}

int repo_read_ref(const char *gitdir, const char *name,
                  unsigned char oid[OID_RAW])
{
    if (!ref_name_is_valid(name))
        return -1;
    return resolve_ref(gitdir, name, oid, 0);
}

int repo_write_ref(const char *gitdir, const char *name,
                   const unsigned char oid[OID_RAW])
{
    return write_ref_file(gitdir, name, oid);
}

static int refs_has_name(const RemoteRefs *refs, const char *name)
{
    for (size_t i = 0; i < refs->count; i++)
        if (strcmp(refs->refs[i].name, name) == 0)
            return 1;
    return 0;
}

int repo_write_remote_refs(const char *gitdir, const RemoteRefs *refs,
                           const RefWriteOptions *opts)
{
    int fetch_tags = !opts || !opts->no_tags;

    /* Preflight every tag before writing anything. A clobber refusal must not
     * leave the remote-tracking refs half updated. */
    if (fetch_tags) {
        for (size_t i = 0; i < refs->count; i++) {
            const Ref *r = &refs->refs[i];
            if (strncmp(r->name, "refs/tags/", 10) != 0 || r->symref)
                continue;

            unsigned char existing[OID_RAW];
            int rr = repo_read_ref(gitdir, r->name, existing);
            if (rr < 0) {
                warn_cgc("cannot read local tag '%s'", r->name + 10);
                return -1;
            }
            if (rr == 0 && memcmp(existing, r->oid, OID_RAW) != 0 &&
                (!opts || !opts->force_tags)) {
                warn_cgc("tag '%s' would be clobbered "
                         "(use --force to overwrite)",
                         r->name + 10);
                return -1;
            }
        }
    }

    for (size_t i = 0; i < refs->count; i++) {
        const Ref *r = &refs->refs[i];
        int rc;
        if (strncmp(r->name, "refs/heads/", 11) == 0) {
            Buf remote;
            buf_init(&remote);
            buf_appendf(&remote, "refs/remotes/origin/%s", r->name + 11);
            if (r->symref)
                rc = write_symref_file(gitdir, (char *)remote.data, r->symref);
            else
                rc = write_ref_file(gitdir, (char *)remote.data, r->oid);
            buf_free(&remote);
            if (rc != 0)
                return -1;
        } else if (fetch_tags && strncmp(r->name, "refs/tags/", 10) == 0) {
            if (r->symref)
                rc = write_symref_file(gitdir, r->name, r->symref);
            else
                rc = write_ref_file(gitdir, r->name, r->oid);
            if (rc != 0)
                return -1;
        }
    }

    if (refs->default_ref &&
        strncmp(refs->default_ref, "refs/heads/", 11) == 0) {
        Buf target;
        buf_init(&target);
        buf_appendf(&target, "refs/remotes/origin/%s", refs->default_ref + 11);
        int rc = write_symref_file(gitdir, "refs/remotes/origin/HEAD",
                                   (char *)target.data);
        buf_free(&target);
        if (rc != 0)
            return -1;
    }
    return 0;
}

/* True when rel (a refs/remotes/origin/... name) still has an advertised
 * branch behind it. HEAD and unrelated refs are always kept. */
static int remote_ref_is_expected(const RemoteRefs *refs, const char *rel)
{
    static const char prefix[] = "refs/remotes/origin/";
    if (strncmp(rel, prefix, sizeof(prefix) - 1) != 0)
        return 1;

    const char *branch = rel + sizeof(prefix) - 1;
    if (strcmp(branch, "HEAD") == 0)
        return 1;

    Buf full;
    buf_init(&full);
    buf_appendf(&full, "refs/heads/%s", branch);
    int found = refs_has_name(refs, (char *)full.data);
    buf_free(&full);
    return found;
}

static int prune_loose_refs(const char *gitdir, const char *rel,
                            const RemoteRefs *refs)
{
    char *dir = path_join(gitdir, rel);
    DIR *d = opendir(dir);
    if (!d) {
        int e = errno;
        free(dir);
        return e == ENOENT ? 0 : -1;
    }

    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        Buf child;
        buf_init(&child);
        buf_appendf(&child, "%s/%s", rel, de->d_name);

        char *full = path_join(gitdir, (char *)child.data);
        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (prune_loose_refs(gitdir, (char *)child.data, refs) != 0)
                    rc = -1;
                /* Remove the directory if pruning emptied it. */
                rmdir(full);
            } else if (S_ISREG(st.st_mode) &&
                       !remote_ref_is_expected(refs, (char *)child.data)) {
                if (unlink(full) != 0)
                    rc = -1;
            }
        }
        free(full);
        buf_free(&child);
    }

    closedir(d);
    rmdir(dir);
    free(dir);
    return rc;
}

/* Remove pruned refs from packed-refs, including any peeled line that follows
 * them. Returns 0 when the file is absent or nothing changed. */
static int prune_packed_refs(const char *gitdir, const RemoteRefs *refs)
{
    char *path = path_join(gitdir, "packed-refs");
    Buf in;
    buf_init(&in);
    if (read_file(path, &in) != 0) {
        int e = errno;
        buf_free(&in);
        free(path);
        return e == ENOENT ? 0 : -1;
    }

    Buf out;
    buf_init(&out);
    int changed = 0;
    int skip_peeled = 0;
    size_t pos = 0;
    while (pos < in.len) {
        size_t start = pos;
        while (pos < in.len && in.data[pos] != '\n')
            pos++;
        size_t len = pos - start;
        int has_nl = pos < in.len;
        if (has_nl)
            pos++;

        const char *line = (const char *)in.data + start;
        if (len > 0 && line[0] == '^') {
            if (skip_peeled) {
                changed = 1;
                skip_peeled = 0;
                continue;
            }
        } else {
            skip_peeled = 0;
            if (len > OID_HEX + 1 && line[OID_HEX] == ' ') {
                size_t name_len = len - (OID_HEX + 1);
                if (name_len > 0 && line[len - 1] == '\r')
                    name_len--;
                char *name = xstrndup(line + OID_HEX + 1, name_len);
                int drop = !remote_ref_is_expected(refs, name);
                free(name);
                if (drop) {
                    changed = 1;
                    skip_peeled = 1;
                    continue;
                }
            }
        }

        buf_append(&out, line, len);
        if (has_nl)
            buf_putc(&out, '\n');
    }

    int rc = 0;
    if (changed)
        rc = write_file_atomic(path, out.data, out.len, 0644);
    buf_free(&out);
    buf_free(&in);
    free(path);
    return rc;
}

int repo_prune_remote_refs(const char *gitdir, const RemoteRefs *refs)
{
    int rc = prune_loose_refs(gitdir, "refs/remotes/origin", refs);
    if (prune_packed_refs(gitdir, refs) != 0)
        rc = -1;
    return rc;
}

int repo_write_fetch_head(const char *gitdir, const RemoteRefs *refs,
                          const char *url, const char *merge_ref)
{
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < refs->count; i++) {
        const Ref *r = &refs->refs[i];
        if (strcmp(r->name, "HEAD") == 0)
            continue;

        char hex[OID_HEX + 1];
        oid_hex(r->oid, hex);

        const char *kind = NULL;
        const char *name = NULL;
        if (strncmp(r->name, "refs/heads/", 11) == 0) {
            kind = "branch";
            name = r->name + 11;
        } else if (strncmp(r->name, "refs/tags/", 10) == 0) {
            kind = "tag";
            name = r->name + 10;
        } else {
            continue;
        }

        int mergeable = merge_ref && strcmp(r->name, merge_ref) == 0;
        buf_appendf(&b, "%s\t%s\t%s '%s' of %s\n", hex,
                    mergeable ? "" : "not-for-merge", kind, name, url);
    }

    char *path = path_join(gitdir, "FETCH_HEAD");
    int rc = write_file_atomic(path, b.data, b.len, 0644);
    free(path);
    buf_free(&b);
    return rc;
}

int repo_write_orig_head(const char *gitdir,
                         const unsigned char oid[OID_RAW])
{
    char hex[OID_HEX + 1];
    oid_hex(oid, hex);
    char line[OID_HEX + 2];
    memcpy(line, hex, OID_HEX);
    line[OID_HEX] = '\n';
    line[OID_HEX + 1] = '\0';

    char *path = path_join(gitdir, "ORIG_HEAD");
    int rc = write_file_atomic(path, line, OID_HEX + 1, 0644);
    free(path);
    return rc;
}

int repo_read_origin_url(const char *gitdir, char **out_url)
{
    *out_url = NULL;
    char *path = path_join(gitdir, "config");
    Buf b;
    buf_init(&b);
    int rc = read_file(path, &b);
    free(path);
    if (rc != 0) {
        buf_free(&b);
        return 1;
    }

    int in_origin = 0;
    int found = 1;
    char *save = NULL;
    for (char *line = strtok_r((char *)b.data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(line);
        while (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';

        char *s = trim_ws(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            in_origin = strcmp(s, "[remote \"origin\"]") == 0;
            continue;
        }
        if (!in_origin)
            continue;

        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim_ws(s);
        char *value = trim_ws(eq + 1);
        if (strcmp(key, "url") == 0) {
            if (!text_is_safe(value) || !*value) {
                found = -1;
                break;
            }
            *out_url = xstrdup(value);
            found = 0;
            break;
        }
    }

    buf_free(&b);
    return found;
}

int repo_head(const char *gitdir, char **out_ref,
              unsigned char oid[OID_RAW], int *has_oid)
{
    *out_ref = NULL;
    *has_oid = 0;

    char *path = path_join(gitdir, "HEAD");
    Buf b;
    buf_init(&b);
    int rc = read_file(path, &b);
    free(path);
    if (rc != 0) {
        buf_free(&b);
        return -1;
    }

    char *s = trim_ws((char *)b.data);
    if (strncmp(s, "ref: ", 5) == 0) {
        char *target = trim_ws(s + 5);
        if (!ref_name_is_valid(target)) {
            rc = -1;
        } else {
            *out_ref = xstrdup(target);
            int r = repo_read_ref(gitdir, target, oid);
            if (r == 0)
                *has_oid = 1;
            else if (r < 0)
                rc = -1;
        }
    } else if (oid_parse(s, oid) == 0) {
        *has_oid = 1;
    } else {
        rc = -1;
    }
    buf_free(&b);

    if (rc != 0) {
        free(*out_ref);
        *out_ref = NULL;
        *has_oid = 0;
    }
    return rc;
}

typedef struct {
    unsigned char (*oids)[OID_RAW];
    size_t count;
    size_t cap;
} OidVec;

/* Keep the request bounded: a repository with an enormous number of refs does
 * not need all of them to negotiate a small fetch. */
#define CGC_MAX_HAVES 1024

static void oidvec_add(OidVec *v, const unsigned char oid[OID_RAW])
{
    if (v->count >= CGC_MAX_HAVES)
        return;
    for (size_t i = 0; i < v->count; i++)
        if (memcmp(v->oids[i], oid, OID_RAW) == 0)
            return;
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->oids = xrealloc(v->oids, v->cap * OID_RAW);
    }
    memcpy(v->oids[v->count++], oid, OID_RAW);
}

static int walk_refs(const char *gitdir, const char *rel, int depth,
                     OidVec *v)
{
    char *dirpath = path_join(gitdir, rel);
    DIR *d = opendir(dirpath);
    if (!d) {
        int e = errno;
        free(dirpath);
        return e == ENOENT ? 0 : -1;
    }

    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        Buf child;
        buf_init(&child);
        buf_appendf(&child, "%s/%s", rel, de->d_name);

        char *full = path_join(gitdir, (char *)child.data);
        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (depth < 16 &&
                    walk_refs(gitdir, (char *)child.data, depth + 1, v) != 0)
                    rc = -1;
            } else if (S_ISREG(st.st_mode)) {
                unsigned char oid[OID_RAW];
                if (repo_read_ref(gitdir, (char *)child.data, oid) == 0)
                    oidvec_add(v, oid);
            }
        }
        free(full);
        buf_free(&child);
        if (rc != 0)
            break;
    }

    closedir(d);
    free(dirpath);
    return rc;
}

int repo_collect_local_oids(const char *gitdir,
                            unsigned char (**oids)[OID_RAW], size_t *count)
{
    OidVec v;
    memset(&v, 0, sizeof(v));

    if (walk_refs(gitdir, "refs", 0, &v) != 0) {
        free(v.oids);
        return -1;
    }

    char *packed = path_join(gitdir, "packed-refs");
    Buf b;
    buf_init(&b);
    if (read_file(packed, &b) == 0) {
        char *save = NULL;
        for (char *line = strtok_r((char *)b.data, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == '#' || line[0] == '^' ||
                strlen(line) < OID_HEX)
                continue;
            char hex[OID_HEX + 1];
            memcpy(hex, line, OID_HEX);
            hex[OID_HEX] = '\0';
            unsigned char oid[OID_RAW];
            if (oid_parse(hex, oid) == 0)
                oidvec_add(&v, oid);
        }
    }
    free(packed);
    buf_free(&b);

    char *ref = NULL;
    unsigned char head_oid[OID_RAW];
    int has_head = 0;
    if (repo_head(gitdir, &ref, head_oid, &has_head) == 0 && has_head)
        oidvec_add(&v, head_oid);
    free(ref);

    *oids = v.oids;
    *count = v.count;
    return 0;
}
