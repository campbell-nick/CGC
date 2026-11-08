#include "checkout.h"
#include "http.h"
#include "protocol.h"
#include "repo.h"
#include "url.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fprintf(out,
            "usage: cgc [options] <repository> [directory]\n"
            "\n"
            "Clone a Git repository over HTTPS.\n"
            "\n"
            "repository:\n"
            "  https://github.com/owner/repo(.git)   explicit URL\n"
            "  gh:owner/repo                         GitHub shorthand\n"
            "  gl:owner/repo                         GitLab shorthand\n"
            "  bb:owner/repo                         Bitbucket shorthand\n"
            "  sym:owner/repo                        git.symlinx.net shorthand\n"
            "  owner/repo                            default host shorthand\n"
            "  host.tld/owner/repo                   explicit host, no scheme\n"
            "\n"
            "options:\n"
            "  --host <host>   default host for owner/repo (env CGC_DEFAULT_HOST)\n"
            "  --token <tok>   bearer token for HTTPS auth (env CGC_TOKEN/GIT_TOKEN)\n"
            "  -b, --branch <name>\n"
            "                 checkout <name> instead of the remote's default branch\n"
            "  --depth <n>     create a shallow clone with history truncated to n commits\n"
            "  --single-branch clone only the history of one branch\n"
            "  --no-single-branch\n"
            "                 fetch all branches (default unless --depth is given)\n"
            "  -q, --quiet     suppress progress messages\n"
            "  -h, --help      show this help\n"
            "  -V, --version   show version\n");
}

static char *cleanup_path = NULL;
static int cleanup_enabled = 0;
static int cleanup_created = 0;

