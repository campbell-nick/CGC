#include "repo.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

int repo_remote_name_is_valid(const char *name)
{
    if (!name || !*name || name[0] == '-' || name[0] == '/')
        return 0;
    if (strstr(name, "..") || strstr(name, "//"))
        return 0;
    size_t len = strlen(name);
    if (name[len - 1] == '/')
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' && *p != '/')
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

int repo_write_symref(const char *gitdir, const char *name, const char *target)
{
    return write_symref_file(gitdir, name, target);
}

int repo_write_refs(const char *gitdir, const RemoteRefs *refs,
                    const char *remote)
{
    if (!repo_remote_name_is_valid(remote))
        return -1;

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

            Buf remote_ref;
            buf_init(&remote_ref);
            buf_appendf(&remote_ref, "refs/remotes/%s/%s", remote,
                        r->name + 11);
            if (r->symref)
                rc = write_symref_file(gitdir, (char *)remote_ref.data,
                                       r->symref);
            else
                rc = write_ref_file(gitdir, (char *)remote_ref.data, r->oid);
            buf_free(&remote_ref);
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
        buf_appendf(&target, "refs/remotes/%s/%s", remote,
                    refs->default_ref + 11);

        /* A single-branch or --branch clone may not fetch the remote's
         * default branch. Do not leave refs/remotes/<remote>/HEAD pointing at
         * a ref that does not exist (git fsck rejects that). */
        unsigned char existing[OID_RAW];
        int have_target = repo_read_ref(gitdir, (char *)target.data,
                                        existing) == 0;
        int rc = 0;
        if (have_target) {
            Buf head;
            buf_init(&head);
            buf_appendf(&head, "refs/remotes/%s/HEAD", remote);
            rc = write_symref_file(gitdir, (char *)head.data,
                                   (char *)target.data);
            buf_free(&head);
        }
        buf_free(&target);
        if (rc != 0)
            return -1;
    }
    return 0;
}

