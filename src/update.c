#include "update.h"
#include "checkout.h"
#include "odb.h"
#include "protocol.h"
#include "repo.h"
#include "util.h"

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

/* Bound the commit walk used for fast-forward detection. If a history is
 * larger than this we refuse rather than risk an unbounded traversal. */
#define CGC_MAX_COMMIT_WALK 200000

typedef struct {
    unsigned char (*oids)[OID_RAW];
    size_t count;
    size_t cap;
} OidVec;

static void oidvec_push(OidVec *v, const unsigned char oid[OID_RAW])
{
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->oids = xrealloc(v->oids, v->cap * OID_RAW);
    }
    memcpy(v->oids[v->count++], oid, OID_RAW);
}

static int oidvec_contains(const OidVec *v, const unsigned char oid[OID_RAW])
{
    for (size_t i = 0; i < v->count; i++)
        if (memcmp(v->oids[i], oid, OID_RAW) == 0)
            return 1;
    return 0;
}

static void oidvec_free(OidVec *v)
{
    free(v->oids);
    memset(v, 0, sizeof(*v));
}

static char *absolute_path(const char *path)
{
    char cwd[PATH_MAX];
    if (path && path[0] == '/')
        return xstrdup(path);
    if (!getcwd(cwd, sizeof(cwd)))
        return NULL;
    if (!path || !*path || strcmp(path, ".") == 0)
        return xstrdup(cwd);
    return path_join(cwd, path);
}

/* Parse a ".git" file ("gitdir: <path>") used by linked worktrees. */
static int read_gitfile(const char *dotgit, const char *dir, char **out)
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

    *out = s[0] == '/' ? xstrdup(s) : path_join(dir, s);
    buf_free(&b);
    return 0;
}

/* Find the repository containing start, walking up parent directories.
 * Returns 0 when found, 1 when there is no repository, -1 on error. */
static int find_repo(const char *start, char **out_gitdir, char **out_worktree)
{
    char *cur = absolute_path(start);
    if (!cur)
        return -1;

    int result = 1;
    for (;;) {
        char *dotgit = path_join(cur, ".git");
        struct stat st;
        if (stat(dotgit, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                *out_gitdir = dotgit;
                *out_worktree = xstrdup(cur);
                result = 0;
                break;
            }
            if (S_ISREG(st.st_mode)) {
                char *gitdir = NULL;
                if (read_gitfile(dotgit, cur, &gitdir) == 0) {
                    *out_gitdir = gitdir;
                    *out_worktree = xstrdup(cur);
                    free(dotgit);
                    result = 0;
                    break;
                }
                free(dotgit);
                result = -1;
                break;
            }
        }
        free(dotgit);

        char *parent = path_dirname(cur);
        if (strcmp(parent, cur) == 0) {
            free(parent);
            break;
        }
        free(cur);
        cur = parent;
    }

    free(cur);
    return result;
}

static int is_shallow(const char *gitdir)
{
    char *path = path_join(gitdir, "shallow");
    struct stat st;
    int rc = lstat(path, &st) == 0;
    free(path);
    return rc;
}

static int push_commit_parents(const char *gitdir,
                               const unsigned char oid[OID_RAW], OidVec *stack)
{
    int type = 0;
    Buf b;
    buf_init(&b);
    int r = odb_read(gitdir, oid, &type, &b);
    if (r <= 0 || type != OBJ_COMMIT) {
        buf_free(&b);
        return -1;
    }

    size_t pos = 0;
    while (pos < b.len) {
        size_t eol = pos;
        while (eol < b.len && b.data[eol] != '\n')
            eol++;
        size_t len = eol - pos;

        if (len == 0)
            break; /* blank line ends the commit header */
        if (len > 7 && memcmp(b.data + pos, "parent ", 7) == 0) {
            if (len != 7 + OID_HEX) {
                buf_free(&b);
                return -1;
            }
            char hex[OID_HEX + 1];
            memcpy(hex, b.data + pos + 7, OID_HEX);
            hex[OID_HEX] = '\0';
            unsigned char parent[OID_RAW];
            if (oid_parse(hex, parent) != 0) {
                buf_free(&b);
                return -1;
            }
            oidvec_push(stack, parent);
        }
        pos = eol < b.len ? eol + 1 : b.len;
    }

    buf_free(&b);
    return 0;
}