static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 ||
                    strcmp(de->d_name, "..") == 0)
                    continue;
                char *child = path_join(path, de->d_name);
                remove_tree(child);
                free(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void cleanup_atexit(void)
{
    if (cleanup_enabled && cleanup_path && cleanup_created)
        remove_tree(cleanup_path);
    free(cleanup_path);
    cleanup_path = NULL;
}

static void on_signal(int sig)
{
    (void)sig;
    cgc_interrupted = 1;
}

int main(int argc, char **argv)
{
    atexit(cleanup_atexit);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: interrupted syscalls should return EINTR. */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    const char *host = getenv("CGC_DEFAULT_HOST");
    if (!host || !*host)
        host = "github.com";

    const char *token = getenv("CGC_TOKEN");
    if (!token || !*token)
        token = getenv("GIT_TOKEN");

    int quiet = 0;
    const char *branch = NULL;
    int depth = 0;
    int single_branch = 0;
    int no_single_branch = 0;
    const char *positional[3];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("cgc %s\n", CGC_VERSION);
            return 0;
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(a, "--host") == 0) {
            if (i + 1 >= argc)
                die("--host requires an argument");
            host = argv[++i];
        } else if (strcmp(a, "--token") == 0) {
            if (i + 1 >= argc)
                die("--token requires an argument");
            token = argv[++i];
        } else if (strcmp(a, "-b") == 0 || strcmp(a, "--branch") == 0) {
            if (i + 1 >= argc)
                die("--branch requires an argument");
            branch = argv[++i];
        } else if (strcmp(a, "--depth") == 0) {
            if (i + 1 >= argc)
                die("--depth requires an argument");
            depth = atoi(argv[++i]);
            if (depth <= 0)
                die("--depth must be a positive integer");
        } else if (strcmp(a, "--single-branch") == 0) {
            single_branch = 1;
        } else if (strcmp(a, "--no-single-branch") == 0) {
            no_single_branch = 1;
        } else if (strcmp(a, "--") == 0) {
            for (i++; i < argc; i++) {
                if (npos >= 2)
                    die("too many arguments");
                positional[npos++] = argv[i];
            }
        } else if (a[0] == '-' && a[1] != 0) {
            die("unknown option: %s", a);
        } else {
            if (npos >= 2)
                die("too many arguments");
            positional[npos++] = a;
        }
    }

    if (npos == 0) {
        usage(stderr);
        return 2;
    }

    cgc_quiet = quiet;

    http_set_token(token);

    char *url;
    char *derived_name;
    url_resolve(positional[0], host, &url, &derived_name);

    /* Strip any embedded credentials from the URL so they are not persisted
     * in .git/config. Use --token / CGC_TOKEN / GIT_TOKEN for auth. */
    {
        char *p = strstr(url, "://");
        if (p) {
            p += 3;
            char *at = strchr(p, '@');
            char *slash = strchr(p, '/');
            if (at && (!slash || at < slash)) {
                size_t prefix = (size_t)(p - url);
                size_t rest = strlen(at + 1);
                memmove(url + prefix, at + 1, rest + 1);
            }
        }
    }

    const char *dest = npos >= 2 ? positional[1] : derived_name;

    int exists = 0;
    int empty = is_dir_empty(dest, &exists);
    if (empty < 0)
        die("cannot inspect destination '%s': %s", dest, strerror(errno));
    if (exists && !empty)
        die("destination '%s' already exists and is not an empty directory", dest);
    if (!exists && mkdir_p(dest, 0777) != 0)
        die("cannot create destination '%s': %s", dest, strerror(errno));

    cleanup_created = !exists;
    cleanup_path = xstrdup(dest);
    cleanup_enabled = 1;

    if (!quiet)
        fprintf(stderr, "Cloning into '%s'...\n", dest);

    char *gitdir = path_join(dest, ".git");
    if (repo_init(gitdir) != 0)
        die("cannot initialise repository in '%s'", gitdir);

    if (no_single_branch)
        single_branch = 0;
    else if (depth > 0)
        single_branch = 1;

    RemoteRefs refs;
    if (protocol_ls_refs(url, &refs) != 0) {
        remote_refs_free(&refs);
        die("cannot read refs from %s", url);
    }

    /* Single-branch: keep only HEAD and the chosen branch/tag's refs. */
    if (single_branch && !refs.empty) {
        const char *sb_name = branch;
        if (!sb_name && refs.default_ref &&
            strncmp(refs.default_ref, "refs/heads/", 11) == 0)
            sb_name = refs.default_ref + 11;

        if (sb_name && *sb_name) {
            char hbuf[512], tbuf[512];
            snprintf(hbuf, sizeof(hbuf), "refs/heads/%s", sb_name);
            snprintf(tbuf, sizeof(tbuf), "refs/tags/%s", sb_name);

            size_t w = 0;
            for (size_t i = 0; i < refs.count; i++) {
                Ref *r = &refs.refs[i];
                int keep = (strcmp(r->name, "HEAD") == 0 ||
                            strcmp(r->name, hbuf) == 0 ||
                            strcmp(r->name, tbuf) == 0);
                if (keep) {
                    if (w != i)
                        refs.refs[w] = refs.refs[i];
                    w++;
                } else {
                    free(refs.refs[i].name);
                    free(refs.refs[i].symref);
                }
            }
            refs.count = w;
        }
    }

    /* Resolve the requested branch/tag (or the remote's default). */
    const char *head_ref = refs.default_ref ? refs.default_ref : "refs/heads/main";
    unsigned char target_oid[OID_RAW];
    memset(target_oid, 0, sizeof(target_oid));
    int has_target = refs.has_default_oid;
    int detached = 0;
    const char *config_ref = head_ref;

    if (branch) {
        if (refs.empty)
            die("cannot checkout '%s' in an empty repository", branch);

        Ref *chosen = NULL;
        char buf[512];
        snprintf(buf, sizeof(buf), "refs/heads/%s", branch);
        /* store candidates in static-ish buffers via direct comparison */
        /* We'll search by trying full names. */
        for (size_t i = 0; i < refs.count; i++) {
            Ref *r = &refs.refs[i];
            if (strcmp(r->name, branch) == 0 ||
                strcmp(r->name, buf) == 0) {
                chosen = r;
                break;
            }
            char tbuf[512];
            snprintf(tbuf, sizeof(tbuf), "refs/tags/%s", branch);
            if (strcmp(r->name, tbuf) == 0) {
                chosen = r;
                break;
            }
        }
        if (!chosen) {
            /* try refs/tags/<branch> explicitly */
            char tbuf[512];
            snprintf(tbuf, sizeof(tbuf), "refs/tags/%s", branch);
            for (size_t i = 0; i < refs.count; i++) {
                if (strcmp(refs.refs[i].name, tbuf) == 0) {
                    chosen = &refs.refs[i];
                    break;
                }
            }
        }
        if (!chosen)
            die("Remote branch or tag '%s' not found in upstream origin", branch);

        if (chosen->has_peeled)
            memcpy(target_oid, chosen->peeled, OID_RAW);
        else
            memcpy(target_oid, chosen->oid, OID_RAW);
        has_target = 1;

        if (strncmp(chosen->name, "refs/heads/", 11) == 0) {
            detached = 0;
            head_ref = chosen->name;
            config_ref = chosen->name;
        } else {
            detached = 1;
            head_ref = NULL;
            config_ref = NULL;
        }
    } else if (refs.has_default_oid) {
        memcpy(target_oid, refs.default_oid, OID_RAW);
    }

    if (repo_write_config(gitdir, url, config_ref) != 0)
        die("cannot write repository configuration");
    if (detached) {
        if (repo_set_head_detached(gitdir, target_oid) != 0)
            die("cannot write HEAD");
    } else {
        if (repo_set_head(gitdir, head_ref) != 0)
            die("cannot write HEAD");
    }

    if (!refs.empty) {
        if (protocol_fetch(url, &refs, gitdir, depth) != 0) {
            remote_refs_free(&refs);
            die("fetch failed");
        }
        repo_write_refs(gitdir, &refs);

        if (has_target) {
            if (checkout_commit(gitdir, dest, target_oid) != 0) {
                remote_refs_free(&refs);
                die("checkout failed");
            }
        }
    } else {
        fprintf(stderr,
                "warning: You appear to have cloned an empty repository.\n");
    }

    remote_refs_free(&refs);
    free(gitdir);
    free(url);
    free(derived_name);

    cleanup_enabled = 0;
    return 0;
}
