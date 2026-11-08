#include "url.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int url_parse(const char *url, Url *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = strstr(url, "://");
    if (!p)
        return -1;

    out->scheme = xstrndup(url, (size_t)(p - url));
    if (strcmp(out->scheme, "https") != 0 && strcmp(out->scheme, "http") != 0) {
        free(out->scheme);
        out->scheme = NULL;
        return -1;
    }
    p += 3;

    const char *auth_end = strchr(p, '/');
    if (!auth_end)
        auth_end = p + strlen(p);

    const char *host_start = p;
    const char *at = memchr(p, '@', (size_t)(auth_end - p));
    if (at)
        host_start = at + 1;

    const char *colon = NULL;
    for (const char *q = host_start; q < auth_end; q++) {
        if (*q == ':') {
            colon = q;
            break;
        }
    }

    const char *host_end = colon ? colon : auth_end;
    if (host_end == host_start)
        goto fail;
    out->host = xstrndup(host_start, (size_t)(host_end - host_start));

    int default_port = strcmp(out->scheme, "https") == 0 ? 443 : 80;
    out->port = default_port;
    if (colon) {
        char *end = NULL;
        long v = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || v <= 0 || v > 65535)
            goto fail;
        out->port = (int)v;
    }

    if (*auth_end)
        out->path = xstrdup(auth_end);
    else
        out->path = xstrdup("/");

    return 0;

fail:
    url_free(out);
    return -1;
}

void url_free(Url *u)
{
    free(u->scheme);
    free(u->host);
    free(u->path);
    memset(u, 0, sizeof(*u));
}

static char *append_git_suffix(const char *base)
{
    size_t n = strlen(base);
    if (n >= 4 && strcmp(base + n - 4, ".git") == 0)
        return xstrdup(base);
    Buf b;
    buf_init(&b);
    buf_appendf(&b, "%s.git", base);
    return (char *)b.data;
}

static char *name_from_path(const char *path)
{
    const char *end = path + strlen(path);
    while (end > path && end[-1] == '/')
        end--;
    const char *start = end;
    while (start > path && start[-1] != '/')
        start--;
    char *name = xstrndup(start, (size_t)(end - start));
    size_t n = strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".git") == 0)
        name[n - 4] = 0;
    return name;
}

int url_resolve(const char *arg, const char *default_host,
                char **out_url, char **out_name)
{
    char *url = NULL;
    int is_shorthand = 0;

    if (strncmp(arg, "gh:", 3) == 0) {
        Buf b;
        buf_init(&b);
        buf_appendf(&b, "https://github.com/%s", arg + 3);
        url = (char *)b.data;
        is_shorthand = 1;
    } else if (strncmp(arg, "gl:", 3) == 0) {
        Buf b;
        buf_init(&b);
        buf_appendf(&b, "https://gitlab.com/%s", arg + 3);
        url = (char *)b.data;
        is_shorthand = 1;
    } else if (strncmp(arg, "bb:", 3) == 0) {
        Buf b;
        buf_init(&b);
        buf_appendf(&b, "https://bitbucket.org/%s", arg + 3);
        url = (char *)b.data;
        is_shorthand = 1;
    } else if (strstr(arg, "://")) {
        url = xstrdup(arg);
    } else {
        /* Bare path: either owner/repo or host.tld/owner/repo. */
        const char *slash = strchr(arg, '/');
        if (!slash || slash == arg)
            die("'%s' is not a repository URL or owner/repo shorthand", arg);

        const char *host = default_host;
        const char *path = arg;
        size_t first_len = (size_t)(slash - arg);
        int segments = 1;
        for (const char *q = arg; *q; q++)
            if (*q == '/' && q[1] && q[1] != '/')
                segments++;
        int first_looks_like_host =
            memchr(arg, '.', first_len) != NULL && segments >= 3;
        if (first_looks_like_host) {
            host = xstrndup(arg, first_len);
            path = slash + 1;
        }

        Buf b;
        buf_init(&b);
        buf_appendf(&b, "https://%s/%s", host, path);
        url = (char *)b.data;
        is_shorthand = 1;
        if (first_looks_like_host)
            free((char *)host);
    }

    /* Trim trailing slashes. */
    size_t n = strlen(url);
    while (n > 0 && url[n - 1] == '/')
        url[--n] = 0;

    if (is_shorthand) {
        char *with_git = append_git_suffix(url);
        free(url);
        url = with_git;
    }

    Url parsed;
    if (url_parse(url, &parsed) != 0)
        die("unsupported or malformed URL: %s", url);
    char *name = name_from_path(parsed.path);
    url_free(&parsed);

    if (!*name)
        die("cannot derive a directory name from '%s'", arg);

    *out_url = url;
    *out_name = name;
    return 0;
}
