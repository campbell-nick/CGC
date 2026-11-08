#ifndef CGC_URL_H
#define CGC_URL_H

typedef struct {
    char *scheme;
    char *host;
    int port;
    char *path;
} Url;

/* Parse an absolute http(s) URL. Returns 0 on success. */
int url_parse(const char *url, Url *out);
void url_free(Url *u);

/* Turn a user argument into a full clone URL and a default directory name.
 * Supports:
 *   https://host/owner/repo(.git)
 *   gh:owner/repo      -> https://github.com/owner/repo.git
 *   gl:owner/repo      -> https://gitlab.com/owner/repo.git
 *   bb:owner/repo      -> https://bitbucket.org/owner/repo.git
 *   sym:owner/repo     -> https://git.symlinx.net/owner/repo.git
 *   owner/repo         -> https://<default_host>/owner/repo.git
 *   host.tld/owner/repo-> https://host.tld/owner/repo.git
 * Returns 0 on success; *out_url and *out_name are malloc'd. */
int url_resolve(const char *arg, const char *default_host,
                char **out_url, char **out_name);

#endif
