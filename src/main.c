#include "checkout.h"
#include "protocol.h"
#include "repo.h"
#include "url.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            "  owner/repo                            default host shorthand\n"
            "  host.tld/owner/repo                   explicit host, no scheme\n"
            "\n"
            "options:\n"
            "  --host <host>   default host for owner/repo (env CGC_DEFAULT_HOST)\n"
            "  -q, --quiet     suppress progress messages\n"
            "  -h, --help      show this help\n"
            "  -V, --version   show version\n");
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    const char *host = getenv("CGC_DEFAULT_HOST");
    if (!host || !*host)
        host = "github.com";

    int quiet = 0;
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

    char *url;
    char *derived_name;
    url_resolve(positional[0], host, &url, &derived_name);

    const char *dest = npos >= 2 ? positional[1] : derived_name;

    int exists = 0;
    int empty = is_dir_empty(dest, &exists);
    if (empty < 0)
        die("cannot inspect destination '%s': %s", dest, strerror(errno));
    if (exists && !empty)
        die("destination '%s' already exists and is not an empty directory", dest);
    if (!exists && mkdir_p(dest, 0777) != 0)
        die("cannot create destination '%s': %s", dest, strerror(errno));

    if (!quiet)
        fprintf(stderr, "Cloning into '%s'...\n", dest);

    char *gitdir = path_join(dest, ".git");
    if (repo_init(gitdir) != 0)
        die("cannot initialise repository in '%s'", gitdir);

    RemoteRefs refs;
    if (protocol_ls_refs(url, &refs) != 0) {
        remote_refs_free(&refs);
        die("cannot read refs from %s", url);
    }

    const char *head_ref = refs.default_ref ? refs.default_ref : "refs/heads/main";

    if (repo_write_config(gitdir, url, head_ref) != 0 ||
        repo_set_head(gitdir, head_ref) != 0)
        die("cannot write repository configuration");

    if (!refs.empty) {
        if (protocol_fetch(url, &refs, gitdir) != 0) {
            remote_refs_free(&refs);
            die("fetch failed");
        }
        repo_write_refs(gitdir, &refs);

        if (refs.has_default_oid) {
            if (checkout_commit(gitdir, dest, refs.default_oid) != 0) {
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
    return 0;
}