int repo_write_bare_refs(const char *gitdir, const RemoteRefs *refs)
{
    for (size_t i = 0; i < refs->count; i++) {
        const Ref *r = &refs->refs[i];
        if (strcmp(r->name, "HEAD") == 0)
            continue;
        if (strncmp(r->name, "refs/heads/", 11) != 0 &&
            strncmp(r->name, "refs/tags/", 10) != 0)
            continue;

        int rc;
        if (r->symref)
            rc = write_symref_file(gitdir, r->name, r->symref);
        else
            rc = write_ref_file(gitdir, r->name, r->oid);
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
                      const char *default_ref, const char *remote, int bare,
                      int mirror)
{
    if (!remote)
        remote = "origin";
    if (!repo_remote_name_is_valid(remote) || !text_is_safe(url) ||
        !text_is_safe(default_ref))
        return -1;
    if (mirror)
        bare = 1;

    Buf b;
    buf_init(&b);
    buf_appendf(&b,
                "[core]\n"
                "\trepositoryformatversion = 0\n"
                "\tfilemode = true\n"
                "\tbare = %s\n",
                bare ? "true" : "false");
    if (!bare)
        buf_appendf(&b, "\tlogallrefupdates = true\n");
    buf_appendf(&b, "[remote \"%s\"]\n\turl = %s\n", remote, url);
    if (mirror) {
        buf_appendf(&b, "\tfetch = +refs/*:refs/*\n\tmirror = true\n");
    } else if (!bare) {
        buf_appendf(&b, "\tfetch = +refs/heads/*:refs/remotes/%s/*\n",
                    remote);
        if (default_ref && strncmp(default_ref, "refs/heads/", 11) == 0)
            buf_appendf(&b,
                        "[branch \"%s\"]\n"
                        "\tremote = %s\n"
                        "\tmerge = %s\n",
                        default_ref + 11, remote, default_ref);
    }

    char *path = path_join(gitdir, "config");
    int rc = write_file(path, b.data, b.len, 0644);
    buf_free(&b);
    free(path);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Config reading                                                            */
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

typedef struct {
    char *section;
    char *subsection;
    char *key;
    char *value;
} ConfigEntry;

typedef struct {
    ConfigEntry *entries;
    size_t count;
    size_t cap;
} ConfigFile;

static void config_file_free(ConfigFile *cf)
{
    for (size_t i = 0; i < cf->count; i++) {
        free(cf->entries[i].section);
        free(cf->entries[i].subsection);
        free(cf->entries[i].key);
        free(cf->entries[i].value);
    }
    free(cf->entries);
    memset(cf, 0, sizeof(*cf));
}

/* Parse "[section \"subsection\"]". line points at '['. */
static int config_parse_section(const char *line, char **out_section,
                                char **out_subsection)
{
    const char *end = strchr(line, ']');
    if (!end)
        return -1;

    const char *tail = end + 1;
    while (*tail == ' ' || *tail == '\t' || *tail == '\r')
        tail++;
    if (*tail)
        return -1;

    const char *p = line + 1;
    while (*p == ' ' || *p == '\t')
        p++;
    const char *name_end = p;
    while (name_end < end && *name_end != ' ' && *name_end != '\t')
        name_end++;
    if (name_end == p)
        return -1;

    char *section = xstrndup(p, (size_t)(name_end - p));
    char *subsection = NULL;

    const char *q = name_end;
    while (*q == ' ' || *q == '\t')
        q++;
    if (q < end) {
        if (*q != '"')
            goto fail;
        q++;
        const char *sub_end = strchr(q, '"');
        if (!sub_end || sub_end > end)
            goto fail;
        subsection = xstrndup(q, (size_t)(sub_end - q));
        const char *r = sub_end + 1;
        while (*r == ' ' || *r == '\t')
            r++;
        if (r != end)
            goto fail;
    }

    *out_section = section;
    *out_subsection = subsection;
    return 0;

fail:
    free(section);
    free(subsection);
    return -1;
}

/* Read .git/config into a flat list of key/value entries. Returns 0 on
 * success, -1 on I/O or syntax errors. */
static int config_read(const char *gitdir, ConfigFile *cf)
{
    memset(cf, 0, sizeof(*cf));

    char *path = path_join(gitdir, "config");
    Buf b;
    buf_init(&b);
    if (read_file(path, &b) != 0) {
        buf_free(&b);
        free(path);
        return -1;
    }
    free(path);

    char *section = NULL;
    char *subsection = NULL;
    int rc = 0;
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
            char *new_section = NULL;
            char *new_subsection = NULL;
            if (config_parse_section(s, &new_section, &new_subsection) != 0) {
                rc = -1;
                break;
            }
            free(section);
            free(subsection);
            section = new_section;
            subsection = new_subsection;
            continue;
        }
        if (!section)
            continue;

        char *key = s;
        char *value = (char *)"true";
        char *eq = strchr(s, '=');
        if (eq) {
            *eq = '\0';
            key = trim_ws(s);
            value = trim_ws(eq + 1);
        }
        if (!*key) {
            rc = -1;
            break;
        }

        if (cf->count == cf->cap) {
            cf->cap = cf->cap ? cf->cap * 2 : 16;
            cf->entries =
                xrealloc(cf->entries, cf->cap * sizeof(ConfigEntry));
        }
        ConfigEntry *e = &cf->entries[cf->count++];
        e->section = xstrdup(section);
        e->subsection = subsection ? xstrdup(subsection) : NULL;
        e->key = xstrdup(key);
        e->value = xstrdup(value);
    }

    free(section);
    free(subsection);
    buf_free(&b);
    if (rc != 0)
        config_file_free(cf);
    return rc;
}

static int parse_bool(const char *value, int *out)
{
    if (!value)
        return -1;
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0) {
        *out = 1;
        return 0;
    }
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0 || strcmp(value, "0") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

void remote_config_free(RemoteConfig *rc)
{
    if (!rc)
        return;
    free(rc->name);
    free(rc->url);
    for (size_t i = 0; i < rc->nfetch; i++)
        free(rc->fetch[i]);
    free(rc->fetch);
    memset(rc, 0, sizeof(*rc));
}

int repo_read_remote_config(const char *gitdir, const char *name,
                            RemoteConfig *out)
{
    memset(out, 0, sizeof(*out));
    if (!repo_remote_name_is_valid(name))
        return -1;

    ConfigFile cf;
    if (config_read(gitdir, &cf) != 0)
        return -1;

    int found = 0;
    int rc = 0;
    for (size_t i = 0; i < cf.count; i++) {
        ConfigEntry *e = &cf.entries[i];
        if (strcasecmp(e->section, "remote") != 0 || !e->subsection ||
            strcmp(e->subsection, name) != 0)
            continue;

        found = 1;
        if (strcasecmp(e->key, "url") == 0) {
            if (!text_is_safe(e->value) || !*e->value) {
                rc = -1;
                break;
            }
            free(out->url);
            out->url = xstrdup(e->value);
        } else if (strcasecmp(e->key, "fetch") == 0) {
            if (!text_is_safe(e->value) || !*e->value) {
                rc = -1;
                break;
            }
            out->fetch =
                xrealloc(out->fetch, (out->nfetch + 1) * sizeof(char *));
            out->fetch[out->nfetch++] = xstrdup(e->value);
        } else if (strcasecmp(e->key, "mirror") == 0) {
            if (parse_bool(e->value, &out->mirror) != 0) {
                rc = -1;
                break;
            }
        } else if (strcasecmp(e->key, "prune") == 0) {
            if (parse_bool(e->value, &out->prune) != 0) {
                rc = -1;
                break;
            }
        }
    }

    config_file_free(&cf);
    if (rc != 0) {
        remote_config_free(out);
        return -1;
    }
    if (!found) {
        remote_config_free(out);
        return 1;
    }
    out->name = xstrdup(name);
    if (!out->url) {
        remote_config_free(out);
        return -1;
    }
    return 0;
}