/* 1 if ancestor is reachable from descendant, 0 if not, -1 on error. */
static int is_ancestor(const char *gitdir, const unsigned char ancestor[OID_RAW],
                       const unsigned char descendant[OID_RAW])
{
    if (memcmp(ancestor, descendant, OID_RAW) == 0)
        return 1;

    OidVec stack;
    OidVec seen;
    memset(&stack, 0, sizeof(stack));
    memset(&seen, 0, sizeof(seen));
    oidvec_push(&stack, descendant);

    int result = 0;
    while (stack.count > 0) {
        unsigned char oid[OID_RAW];
        memcpy(oid, stack.oids[--stack.count], OID_RAW);

        if (oidvec_contains(&seen, oid))
            continue;
        if (seen.count >= CGC_MAX_COMMIT_WALK) {
            result = -1;
            break;
        }
        oidvec_push(&seen, oid);

        if (memcmp(oid, ancestor, OID_RAW) == 0) {
            result = 1;
            break;
        }
        if (push_commit_parents(gitdir, oid, &stack) != 0) {
            result = -1;
            break;
        }
    }

    oidvec_free(&stack);
    oidvec_free(&seen);
    return result;
}

static char *branch_ref(const char *name)
{
    if (strncmp(name, "refs/heads/", 11) == 0)
        return xstrdup(name);
    Buf b;
    buf_init(&b);
    buf_appendf(&b, "refs/heads/%s", name);
    return (char *)b.data;
}

static const Ref *find_ref(const RemoteRefs *refs, const char *name)
{
    for (size_t i = 0; i < refs->count; i++)
        if (strcmp(refs->refs[i].name, name) == 0)
            return &refs->refs[i];
    return NULL;
}

static void short_oid(const unsigned char oid[OID_RAW], char out[8])
{
    char hex[OID_HEX + 1];
    oid_hex(oid, hex);
    memcpy(out, hex, 7);
    out[7] = '\0';
}

/* Print a Git-style fetch summary. Must run before remote-tracking refs are
 * overwritten so old values can be compared. */
static void report_fetch(const char *gitdir, const RemoteRefs *refs,
                         const char *url)
{
    if (cgc_quiet)
        return;

    int any = 0;
    for (size_t i = 0; i < refs->count && !any; i++) {
        const Ref *r = &refs->refs[i];
        unsigned char old[OID_RAW];

        if (strncmp(r->name, "refs/heads/", 11) == 0) {
            Buf remote;
            buf_init(&remote);
            buf_appendf(&remote, "refs/remotes/origin/%s", r->name + 11);
            int found = repo_read_ref(gitdir, (char *)remote.data, old) == 0;
            if (!found || memcmp(old, r->oid, OID_RAW) != 0)
                any = 1;
            buf_free(&remote);
        } else if (strncmp(r->name, "refs/tags/", 10) == 0) {
            int found = repo_read_ref(gitdir, r->name, old) == 0;
            if (!found || memcmp(old, r->oid, OID_RAW) != 0)
                any = 1;
        }
    }

    if (!any)
        return;

    fprintf(stderr, "From %s\n", url);
    for (size_t i = 0; i < refs->count; i++) {
        const Ref *r = &refs->refs[i];
        unsigned char old[OID_RAW];

        if (strncmp(r->name, "refs/heads/", 11) == 0) {
            Buf remote;
            buf_init(&remote);
            buf_appendf(&remote, "refs/remotes/origin/%s", r->name + 11);
            int found = repo_read_ref(gitdir, (char *)remote.data, old) == 0;
            if (!found) {
                fprintf(stderr, " * [new branch]      %s -> origin/%s\n",
                        r->name + 11, r->name + 11);
            } else if (memcmp(old, r->oid, OID_RAW) != 0) {
                char a[8], b[8];
                short_oid(old, a);
                short_oid(r->oid, b);
                fprintf(stderr, "   %s..%s  %s -> origin/%s\n",
                        a, b, r->name + 11, r->name + 11);
            }
            buf_free(&remote);
        } else if (strncmp(r->name, "refs/tags/", 10) == 0) {
            int found = repo_read_ref(gitdir, r->name, old) == 0;
            if (!found) {
                fprintf(stderr, " * [new tag]         %s -> %s\n",
                        r->name + 10, r->name + 10);
            } else if (memcmp(old, r->oid, OID_RAW) != 0) {
                char a[8], b[8];
                short_oid(old, a);
                short_oid(r->oid, b);
                fprintf(stderr, "   %s..%s  %s -> %s\n",
                        a, b, r->name + 10, r->name + 10);
            }
        }
    }
    fflush(stderr);
}

