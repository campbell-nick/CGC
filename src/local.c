#include "local.h"
#include "pack.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int is_gitdir(const char *path)
{
    char *head = path_join(path, "HEAD");
    char *objects = path_join(path, "objects");
    char *refs = path_join(path, "refs");
    char *commondir = path_join(path, "commondir");
    struct stat st;
    int has_objects = stat(objects, &st) == 0 && S_ISDIR(st.st_mode);
    int has_refs = stat(refs, &st) == 0 && S_ISDIR(st.st_mode);
    int has_common = stat(commondir, &st) == 0 && S_ISREG(st.st_mode);
    int ok = stat(head, &st) == 0 && S_ISREG(st.st_mode) &&
             ((has_objects && has_refs) || has_common);
    free(head);
    free(objects);
    free(refs);
    free(commondir);
    return ok;
}

/* A linked worktree keeps HEAD in its own gitdir but shares objects and refs
 * with the common gitdir named by "commondir". */
static char *common_gitdir(const char *gitdir)
{
    char *path = path_join(gitdir, "commondir");
    Buf b;
    buf_init(&b);
    if (read_file(path, &b) != 0) {
        buf_free(&b);
        free(path);
        return xstrdup(gitdir);
    }
    free(path);

    char *s = (char *)b.data;
    if (!s) {
        buf_free(&b);
        return xstrdup(gitdir);
    }
    char *nl = strchr(s, '\n');
    if (nl)
        *nl = '\0';
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == ' ' ||
                       s[len - 1] == '\t'))
        s[--len] = '\0';

    char *joined = s[0] == '/' ? xstrdup(s) : path_join(gitdir, s);
    char *abs = realpath(joined, NULL);
    free(joined);
    buf_free(&b);
    if (!abs)
        return xstrdup(gitdir);
    return abs;
}

/* Parse a ".git" file ("gitdir: <path>") used by linked worktrees. */
static int read_gitfile(const char *dotgit, const char *base, char **out)
{
    Buf b;
    buf_init(&b);
    if (read_file(dotgit, &b) != 0) {
        buf_free(&b);
        return -1;
    }

    char *s = (char *)b.data;
    while (*s == ' ' || *s == '\t')
        s++;
    if (strncmp(s, "gitdir:", 7) != 0) {
        buf_free(&b);
        return -1;
    }
    s += 7;
    while (*s == ' ' || *s == '\t')
        s++;

    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
                       end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = '\0';
    if (!*s) {
        buf_free(&b);
        return -1;
    }

    char *path = s[0] == '/' ? xstrdup(s) : path_join(base, s);
    buf_free(&b);

    char *abs = realpath(path, NULL);
    free(path);
    if (!abs)
        return -1;
    *out = abs;
    return 0;
}

int local_repo_gitdir(const char *path, char **out_gitdir)
{
    *out_gitdir = NULL;
    char *abs = realpath(path, NULL);
    if (!abs)
        return -1;

    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        free(abs);
        return -1;
    }

    if (is_gitdir(abs)) {
        *out_gitdir = abs;
        return 0;
    }

    char *dotgit = path_join(abs, ".git");
    int rc = -1;
    if (stat(dotgit, &st) == 0) {
        if (S_ISDIR(st.st_mode) && is_gitdir(dotgit)) {
            *out_gitdir = dotgit;
            dotgit = NULL;
            rc = 0;
        } else if (S_ISREG(st.st_mode)) {
            char *gitdir = NULL;
            if (read_gitfile(dotgit, abs, &gitdir) == 0 && is_gitdir(gitdir)) {
                *out_gitdir = gitdir;
                gitdir = NULL;
                rc = 0;
            }
            free(gitdir);
        }
    }
    free(dotgit);
    free(abs);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Ref listing                                                               */
/* ------------------------------------------------------------------------- */

static Ref *ref_lookup(RemoteRefs *refs, const char *name)
{
    for (size_t i = 0; i < refs->count; i++)
        if (strcmp(refs->refs[i].name, name) == 0)
            return &refs->refs[i];
    return NULL;
}

static Ref *ref_ensure(RemoteRefs *refs, const char *name)
{
    Ref *ref = ref_lookup(refs, name);
    if (ref)
        return ref;

    refs->refs = xrealloc(refs->refs, (refs->count + 1) * sizeof(Ref));
    ref = &refs->refs[refs->count++];
    memset(ref, 0, sizeof(*ref));
    ref->name = xstrdup(name);
    return ref;
}

static int parse_ref_content(Buf *b, Ref *ref)
{
    char *s = (char *)b->data;
    if (!s)
        return -1;

    char *nl = strchr(s, '\n');
    if (nl)
        *nl = '\0';
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == ' ' ||
                       s[len - 1] == '\t'))
        s[--len] = '\0';

    if (strncmp(s, "ref: ", 5) == 0) {
        const char *target = s + 5;
        while (*target == ' ' || *target == '\t')
            target++;
        if (!ref_name_is_valid(target))
            return -1;
        free(ref->symref);
        ref->symref = xstrdup(target);
        return 0;
    }

    return oid_parse(s, ref->oid) == 0 ? 0 : -1;
}