static int config_remote_exists(const ConfigFile *cf, const char *name)
{
    for (size_t i = 0; i < cf->count; i++) {
        const ConfigEntry *e = &cf->entries[i];
        if (strcasecmp(e->section, "remote") == 0 && e->subsection &&
            strcmp(e->subsection, name) == 0 &&
            repo_remote_name_is_valid(e->subsection))
            return 1;
    }
    return 0;
}

int repo_list_remotes(const char *gitdir, char ***out_names, size_t *out_count)
{
    *out_names = NULL;
    *out_count = 0;

    ConfigFile cf;
    if (config_read(gitdir, &cf) != 0)
        return -1;

    char **names = NULL;
    size_t count = 0;
    for (size_t i = 0; i < cf.count; i++) {
        ConfigEntry *e = &cf.entries[i];
        if (strcasecmp(e->section, "remote") != 0 || !e->subsection)
            continue;
        if (!repo_remote_name_is_valid(e->subsection))
            continue;

        int seen = 0;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(names[j], e->subsection) == 0) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            names = xrealloc(names, (count + 1) * sizeof(char *));
            names[count++] = xstrdup(e->subsection);
        }
    }

    config_file_free(&cf);
    *out_names = names;
    *out_count = count;
    return 0;
}

int repo_find_remote(const char *gitdir, const char *branch_ref,
                     char **out_name)
{
    *out_name = NULL;

    ConfigFile cf;
    if (config_read(gitdir, &cf) != 0)
        return -1;

    int rc = 1;
    char *branch_remote = NULL;

    if (branch_ref && strncmp(branch_ref, "refs/heads/", 11) == 0) {
        const char *shortname = branch_ref + 11;
        for (size_t i = 0; i < cf.count; i++) {
            ConfigEntry *e = &cf.entries[i];
            if (strcasecmp(e->section, "branch") != 0 || !e->subsection ||
                strcmp(e->subsection, shortname) != 0)
                continue;
            if (strcasecmp(e->key, "remote") != 0)
                continue;
            if (!text_is_safe(e->value) || !*e->value) {
                rc = -1;
                goto done;
            }
            branch_remote = xstrdup(e->value);
            break;
        }

        if (branch_remote && strcmp(branch_remote, ".") != 0 &&
            config_remote_exists(&cf, branch_remote)) {
            *out_name = xstrdup(branch_remote);
            rc = 0;
            goto done;
        }
    }

    if (config_remote_exists(&cf, "origin")) {
        *out_name = xstrdup("origin");
        rc = 0;
        goto done;
    }

    for (size_t i = 0; i < cf.count; i++) {
        ConfigEntry *e = &cf.entries[i];
        if (strcasecmp(e->section, "remote") == 0 && e->subsection &&
            repo_remote_name_is_valid(e->subsection)) {
            *out_name = xstrdup(e->subsection);
            rc = 0;
            break;
        }
    }

done:
    free(branch_remote);
    config_file_free(&cf);
    return rc;
}

int repo_read_branch_merge(const char *gitdir, const char *branch_ref,
                           char **out_merge)
{
    *out_merge = NULL;
    if (!branch_ref || strncmp(branch_ref, "refs/heads/", 11) != 0)
        return 1;

    ConfigFile cf;
    if (config_read(gitdir, &cf) != 0)
        return -1;

    int rc = 1;
    const char *shortname = branch_ref + 11;
    for (size_t i = 0; i < cf.count; i++) {
        ConfigEntry *e = &cf.entries[i];
        if (strcasecmp(e->section, "branch") != 0 || !e->subsection ||
            strcmp(e->subsection, shortname) != 0)
            continue;
        if (strcasecmp(e->key, "merge") != 0)
            continue;
        if (!ref_name_is_valid(e->value)) {
            rc = -1;
            break;
        }
        *out_merge = xstrdup(e->value);
        rc = 0;
        break;
    }

    config_file_free(&cf);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Refspec mapping and writing                                               */
/* ------------------------------------------------------------------------- */

void ref_mappings_free(RefMapping *maps, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(maps[i].src);
        free(maps[i].dst);
        free(maps[i].symref);
    }
    free(maps);
}

