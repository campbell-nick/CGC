#include "update.h"
#include "checkout.h"
#include "local.h"
#include "odb.h"
#include "protocol.h"
#include "repo.h"
#include "url.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* Detect a bare repository: a gitdir with HEAD, objects, refs, and a config
 * that sets bare = true. */
static int is_bare_gitdir(const char *dir)
{
    char *head = path_join(dir, "HEAD");
    char *objects = path_join(dir, "objects");
    char *refs = path_join(dir, "refs");
    char *config = path_join(dir, "config");
    struct stat st;
    int ok = stat(head, &st) == 0 && S_ISREG(st.st_mode) &&
             stat(objects, &st) == 0 && S_ISDIR(st.st_mode) &&
             stat(refs, &st) == 0 && S_ISDIR(st.st_mode) &&
             stat(config, &st) == 0 && S_ISREG(st.st_mode);
    free(head);
    free(objects);
    free(refs);

    if (!ok) {
        free(config);
        return 0;
    }

    Buf b;
    buf_init(&b);
    if (read_file(config, &b) != 0) {
        buf_free(&b);
        free(config);
        return 0;
    }
    free(config);

    int bare = 0;
    char *save = NULL;
    for (char *line = strtok_r((char *)b.data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (strncasecmp(line, "bare", 4) != 0)
            continue;
        char *p = line + 4;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '=')
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncasecmp(p, "true", 4) == 0 &&
            (p[4] == '\0' || p[4] == '\r' || p[4] == ' ' || p[4] == '\t'))
            bare = 1;
    }
    buf_free(&b);
    return bare;
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
        if (is_bare_gitdir(cur)) {
            *out_gitdir = xstrdup(cur);
            *out_worktree = NULL;
            result = 0;
            break;
        }

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

static void display_ref(const char *ref, const char *remote, Buf *out)
{
    Buf prefix;
    buf_init(&prefix);
    if (remote) {
        buf_appendf(&prefix, "refs/remotes/%s/", remote);
        if (strncmp(ref, (char *)prefix.data, prefix.len) == 0) {
            buf_append(out, ref + prefix.len, strlen(ref + prefix.len));
            buf_free(&prefix);
            return;
        }
    }
    buf_free(&prefix);

    if (strncmp(ref, "refs/heads/", 11) == 0)
        buf_append(out, ref + 11, strlen(ref + 11));
    else if (strncmp(ref, "refs/tags/", 10) == 0)
        buf_append(out, ref + 10, strlen(ref + 10));
    else
        buf_append(out, ref, strlen(ref));
}

/* Print a Git-style fetch summary. Must run before the refs are overwritten so
 * old values can be compared. */
static void report_fetch(const char *gitdir, const RefMapping *maps,
                         size_t nmaps, const char *remote, const char *url)
{
    if (cgc_quiet)
        return;

    int any = 0;
    for (size_t i = 0; i < nmaps; i++) {
        const RefMapping *m = &maps[i];
        if (!m->dst)
            continue;
        unsigned char old[OID_RAW];
        int found = repo_read_ref(gitdir, m->dst, old) == 0;
        if (!found || memcmp(old, m->oid, OID_RAW) != 0) {
            any = 1;
            break;
        }
    }
    if (!any)
        return;

    fprintf(stderr, "From %s\n", url);
    for (size_t i = 0; i < nmaps; i++) {
        const RefMapping *m = &maps[i];
        if (!m->dst)
            continue;

        unsigned char old[OID_RAW];
        int found = repo_read_ref(gitdir, m->dst, old) == 0;
        if (found && memcmp(old, m->oid, OID_RAW) == 0)
            continue;

        Buf src, dst;
        buf_init(&src);
        buf_init(&dst);
        display_ref(m->src, NULL, &src);
        display_ref(m->dst, remote, &dst);

        if (!found) {
            const char *kind = strncmp(m->src, "refs/tags/", 10) == 0
                                   ? "tag"
                                   : (strncmp(m->src, "refs/heads/", 11) == 0
                                          ? "branch"
                                          : "ref");
            fprintf(stderr, " * [new %s] %s -> %s\n", kind,
                    (char *)src.data, (char *)dst.data);
        } else {
            char a[8], b[8];
            short_oid(old, a);
            short_oid(m->oid, b);
            fprintf(stderr, "   %s..%s  %s -> %s\n", a, b,
                    (char *)src.data, (char *)dst.data);
        }
        buf_free(&src);
        buf_free(&dst);
    }
    fflush(stderr);
}