static int read_packed_refs(const char *gitdir, RemoteRefs *out)
{
    char *path = path_join(gitdir, "packed-refs");
    Buf b;
    buf_init(&b);
    if (read_file(path, &b) != 0) {
        buf_free(&b);
        free(path);
        return 0;
    }
    free(path);

    Ref *prev = NULL;
    char *save = NULL;
    for (char *line = strtok_r((char *)b.data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                           line[len - 1] == '\t'))
            line[--len] = '\0';
        if (!*line || line[0] == '#') {
            prev = NULL;
            continue;
        }
        if (line[0] == '^') {
            if (prev && oid_parse(line + 1, prev->peeled) == 0)
                prev->has_peeled = 1;
            continue;
        }

        if (len < OID_HEX + 2 || line[OID_HEX] != ' ')
            continue;

        char hex[OID_HEX + 1];
        memcpy(hex, line, OID_HEX);
        hex[OID_HEX] = '\0';
        unsigned char oid[OID_RAW];
        if (oid_parse(hex, oid) != 0)
            continue;

        const char *name = line + OID_HEX + 1;
        if ((strncmp(name, "refs/heads/", 11) != 0 &&
             strncmp(name, "refs/tags/", 10) != 0) ||
            !ref_name_is_valid(name)) {
            prev = NULL;
            continue;
        }

        Ref *ref = ref_ensure(out, name);
        memcpy(ref->oid, oid, OID_RAW);
        free(ref->symref);
        ref->symref = NULL;
        ref->has_peeled = 0;
        prev = ref;
    }

    buf_free(&b);
    return 0;
}

static int read_loose_refs(const char *gitdir, const char *rel,
                           RemoteRefs *out)
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
                if (read_loose_refs(gitdir, (char *)child.data, out) != 0)
                    rc = -1;
            } else if (S_ISREG(st.st_mode)) {
                size_t n = strlen(de->d_name);
                if (n < 5 || strcmp(de->d_name + n - 5, ".lock") != 0) {
                    if (!ref_name_is_valid((char *)child.data)) {
                        rc = -1;
                    } else {
                        Buf content;
                        buf_init(&content);
                        if (read_file(full, &content) != 0) {
                            rc = -1;
                        } else {
                            Ref *ref = ref_ensure(out, (char *)child.data);
                            free(ref->symref);
                            ref->symref = NULL;
                            if (parse_ref_content(&content, ref) != 0)
                                rc = -1;
                        }
                        buf_free(&content);
                    }
                }
            }
        }
        free(full);
        buf_free(&child);
        if (rc != 0)
            break;
    }

    closedir(d);
    free(dir);
    return rc;
}

static int read_head(const char *gitdir, RemoteRefs *out)
{
    char *path = path_join(gitdir, "HEAD");
    Buf b;
    buf_init(&b);
    if (read_file(path, &b) != 0) {
        buf_free(&b);
        free(path);
        return -1;
    }
    free(path);

    char *s = (char *)b.data;
    if (!s) {
        buf_free(&b);
        return -1;
    }
    char *nl = strchr(s, '\n');
    if (nl)
        *nl = '\0';
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == ' ' ||
                       s[len - 1] == '\t'))
        s[--len] = '\0';

    Ref *head = ref_ensure(out, "HEAD");
    int rc = 0;
    if (strncmp(s, "ref: ", 5) == 0) {
        const char *target = s + 5;
        while (*target == ' ' || *target == '\t')
            target++;
        if (!ref_name_is_valid(target)) {
            rc = -1;
        } else {
            head->symref = xstrdup(target);
        }
    } else if (oid_parse(s, head->oid) != 0) {
        rc = -1;
    } else {
        memcpy(out->default_oid, head->oid, OID_RAW);
        out->has_default_oid = 1;
    }

    buf_free(&b);
    return rc;
}

static int finish_refs(RemoteRefs *out)
{
    size_t non_head = 0;
    for (size_t i = 0; i < out->count; i++)
        if (strcmp(out->refs[i].name, "HEAD") != 0)
            non_head++;
    if (non_head == 0)
        out->empty = 1;

    Ref *head = ref_lookup(out, "HEAD");
    if (head && head->symref) {
        if (strncmp(head->symref, "refs/heads/", 11) == 0)
            out->default_ref = xstrdup(head->symref);

        Ref *target = ref_lookup(out, head->symref);
        if (target && !target->symref) {
            memcpy(head->oid, target->oid, OID_RAW);
            if (!out->has_default_oid) {
                memcpy(out->default_oid, target->oid, OID_RAW);
                out->has_default_oid = 1;
            }
        }
    }

    if (!out->default_ref) {
        const char *candidates[] = {"refs/heads/main", "refs/heads/master"};
        for (size_t c = 0; c < 2 && !out->default_ref; c++) {
            Ref *r = ref_lookup(out, candidates[c]);
            if (r && !r->symref)
                out->default_ref = xstrdup(candidates[c]);
        }
    }
    if (!out->default_ref) {
        for (size_t i = 0; i < out->count; i++) {
            if (strncmp(out->refs[i].name, "refs/heads/", 11) == 0 &&
                !out->refs[i].symref) {
                out->default_ref = xstrdup(out->refs[i].name);
                break;
            }
        }
    }

    if (out->default_ref && !out->has_default_oid) {
        Ref *target = ref_lookup(out, out->default_ref);
        if (target && !target->symref) {
            memcpy(out->default_oid, target->oid, OID_RAW);
            out->has_default_oid = 1;
        }
    }
    return 0;
}