int repo_refspec_selects(const RefspecSet *set, const char *refname,
                         int include_tags)
{
    if (set->count == 0)
        return 1;

    char *dst = NULL;
    int rc = refspec_set_map(set, refname, &dst, NULL);
    free(dst);
    if (rc == REFSPEC_MATCH)
        return 1;
    if (rc == REFSPEC_EXCLUDED || rc < 0)
        return 0;
    return include_tags && strncmp(refname, "refs/tags/", 10) == 0;
}

int repo_map_refspecs(const RefspecSet *set, const RemoteRefs *refs,
                      int include_tags, RefMapping **out, size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    RefMapping *maps = NULL;
    size_t count = 0;
    size_t cap = 0;

    for (size_t i = 0; i < refs->count; i++) {
        const Ref *r = &refs->refs[i];
        if (strcmp(r->name, "HEAD") == 0)
            continue;

        char *dst = NULL;
        int force = 0;
        int rc = refspec_set_map(set, r->name, &dst, &force);
        if (rc < 0) {
            free(dst);
            ref_mappings_free(maps, count);
            return -1;
        }
        if (rc == REFSPEC_EXCLUDED) {
            free(dst);
            continue;
        }
        if (rc == REFSPEC_NO_MATCH) {
            if (!include_tags || strncmp(r->name, "refs/tags/", 10) != 0)
                continue;
            dst = xstrdup(r->name);
            force = 0;
        }

        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            maps = xrealloc(maps, cap * sizeof(RefMapping));
        }
        RefMapping *m = &maps[count++];
        memset(m, 0, sizeof(*m));
        m->src = xstrdup(r->name);
        m->dst = dst;
        m->symref = r->symref ? xstrdup(r->symref) : NULL;
        memcpy(m->oid, r->oid, OID_RAW);
        m->force = force;
    }

    *out = maps;
    *out_count = count;
    return 0;
}

int repo_write_mappings(const char *gitdir, const RefMapping *maps, size_t count,
                        const RefWriteOptions *opts)
{
    int no_tags = opts && opts->no_tags;

    /* Preflight every tag before writing anything. A clobber refusal must not
     * leave the other refs half updated. */
    for (size_t i = 0; i < count; i++) {
        const RefMapping *m = &maps[i];
        if (!m->dst || m->symref || strncmp(m->dst, "refs/tags/", 10) != 0)
            continue;
        if (no_tags)
            continue;

        int force = m->force || (opts && opts->force_tags);
        unsigned char existing[OID_RAW];
        int rc = repo_read_ref(gitdir, m->dst, existing);
        if (rc < 0) {
            warn_cgc("cannot read local tag '%s'", m->dst + 10);
            return -1;
        }
        if (rc == 0 && memcmp(existing, m->oid, OID_RAW) != 0 && !force) {
            warn_cgc("tag '%s' would be clobbered (use --force to overwrite)",
                     m->dst + 10);
            return -1;
        }
    }

    for (size_t i = 0; i < count; i++) {
        const RefMapping *m = &maps[i];
        if (!m->dst)
            continue;
        if (no_tags && strncmp(m->dst, "refs/tags/", 10) == 0)
            continue;

        int rc;
        if (m->symref)
            rc = write_symref_file(gitdir, m->dst, m->symref);
        else
            rc = write_ref_file(gitdir, m->dst, m->oid);
        if (rc != 0)
            return -1;
    }
    return 0;
}

int repo_write_mirror_refs(const char *gitdir, const RemoteRefs *refs)
{
    RefspecSet set;
    refspec_set_init(&set);

    RefMapping *maps = NULL;
    size_t nmaps = 0;
    int rc = -1;

    if (refspec_set_add(&set, "+refs/*:refs/*") != 0)
        goto done;
    if (repo_map_refspecs(&set, refs, 0, &maps, &nmaps) != 0)
        goto done;

    RefWriteOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.force_tags = 1;
    rc = repo_write_mappings(gitdir, maps, nmaps, &opts);

done:
    ref_mappings_free(maps, nmaps);
    refspec_set_free(&set);
    return rc;
}