int update_run(const char *start_dir, const char *branch_opt, int pull,
               const UpdateOptions *opts)
{
    char *gitdir = NULL;
    char *worktree = NULL;
    int found = find_repo(start_dir, &gitdir, &worktree);
    if (found != 0) {
        if (found < 0)
            warn_cgc("cannot inspect '%s': %s", start_dir, strerror(errno));
        else
            warn_cgc("not a repository: '%s'", start_dir);
        free(gitdir);
        free(worktree);
        return -1;
    }

    int rc = -1;
    char *url = NULL;
    char *head_ref = NULL;
    char *target_ref = NULL;
    unsigned char (*haves)[OID_RAW] = NULL;
    size_t nhaves = 0;
    RemoteRefs refs;
    memset(&refs, 0, sizeof(refs));
    RemoteRefs filtered;
    memset(&filtered, 0, sizeof(filtered));
    int have_filter = 0;

    if (is_shallow(gitdir)) {
        warn_cgc("fetching into a shallow clone is not supported yet");
        goto done;
    }

    if (repo_read_origin_url(gitdir, &url) != 0 || !url) {
        warn_cgc("no origin remote configured");
        goto done;
    }

    unsigned char head_oid[OID_RAW];
    int has_head = 0;
    if (repo_head(gitdir, &head_ref, head_oid, &has_head) != 0) {
        warn_cgc("cannot read HEAD");
        goto done;
    }

    if (branch_opt) {
        target_ref = branch_ref(branch_opt);
        if (!ref_name_is_valid(target_ref)) {
            warn_cgc("invalid branch name '%s'", branch_opt);
            goto done;
        }
    } else if (head_ref && strncmp(head_ref, "refs/heads/", 11) == 0) {
        target_ref = xstrdup(head_ref);
    }

    if (pull && !target_ref) {
        warn_cgc("cannot pull with a detached HEAD; use --branch <name>");
        goto done;
    }
    if (pull && branch_opt && head_ref && has_head &&
        strcmp(head_ref, target_ref) != 0) {
        warn_cgc("refusing to switch branches; check out '%s' first",
                 branch_opt);
        goto done;
    }

    if (protocol_ls_refs(url, &refs) != 0) {
        warn_cgc("could not list remote refs");
        goto done;
    }

    if (refs.empty) {
        if (pull) {
            warn_cgc("remote repository is empty");
            goto done;
        }
        if (opts && opts->prune && !branch_opt &&
            repo_prune_remote_refs(gitdir, &refs) != 0)
            warn_cgc("could not prune stale remote-tracking refs");
        if (!cgc_quiet)
            fprintf(stderr, "From %s\n", url);
        rc = 0;
        goto done;
    }

    if (branch_opt && target_ref && !find_ref(&refs, target_ref)) {
        warn_cgc("remote branch '%s' not found", branch_opt);
        goto done;
    }

    /* With --branch, fetch only that branch (plus tags unless --no-tags).
     * --no-tags on its own removes tags from the request. The Refs are
     * shallow copies owned by `refs`. */
    const RemoteRefs *active = &refs;
    int no_tags = opts && opts->no_tags;
    if (branch_opt || no_tags) {
        filtered.refs = xcalloc(refs.count, sizeof(Ref));
        for (size_t i = 0; i < refs.count; i++) {
            const Ref *r = &refs.refs[i];
            int is_tag = strncmp(r->name, "refs/tags/", 10) == 0;
            if (branch_opt && target_ref && strcmp(r->name, target_ref) != 0 &&
                !is_tag)
                continue;
            if (no_tags && is_tag)
                continue;
            filtered.refs[filtered.count++] = *r;
        }
        have_filter = 1;
        active = &filtered;
    }

    if (repo_collect_local_oids(gitdir, &haves, &nhaves) != 0) {
        warn_cgc("cannot read local refs");
        goto done;
    }

    if (protocol_fetch(url, active, gitdir, 0,
                       (const unsigned char (*)[OID_RAW])haves, nhaves) != 0) {
        warn_cgc("fetch failed");
        goto done;
    }

    report_fetch(gitdir, active, url);

    RefWriteOptions ref_opts;
    memset(&ref_opts, 0, sizeof(ref_opts));
    ref_opts.force_tags = opts && opts->force_tags;
    ref_opts.no_tags = no_tags;
    if (repo_write_remote_refs(gitdir, active, &ref_opts) != 0) {
        warn_cgc("cannot update remote refs");
        goto done;
    }
    if (repo_write_fetch_head(gitdir, active, url,
                              pull ? target_ref : NULL) != 0)
        warn_cgc("cannot write FETCH_HEAD");

    if (opts && opts->prune && !branch_opt) {
        if (repo_prune_remote_refs(gitdir, active) != 0)
            warn_cgc("could not prune stale remote-tracking refs");
    }

    if (!pull) {
        rc = 0;
        goto done;
    }

    const Ref *remote_branch = find_ref(active, target_ref);
    if (!remote_branch) {
        warn_cgc("remote branch '%s' not found", target_ref);
        goto done;
    }

    unsigned char new_oid[OID_RAW];
    memcpy(new_oid, remote_branch->oid, OID_RAW);

    unsigned char local_oid[OID_RAW];
    int have_local = repo_read_ref(gitdir, target_ref, local_oid) == 0;
    int detached = (head_ref == NULL);

    if (detached) {
        /* Switching from a detached HEAD to a branch. Check out the branch tip
         * without discarding local commits: fast-forward it when possible, use
         * the local tip when it is ahead, and refuse when they have diverged. */
        unsigned char target_oid[OID_RAW];
        memcpy(target_oid, new_oid, OID_RAW);
        int advance = 1;

        if (have_local) {
            int ff = is_ancestor(gitdir, local_oid, new_oid);
            if (ff < 0) {
                warn_cgc("cannot verify that the update is a fast-forward");
                goto done;
            }
            if (ff) {
                memcpy(target_oid, new_oid, OID_RAW);
                advance = memcmp(local_oid, new_oid, OID_RAW) != 0;
            } else {
                int ahead = is_ancestor(gitdir, new_oid, local_oid);
                if (ahead != 1) {
                    warn_cgc("not a fast-forward; refusing to pull "
                             "(merge and rebase are not supported)");
                    goto done;
                }
                memcpy(target_oid, local_oid, OID_RAW);
                advance = 0;
            }
        }

        int moving = memcmp(head_oid, target_oid, OID_RAW) != 0;
        if (moving && repo_write_orig_head(gitdir, head_oid) != 0)
            warn_cgc("cannot write ORIG_HEAD");
        if (moving &&
            checkout_update(gitdir, worktree, head_oid, target_oid) != 0) {
            warn_cgc("could not update the working tree");
            goto done;
        }
        if (advance && repo_write_ref(gitdir, target_ref, new_oid) != 0) {
            warn_cgc("cannot update '%s'", target_ref);
            goto done;
        }
        if (repo_set_head(gitdir, target_ref) != 0) {
            warn_cgc("cannot update HEAD");
            goto done;
        }

        if (!cgc_quiet) {
            const char *name = strncmp(target_ref, "refs/heads/", 11) == 0
                                   ? target_ref + 11
                                   : target_ref;
            fprintf(stderr, "Switched to branch '%s'\n", name);
            fflush(stderr);
        }
        rc = 0;
        goto done;
    }

    if (have_local && memcmp(local_oid, new_oid, OID_RAW) == 0) {
        if (!cgc_quiet)
            fprintf(stderr, "Already up to date.\n");
        rc = 0;
        goto done;
    }

    if (have_local) {
        int ff = is_ancestor(gitdir, local_oid, new_oid);
        if (ff < 0) {
            warn_cgc("cannot verify that the update is a fast-forward");
            goto done;
        }
        if (!ff) {
            int ahead = is_ancestor(gitdir, new_oid, local_oid);
            if (ahead == 1) {
                if (!cgc_quiet)
                    fprintf(stderr, "Already up to date.\n");
                rc = 0;
                goto done;
            }
            warn_cgc("not a fast-forward; refusing to pull "
                     "(merge and rebase are not supported)");
            goto done;
        }
    }

    if (have_local) {
        if (repo_write_orig_head(gitdir, local_oid) != 0)
            warn_cgc("cannot write ORIG_HEAD");
        if (checkout_update(gitdir, worktree, local_oid, new_oid) != 0) {
            warn_cgc("could not update the working tree");
            goto done;
        }
    } else {
        if (checkout_commit(gitdir, worktree, new_oid) != 0) {
            warn_cgc("could not check out the branch");
            goto done;
        }
    }

    if (repo_write_ref(gitdir, target_ref, new_oid) != 0) {
        warn_cgc("cannot update '%s'", target_ref);
        goto done;
    }
    if (!head_ref || strcmp(head_ref, target_ref) != 0) {
        if (repo_set_head(gitdir, target_ref) != 0) {
            warn_cgc("cannot update HEAD");
            goto done;
        }
    }

    if (!cgc_quiet) {
        char b[8];
        if (have_local) {
            char a[8];
            short_oid(local_oid, a);
            short_oid(new_oid, b);
            fprintf(stderr, "Updating %s..%s\nFast-forward\n", a, b);
        } else {
            short_oid(new_oid, b);
            fprintf(stderr, "Checking out %s\n", b);
        }
        fflush(stderr);
    }

    rc = 0;

done:
    if (have_filter)
        free(filtered.refs);
    free(url);
    free(head_ref);
    free(target_ref);
    free(haves);
    remote_refs_free(&refs);
    free(gitdir);
    free(worktree);
    return rc;
}