static const char *mapping_dst_for_src(const RefMapping *maps, size_t count,
                                       const char *src)
{
    for (size_t i = 0; i < count; i++)
        if (maps[i].dst && strcmp(maps[i].src, src) == 0)
            return maps[i].dst;
    return NULL;
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
    char *remote = NULL;
    RemoteConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    RefspecSet spec;
    refspec_set_init(&spec);
    char *url = NULL;
    char *head_ref = NULL;
    char *target_ref = NULL;
    char *merge_alloc = NULL;
    const char *merge_ref = NULL;
    unsigned char (*haves)[OID_RAW] = NULL;
    size_t nhaves = 0;
    RemoteRefs refs;
    memset(&refs, 0, sizeof(refs));
    RemoteRefs filtered;
    memset(&filtered, 0, sizeof(filtered));
    int have_filter = 0;
    RefMapping *maps = NULL;
    size_t nmaps = 0;

    if (is_shallow(gitdir)) {
        warn_cgc("fetching into a shallow clone is not supported yet");
        goto done;
    }

    if (pull && !worktree) {
        warn_cgc("cannot pull in a bare repository");
        goto done;
    }

    unsigned char head_oid[OID_RAW];
    int has_head = 0;
    if (repo_head(gitdir, &head_ref, head_oid, &has_head) != 0) {
        warn_cgc("cannot read HEAD");
        goto done;
    }

    /* Resolve the remote: --remote wins, then the current branch's configured
     * remote, then origin, then the first configured remote. */
    if (opts && opts->remote) {
        if (!repo_remote_name_is_valid(opts->remote)) {
            warn_cgc("invalid remote name '%s'", opts->remote);
            goto done;
        }
        remote = xstrdup(opts->remote);
    } else {
        int fr = repo_find_remote(gitdir, head_ref, &remote);
        if (fr < 0) {
            warn_cgc("cannot read remote configuration");
            goto done;
        }
        if (fr > 0 || !remote) {
            warn_cgc("no remote configured");
            goto done;
        }
    }

    int cr = repo_read_remote_config(gitdir, remote, &cfg);
    if (cr < 0) {
        warn_cgc("cannot read configuration for remote '%s'", remote);
        goto done;
    }
    if (cr > 0) {
        warn_cgc("remote '%s' is not configured", remote);
        goto done;
    }

    url = xstrdup(cfg.url);
    int local = url_is_local(url);
    int mirror = cfg.mirror;

    if (branch_opt) {
        target_ref = branch_ref(branch_opt);
        if (!ref_name_is_valid(target_ref)) {
            warn_cgc("invalid branch name '%s'", branch_opt);
            goto done;
        }
        merge_ref = target_ref;
    } else if (head_ref && strncmp(head_ref, "refs/heads/", 11) == 0) {
        target_ref = xstrdup(head_ref);
        char *configured = NULL;
        int br = repo_read_branch_merge(gitdir, target_ref, &configured);
        if (br < 0) {
            warn_cgc("cannot read branch configuration for '%s'",
                     target_ref + 11);
            goto done;
        }
        if (br == 0 && configured) {
            merge_alloc = configured;
            merge_ref = configured;
        } else {
            merge_ref = target_ref;
        }
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

    /* Build the fetch refspec set. A mirror copies refs verbatim and prunes.
     * A normal repository uses its configured fetch lines, or the standard
     * remote-tracking refspec when none are configured. A bare repository
     * without a refspec records FETCH_HEAD only. */
    if (mirror) {
        if (refspec_set_add(&spec, "+refs/*:refs/*") != 0) {
            warn_cgc("cannot build mirror refspec");
            goto done;
        }
    } else {
        for (size_t i = 0; i < cfg.nfetch; i++) {
            if (refspec_set_add(&spec, cfg.fetch[i]) != 0) {
                warn_cgc("invalid fetch refspec '%s'", cfg.fetch[i]);
                goto done;
            }
        }
        if (cfg.nfetch == 0 && worktree) {
            Buf b;
            buf_init(&b);
            buf_appendf(&b, "+refs/heads/*:refs/remotes/%s/*", remote);
            int add = refspec_set_add(&spec, (char *)b.data);
            buf_free(&b);
            if (add != 0) {
                warn_cgc("cannot build fetch refspec");
                goto done;
            }
        }
    }

    /* Mirror fetches need every ref. A custom refspec may also target
     * namespaces outside refs/heads and refs/tags (for example refs/notes),
     * so ask the remote for all refs in that case too. */
    int all_refs = mirror;
    for (size_t i = 0; !all_refs && i < spec.count; i++) {
        const char *src = spec.items[i].src;
        if (strncmp(src, "refs/heads/", 11) != 0 &&
            strncmp(src, "refs/tags/", 10) != 0)
            all_refs = 1;
    }
    int ls_rc = local ? local_ls_refs(url, all_refs, &refs)
                      : protocol_ls_refs(url, all_refs, &refs);
    if (ls_rc != 0) {
        warn_cgc("could not list remote refs");
        goto done;
    }

    int no_tags = (opts && opts->no_tags) && !mirror;
    int do_prune = ((opts && opts->prune) || mirror) && !branch_opt;

    if (refs.empty) {
        if (pull) {
            warn_cgc("remote repository is empty");
            goto done;
        }
        if (do_prune && repo_prune_mappings(gitdir, &spec, NULL, 0) != 0)
            warn_cgc("could not prune stale refs");
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
     * --no-tags on its own removes tags from the request. Otherwise the
     * configured refspecs decide what is fetched. The Refs are shallow copies
     * owned by `refs`. */
    const RemoteRefs *active = &refs;
    if (branch_opt || no_tags || spec.count > 0) {
        /* Keep the protocol/capability fields; only the ref list is filtered. */
        filtered = refs;
        filtered.refs = xcalloc(refs.count, sizeof(Ref));
        filtered.count = 0;
        for (size_t i = 0; i < refs.count; i++) {
            const Ref *r = &refs.refs[i];
            int is_tag = strncmp(r->name, "refs/tags/", 10) == 0;
            if (branch_opt && target_ref && strcmp(r->name, target_ref) != 0 &&
                !is_tag)
                continue;
            if (no_tags && is_tag)
                continue;
            if (!repo_refspec_selects(&spec, r->name, !no_tags))
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

    int fetch_rc = local ? local_fetch(url, gitdir, 0)
                         : protocol_fetch(
                               url, active, gitdir, 0,
                               (const unsigned char (*)[OID_RAW])haves,
                               nhaves);
    if (fetch_rc != 0) {
        warn_cgc("fetch failed");
        goto done;
    }

    const char *pull_source = merge_ref ? merge_ref : target_ref;

    if (repo_write_fetch_head(gitdir, active, url,
                              pull ? pull_source : NULL) != 0)
        warn_cgc("cannot write FETCH_HEAD");

    /* Work out the pull update before touching refs so a refused
     * fast-forward cannot clobber a local branch through a custom refspec. */
    unsigned char new_oid[OID_RAW];
    unsigned char local_oid[OID_RAW];
    unsigned char switch_oid[OID_RAW];
    int have_local = 0;
    int detached = (head_ref == NULL);
    int up_to_date = 0;
    int advance = 1;

    if (pull) {
        const Ref *remote_branch = find_ref(active, pull_source);
        if (!remote_branch) {
            warn_cgc("remote branch '%s' not found",
                     pull_source ? pull_source : "(unknown)");
            goto done;
        }
        memcpy(new_oid, remote_branch->oid, OID_RAW);

        int rr = repo_read_ref(gitdir, target_ref, local_oid);
        if (rr < 0) {
            warn_cgc("cannot read '%s'", target_ref);
            goto done;
        }
        have_local = rr == 0;

        if (detached) {
            memcpy(switch_oid, new_oid, OID_RAW);
            if (have_local) {
                int ff = is_ancestor(gitdir, local_oid, new_oid);
                if (ff < 0) {
                    warn_cgc("cannot verify that the update is a fast-forward");
                    goto done;
                }
                if (ff) {
                    memcpy(switch_oid, new_oid, OID_RAW);
                    advance = memcmp(local_oid, new_oid, OID_RAW) != 0;
                } else {
                    int ahead = is_ancestor(gitdir, new_oid, local_oid);
                    if (ahead != 1) {
                        warn_cgc("not a fast-forward; refusing to pull "
                                 "(merge and rebase are not supported)");
                        goto done;
                    }
                    memcpy(switch_oid, local_oid, OID_RAW);
                    advance = 0;
                }
            }
        } else if (have_local) {
            if (memcmp(local_oid, new_oid, OID_RAW) == 0) {
                up_to_date = 1;
            } else {
                int ff = is_ancestor(gitdir, local_oid, new_oid);
                if (ff < 0) {
                    warn_cgc("cannot verify that the update is a fast-forward");
                    goto done;
                }
                if (!ff) {
                    int ahead = is_ancestor(gitdir, new_oid, local_oid);
                    if (ahead != 1) {
                        warn_cgc("not a fast-forward; refusing to pull "
                                 "(merge and rebase are not supported)");
                        goto done;
                    }
                    up_to_date = 1;
                }
            }
        }
    }

    if (spec.count == 0) {
        /* No refspec (a bare repository without one): FETCH_HEAD only. Map
         * the refs against an all-refs set so the summary still shows them. */
        RefspecSet report_spec;
        refspec_set_init(&report_spec);
        if (refspec_set_add(&report_spec, "+refs/*:refs/*") == 0 &&
            repo_map_refspecs(&report_spec, active, 0, &maps, &nmaps) == 0)
            report_fetch(gitdir, maps, nmaps, remote, url);
        refspec_set_free(&report_spec);
        rc = 0;
        goto done;
    }

    if (repo_map_refspecs(&spec, active, !no_tags, &maps, &nmaps) != 0) {
        warn_cgc("cannot map remote refs");
        goto done;
    }

    report_fetch(gitdir, maps, nmaps, remote, url);

    RefWriteOptions ref_opts;
    memset(&ref_opts, 0, sizeof(ref_opts));
    ref_opts.force_tags = (opts && opts->force_tags) || mirror;
    ref_opts.no_tags = no_tags;
    if (repo_write_mappings(gitdir, maps, nmaps, &ref_opts) != 0) {
        warn_cgc("cannot update refs");
        goto done;
    }

    if (!mirror && refs.default_ref &&
        strncmp(refs.default_ref, "refs/heads/", 11) == 0) {
        const char *dst = mapping_dst_for_src(maps, nmaps, refs.default_ref);
        if (dst) {
            Buf head_name;
            buf_init(&head_name);
            buf_appendf(&head_name, "refs/remotes/%s/HEAD", remote);
            if (repo_write_symref(gitdir, (char *)head_name.data, dst) != 0)
                warn_cgc("cannot update remote HEAD");
            buf_free(&head_name);
        }
    }

    if (do_prune && repo_prune_mappings(gitdir, &spec, maps, nmaps) != 0)
        warn_cgc("could not prune stale refs");

    if (!pull) {
        rc = 0;
        goto done;
    }

    if (detached) {
        int moving = memcmp(head_oid, switch_oid, OID_RAW) != 0;
        if (moving && repo_write_orig_head(gitdir, head_oid) != 0)
            warn_cgc("cannot write ORIG_HEAD");
        if (moving &&
            checkout_update(gitdir, worktree, head_oid, switch_oid) != 0) {
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

    if (up_to_date) {
        if (!cgc_quiet)
            fprintf(stderr, "Already up to date.\n");
        rc = 0;
        goto done;
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
    ref_mappings_free(maps, nmaps);
    refspec_set_free(&spec);
    remote_config_free(&cfg);
    free(url);
    free(remote);
    free(head_ref);
    free(target_ref);
    free(merge_alloc);
    free(haves);
    remote_refs_free(&refs);
    free(gitdir);
    free(worktree);
    return rc;
}