static int mapping_has_dst(const RefMapping *maps, size_t count,
                           const char *dst)
{
    for (size_t i = 0; i < count; i++) {
        if (maps[i].dst && strcmp(maps[i].dst, dst) == 0)
            return 1;
    }
    return 0;
}

static int ref_matches_wildcard_dst(const RefspecSet *set, const char *name)
{
    for (size_t i = 0; i < set->count; i++) {
        const Refspec *rs = &set->items[i];
        if (rs->negative || !rs->dst || !strchr(rs->dst, '*'))
            continue;
        if (refspec_dst_matches(rs, name))
            return 1;
    }
    return 0;
}

/* Return the literal directory prefix of a wildcard destination. Only
 * patterns whose '*' is a whole path component are prunable. Returns 0 when a
 * prefix was produced, 1 otherwise. */
static int dst_wildcard_prefix(const char *dst, char **out_prefix)
{
    const char *star = strchr(dst, '*');
    if (!star || star == dst || star[-1] != '/')
        return 1;
    *out_prefix = xstrndup(dst, (size_t)(star - dst));
    return 0;
}

static int ref_file_is_symref(const char *path)
{
    Buf b;
    buf_init(&b);
    int rc = read_file(path, &b);
    int sym = rc == 0 && b.len >= 5 && memcmp(b.data, "ref: ", 5) == 0;
    buf_free(&b);
    return sym;
}

static int prune_loose_under(const char *gitdir, const char *rel,
                             const RefspecSet *set, const RefMapping *maps,
                             size_t count, int remove_self)
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
        buf_append(&child, rel, strlen(rel));
        if (child.len && child.data[child.len - 1] != '/')
            buf_putc(&child, '/');
        buf_append(&child, de->d_name, strlen(de->d_name));

        char *full = path_join(gitdir, (char *)child.data);
        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (prune_loose_under(gitdir, (char *)child.data, set, maps,
                                      count, 1) != 0)
                    rc = -1;
                rmdir(full);
            } else if (S_ISREG(st.st_mode)) {
                const char *name = (char *)child.data;
                size_t n = strlen(name);
                int is_lock =
                    n >= 5 && strcmp(name + n - 5, ".lock") == 0;
                if (!is_lock && !ref_file_is_symref(full) &&
                    ref_matches_wildcard_dst(set, name) &&
                    !mapping_has_dst(maps, count, name)) {
                    if (unlink(full) != 0)
                        rc = -1;
                }
            }
        }
        free(full);
        buf_free(&child);
    }

    closedir(d);
    /* Keep the namespace root itself (for example .git/refs) so bare-repo
     * detection and later ref writes still find it. */
    if (remove_self)
        rmdir(dir);
    free(dir);
    return rc;
}

static int prune_packed_mappings(const char *gitdir, const RefspecSet *set,
                                 const RefMapping *maps, size_t count)
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
                int drop = ref_matches_wildcard_dst(set, name) &&
                           !mapping_has_dst(maps, count, name);
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

int repo_prune_mappings(const char *gitdir, const RefspecSet *set,
                        const RefMapping *maps, size_t count)
{
    int rc = 0;
    for (size_t i = 0; i < set->count; i++) {
        const Refspec *rs = &set->items[i];
        if (rs->negative || !rs->dst || !strchr(rs->dst, '*'))
            continue;

        char *prefix = NULL;
        if (dst_wildcard_prefix(rs->dst, &prefix) != 0)
            continue;
        if (prune_loose_under(gitdir, prefix, set, maps, count, 0) != 0)
            rc = -1;
        free(prefix);
    }

    if (prune_packed_mappings(gitdir, set, maps, count) != 0)
        rc = -1;
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Ref and config reading                                                    */
/* ------------------------------------------------------------------------- */

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

        Buf desc;
        buf_init(&desc);
        if (strncmp(r->name, "refs/heads/", 11) == 0)
            buf_appendf(&desc, "branch '%s'", r->name + 11);
        else if (strncmp(r->name, "refs/tags/", 10) == 0)
            buf_appendf(&desc, "tag '%s'", r->name + 10);
        else if (strncmp(r->name, "refs/", 5) == 0)
            buf_appendf(&desc, "'%s'", r->name);
        else
            continue;

        int mergeable = merge_ref && strcmp(r->name, merge_ref) == 0;
        buf_appendf(&b, "%s\t%s\t%s of %s\n", hex,
                    mergeable ? "" : "not-for-merge",
                    (char *)desc.data, url);
        buf_free(&desc);
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
