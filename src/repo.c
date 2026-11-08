#include "repo.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

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

static void write_ref_file(const char *gitdir, const char *name,
                           const unsigned char oid[OID_RAW])
{
    if (!ref_name_is_valid(name))
        return;
    char hex[OID_HEX + 1];
    oid_hex(oid, hex);

    char *path = path_join(gitdir, name);
    char *dir = path_dirname(path);
    if (mkdir_p(dir, 0777) == 0) {
        char line[OID_HEX + 2];
        memcpy(line, hex, OID_HEX);
        line[OID_HEX] = '\n';
        write_file(path, line, OID_HEX + 1, 0644);
    }
    free(dir);
    free(path);
}

static void write_symref_file(const char *gitdir, const char *name,
                              const char *target)
{
    if (!ref_name_is_valid(name) || !ref_name_is_valid(target))
        return;
    char *path = path_join(gitdir, name);
    char *dir = path_dirname(path);
    if (mkdir_p(dir, 0777) == 0) {
        Buf b;
        buf_init(&b);
        buf_appendf(&b, "ref: %s\n", target);
        write_file(path, b.data, b.len, 0644);
        buf_free(&b);
    }
    free(dir);
    free(path);
}

int repo_write_refs(const char *gitdir, const RemoteRefs *refs)
{
    for (size_t i = 0; i < refs->count; i++) {
        const Ref *r = &refs->refs[i];
        if (strcmp(r->name, "HEAD") == 0)
            continue;

        if (strncmp(r->name, "refs/heads/", 11) == 0) {
            if (r->symref)
                write_symref_file(gitdir, r->name, r->symref);
            else
                write_ref_file(gitdir, r->name, r->oid);

            Buf remote;
            buf_init(&remote);
            buf_appendf(&remote, "refs/remotes/origin/%s", r->name + 11);
            if (r->symref)
                write_symref_file(gitdir, (char *)remote.data, r->symref);
            else
                write_ref_file(gitdir, (char *)remote.data, r->oid);
            buf_free(&remote);
        } else if (strncmp(r->name, "refs/tags/", 10) == 0) {
            if (r->symref)
                write_symref_file(gitdir, r->name, r->symref);
            else
                write_ref_file(gitdir, r->name, r->oid);
        }
    }

    if (refs->default_ref && strncmp(refs->default_ref, "refs/heads/", 11) == 0) {
        Buf target;
        buf_init(&target);
        buf_appendf(&target, "refs/remotes/origin/%s", refs->default_ref + 11);
        write_symref_file(gitdir, "refs/remotes/origin/HEAD", (char *)target.data);
        buf_free(&target);
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
    int rc = write_file(path, b.data, b.len, 0644);
    buf_free(&b);
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