int local_ls_refs(const char *path, RemoteRefs *out)
{
    memset(out, 0, sizeof(*out));

    char *gitdir = NULL;
    if (local_repo_gitdir(path, &gitdir) != 0) {
        warn_cgc("not a local git repository: %s", path);
        return -1;
    }
    char *common = common_gitdir(gitdir);

    int rc = read_packed_refs(common, out);
    if (rc == 0)
        rc = read_loose_refs(common, "refs/heads", out);
    if (rc == 0)
        rc = read_loose_refs(common, "refs/tags", out);
    if (rc == 0)
        rc = read_head(gitdir, out);
    if (rc == 0)
        rc = finish_refs(out);

    if (rc != 0) {
        warn_cgc("cannot read refs from local repository: %s", path);
        remote_refs_free(out);
    }
    free(common);
    free(gitdir);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Object copy                                                               */
/* ------------------------------------------------------------------------- */

static int is_hex_name(const char *s, size_t n)
{
    if (strlen(s) != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

static int copy_loose_objects(const char *src, const char *dst)
{
    char *srcdir = path_join(src, "objects");
    DIR *d = opendir(srcdir);
    if (!d) {
        int e = errno;
        free(srcdir);
        return e == ENOENT ? 0 : -1;
    }

    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && rc == 0) {
        if (!is_hex_name(de->d_name, 2))
            continue;

        char *subdir = path_join(srcdir, de->d_name);
        DIR *sd = opendir(subdir);
        if (!sd) {
            rc = -1;
            free(subdir);
            break;
        }

        struct dirent *fe;
        while ((fe = readdir(sd)) != NULL && rc == 0) {
            if (!is_hex_name(fe->d_name, OID_HEX - 2))
                continue;

            char *srcfile = path_join(subdir, fe->d_name);
            Buf rel;
            buf_init(&rel);
            buf_appendf(&rel, "objects/%s/%s", de->d_name, fe->d_name);
            char *dstfile = path_join(dst, (char *)rel.data);
            buf_free(&rel);

            struct stat st;
            if (stat(dstfile, &st) == 0) {
                free(srcfile);
                free(dstfile);
                continue;
            }

            Buf content;
            buf_init(&content);
            if (read_file(srcfile, &content) != 0) {
                rc = -1;
            } else {
                char *dir = path_dirname(dstfile);
                if (mkdir_p(dir, 0777) != 0 ||
                    write_file(dstfile, content.data, content.len, 0444) != 0)
                    rc = -1;
                free(dir);
            }
            buf_free(&content);
            free(srcfile);
            free(dstfile);
        }
        closedir(sd);
        free(subdir);
    }

    closedir(d);
    free(srcdir);
    return rc;
}

static int copy_packs(const char *src, const char *dst)
{
    char *packdir = path_join(src, "objects/pack");
    DIR *d = opendir(packdir);
    if (!d) {
        int e = errno;
        free(packdir);
        return e == ENOENT ? 0 : -1;
    }

    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && rc == 0) {
        size_t n = strlen(de->d_name);
        if (n < 5 || strcmp(de->d_name + n - 5, ".pack") != 0)
            continue;

        char *path = path_join(packdir, de->d_name);
        Buf pack;
        buf_init(&pack);
        if (read_file(path, &pack) != 0) {
            rc = -1;
        } else if (pack_unpack(pack.data, pack.len, dst, NULL, NULL, NULL) != 0) {
            warn_cgc("cannot unpack local packfile: %s", path);
            rc = -1;
        }
        buf_free(&pack);
        free(path);
    }

    closedir(d);
    free(packdir);
    return rc;
}

static int copy_shallow(const char *src, const char *dst)
{
    char *srcpath = path_join(src, "shallow");
    Buf b;
    buf_init(&b);
    if (read_file(srcpath, &b) != 0) {
        buf_free(&b);
        free(srcpath);
        return 0;
    }
    free(srcpath);

    char *dstpath = path_join(dst, "shallow");
    int rc = write_file_atomic(dstpath, b.data, b.len, 0644);
    free(dstpath);
    buf_free(&b);
    return rc;
}

int local_fetch(const char *path, const char *gitdir, int depth)
{
    if (depth > 0) {
        warn_cgc("shallow clones from local repositories are not supported yet");
        return -1;
    }

    char *src = NULL;
    if (local_repo_gitdir(path, &src) != 0) {
        warn_cgc("not a local git repository: %s", path);
        return -1;
    }
    char *common = common_gitdir(src);

    int rc = copy_loose_objects(common, gitdir);
    if (rc == 0)
        rc = copy_packs(common, gitdir);
    if (rc == 0)
        rc = copy_shallow(common, gitdir);

    free(common);
    free(src);
    return rc;
}
