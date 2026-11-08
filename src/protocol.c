#include "protocol.h"
#include "http.h"
#include "odb.h"
#include "pack.h"
#include "pktline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *endpoint(const char *url, const char *suffix)
{
    Buf b;
    buf_init(&b);
    buf_appendf(&b, "%s%s", url, suffix);
    return (char *)b.data;
}

static int line_has(const unsigned char *line, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    if (n > len)
        return 0;
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(line + i, needle, n) == 0)
            return 1;
    return 0;
}

static int discover(const char *url, int *unborn_ok)
{
    *unborn_ok = 0;
    char *u = endpoint(url, "/info/refs?service=git-upload-pack");
    HttpResponse resp;
    int rc = http_request("GET", u, NULL, NULL, 0,
                          "Git-Protocol: version=2\r\n", &resp);
    free(u);
    if (rc != 0)
        return -1;

    if (resp.status != 200) {
        warn_cgc("remote returned HTTP %d while discovering refs", resp.status);
        http_response_free(&resp);
        return -1;
    }

    PktReader pr;
    pkt_reader_init(&pr, resp.body.data, resp.body.len);
    const unsigned char *line;
    size_t linelen;
    int kind = pkt_read(&pr, &line, &linelen);

    /* Smart HTTP prefixes the service announcement before the v2 data. */
    if (kind == PKT_DATA && linelen >= 10 &&
        memcmp(line, "# service=", 10) == 0) {
        while ((kind = pkt_read(&pr, &line, &linelen)) == PKT_DATA)
            ;
        if (kind == PKT_FLUSH || kind == PKT_DELIM)
            kind = pkt_read(&pr, &line, &linelen);
    }

    int ok = 0;
    if (kind == PKT_DATA && linelen >= 9 &&
        memcmp(line, "version 2", 9) == 0)
        ok = 1;

    /* Drain the capability advertisement. */
    while ((kind = pkt_read(&pr, &line, &linelen)) == PKT_DATA) {
        if (linelen >= 7 && memcmp(line, "ls-refs", 7) == 0 &&
            line_has(line, linelen, "unborn"))
            *unborn_ok = 1;
        if (linelen >= 14 && memcmp(line, "object-format=", 14) == 0) {
            size_t rest = linelen - 14;
            while (rest > 0 &&
                   (line[14 + rest - 1] == '\n' || line[14 + rest - 1] == '\r'))
                rest--;
            if (rest != 4 || memcmp(line + 14, "sha1", 4) != 0) {
                warn_cgc("only SHA-1 repositories are supported");
                http_response_free(&resp);
                return -1;
            }
        }
    }

    http_response_free(&resp);
    if (!ok) {
        warn_cgc("server does not support Git protocol v2");
        return -1;
    }
    return 0;
}

static char *trim_newline(const unsigned char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;
    return xstrndup((char *)line, len);
}

static int parse_ref_line(const unsigned char *line, size_t len, Ref *ref)
{
    char *s = trim_newline(line, len);
    int unborn = 0;
    char *save = NULL;
    char *tok = strtok_r(s, " ", &save);
    if (!tok) {
        free(s);
        return -1;
    }

    if (strcmp(tok, "unborn") == 0) {
        unborn = 1;
        tok = strtok_r(NULL, " ", &save);
        if (!tok) {
            free(s);
            return -1;
        }
    } else {
        if (oid_parse(tok, ref->oid) != 0) {
            free(s);
            return -1;
        }
        tok = strtok_r(NULL, " ", &save);
        if (!tok) {
            free(s);
            return -1;
        }
    }

    ref->name = xstrdup(tok);
    if (!unborn && ref->name[0] == 0) {
        free(s);
        return -1;
    }

    while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
        if (strncmp(tok, "symref-target:", 14) == 0) {
            ref->symref = xstrdup(tok + 14);
        } else if (strncmp(tok, "peeled:", 7) == 0) {
            if (oid_parse(tok + 7, ref->peeled) == 0)
                ref->has_peeled = 1;
        }
    }

    free(s);
    return unborn ? 1 : 0;
}

static int post_command(const char *url, const char *endpoint_path,
                        Buf *req, HttpResponse *resp)
{
    char *u = endpoint(url, endpoint_path);
    int rc = http_request("POST", u,
                          "application/x-git-upload-pack-request",
                          req->data, req->len,
                          "Git-Protocol: version=2\r\n"
                          "Accept: application/x-git-upload-pack-result\r\n",
                          resp);
    free(u);
    return rc;
}

int protocol_ls_refs(const char *url, RemoteRefs *out)
{
    memset(out, 0, sizeof(*out));

    int unborn_ok = 0;
    if (discover(url, &unborn_ok) != 0)
        return -1;

    Buf req;
    buf_init(&req);
    pkt_write_str(&req, "command=ls-refs\n");
    pkt_write_str(&req, "agent=cgc/" CGC_VERSION "\n");
    pkt_write_str(&req, "object-format=sha1\n");
    pkt_delim(&req);
    pkt_write_str(&req, "peel\n");
    pkt_write_str(&req, "symrefs\n");
    if (unborn_ok)
        pkt_write_str(&req, "unborn\n");
    pkt_write_str(&req, "ref-prefix HEAD\n");
    pkt_write_str(&req, "ref-prefix refs/heads/\n");
    pkt_write_str(&req, "ref-prefix refs/tags/\n");
    pkt_flush(&req);

    HttpResponse resp;
    if (post_command(url, "/git-upload-pack", &req, &resp) != 0) {
        buf_free(&req);
        return -1;
    }
    buf_free(&req);

    if (resp.status != 200) {
        warn_cgc("remote returned HTTP %d while listing refs", resp.status);
        http_response_free(&resp);
        return -1;
    }

    PktReader pr;
    pkt_reader_init(&pr, resp.body.data, resp.body.len);
    const unsigned char *line;
    size_t linelen;
    int kind;
    size_t cap = 0;

    while ((kind = pkt_read(&pr, &line, &linelen)) == PKT_DATA) {
        Ref ref;
        memset(&ref, 0, sizeof(ref));
        int unborn = parse_ref_line(line, linelen, &ref);
        if (unborn < 0) {
            warn_cgc("malformed ref advertisement");
            remote_refs_free(out);
            http_response_free(&resp);
            return -1;
        }
        if (unborn)
            out->empty = 1;

        if (out->count == cap) {
            cap = cap ? cap * 2 : 16;
            out->refs = xrealloc(out->refs, cap * sizeof(Ref));
        }
        out->refs[out->count++] = ref;
    }
    http_response_free(&resp);

    /* Pick the default branch from HEAD. */
    for (size_t i = 0; i < out->count; i++) {
        Ref *r = &out->refs[i];
        if (strcmp(r->name, "HEAD") != 0)
            continue;
        if (r->symref && strncmp(r->symref, "refs/heads/", 11) == 0)
            out->default_ref = xstrdup(r->symref);
        if (!out->empty) {
            memcpy(out->default_oid, r->oid, OID_RAW);
            out->has_default_oid = 1;
        }
    }

    if (!out->default_ref) {
        const char *candidates[] = {"refs/heads/main", "refs/heads/master"};
        for (size_t c = 0; c < 2 && !out->default_ref; c++) {
            for (size_t i = 0; i < out->count; i++) {
                if (strcmp(out->refs[i].name, candidates[c]) == 0) {
                    out->default_ref = xstrdup(candidates[c]);
                    break;
                }
            }
        }
    }
    if (!out->default_ref) {
        for (size_t i = 0; i < out->count; i++) {
            if (strncmp(out->refs[i].name, "refs/heads/", 11) == 0) {
                out->default_ref = xstrdup(out->refs[i].name);
                break;
            }
        }
    }

    if (!out->default_ref && out->empty)
        out->default_ref = xstrdup("refs/heads/main");

    if (out->default_ref) {
        for (size_t i = 0; i < out->count; i++) {
            if (strcmp(out->refs[i].name, out->default_ref) == 0) {
                memcpy(out->default_oid, out->refs[i].oid, OID_RAW);
                out->has_default_oid = 1;
                break;
            }
        }
    }

    return 0;
}

void remote_refs_free(RemoteRefs *r)
{
    for (size_t i = 0; i < r->count; i++) {
        free(r->refs[i].name);
        free(r->refs[i].symref);
    }
    free(r->refs);
    free(r->default_ref);
    memset(r, 0, sizeof(*r));
}

static int oid_seen(const unsigned char (*oids)[OID_RAW], size_t n,
                    const unsigned char oid[OID_RAW])
{
    for (size_t i = 0; i < n; i++)
        if (memcmp(oids[i], oid, OID_RAW) == 0)
            return 1;
    return 0;
}

int protocol_fetch(const char *url, const RemoteRefs *refs, const char *gitdir)
{
    if (refs->empty)
        return 0;

    unsigned char(*wants)[OID_RAW] = xcalloc(refs->count + 1, OID_RAW);
    size_t nwants = 0;

    for (size_t i = 0; i < refs->count; i++) {
        const Ref *r = &refs->refs[i];
        if (strcmp(r->name, "HEAD") == 0)
            continue;
        if (strncmp(r->name, "refs/", 5) != 0)
            continue;
        if (!oid_seen(wants, nwants, r->oid))
            memcpy(wants[nwants++], r->oid, OID_RAW);
    }
    if (nwants == 0 && refs->has_default_oid)
        memcpy(wants[nwants++], refs->default_oid, OID_RAW);
    else if (refs->has_default_oid &&
             !oid_seen(wants, nwants, refs->default_oid))
        memcpy(wants[nwants++], refs->default_oid, OID_RAW);

    if (nwants == 0) {
        free(wants);
        return 0;
    }

    Buf req;
    buf_init(&req);
    pkt_write_str(&req, "command=fetch\n");
    pkt_write_str(&req, "agent=cgc/" CGC_VERSION "\n");
    pkt_write_str(&req, "object-format=sha1\n");
    pkt_delim(&req);
    pkt_write_str(&req, "ofs-delta\n");
    for (size_t i = 0; i < nwants; i++) {
        char hex[OID_HEX + 1];
        oid_hex(wants[i], hex);
        char line[64];
        snprintf(line, sizeof(line), "want %s\n", hex);
        pkt_write_str(&req, line);
    }
    pkt_write_str(&req, "done\n");
    pkt_flush(&req);
    free(wants);

    HttpResponse resp;
    if (post_command(url, "/git-upload-pack", &req, &resp) != 0) {
        buf_free(&req);
        return -1;
    }
    buf_free(&req);

    if (resp.status != 200) {
        warn_cgc("remote returned HTTP %d while fetching", resp.status);
        http_response_free(&resp);
        return -1;
    }

    PktReader pr;
    pkt_reader_init(&pr, resp.body.data, resp.body.len);
    const unsigned char *line;
    size_t linelen;
    int kind;
    int section = 0; /* 0 none, 1 pack, 2 other */
    int sideband = -1;
    Buf pack;
    buf_init(&pack);

    for (;;) {
        kind = pkt_read(&pr, &line, &linelen);
        if (kind == PKT_EOF)
            break;
        if (kind == PKT_FLUSH) {
            if (section == 1)
                break;
            section = 0;
            continue;
        }
        if (kind == PKT_DELIM) {
            section = 0;
            continue;
        }
        if (kind != PKT_DATA)
            continue;

        if (section == 0) {
            size_t l = linelen;
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
                l--;
            if (l == 8 && memcmp(line, "packfile", 8) == 0) {
                section = 1;
                sideband = -1;
            } else if ((l == 15 && memcmp(line, "acknowledgments", 15) == 0) ||
                       (l == 12 && memcmp(line, "shallow-info", 12) == 0) ||
                       (l == 11 && memcmp(line, "wanted-refs", 11) == 0) ||
                       (l == 13 && memcmp(line, "packfile-uris", 13) == 0)) {
                section = 2;
            } else if (linelen >= 4 && memcmp(line, "PACK", 4) == 0) {
                section = 1;
                sideband = 0;
                buf_append(&pack, line, linelen);
            } else {
                /* Tolerate unknown section headers. */
                section = 2;
            }
            continue;
        }

        if (section == 1) {
            if (sideband < 0) {
                if (linelen >= 1 && line[0] >= 1 && line[0] <= 3)
                    sideband = 1;
                else
                    sideband = 0;
            }
            if (sideband) {
                if (linelen < 1)
                    continue;
                int band = line[0];
                const unsigned char *payload = line + 1;
                size_t plen = linelen - 1;
                if (band == 1) {
                    buf_append(&pack, payload, plen);
                } else if (band == 2) {
                    if (!cgc_quiet && isatty(STDERR_FILENO)) {
                        fwrite(payload, 1, plen, stderr);
                        fflush(stderr);
                    }
                } else if (band == 3) {
                    char *msg = trim_newline(payload, plen);
                    warn_cgc("remote error: %s", msg);
                    free(msg);
                    buf_free(&pack);
                    http_response_free(&resp);
                    return -1;
                }
            } else {
                buf_append(&pack, line, linelen);
            }
        }
    }

    /* A flush (or the end of the body) terminates the packfile section. */
    http_response_free(&resp);

    if (pack.len < 12 || memcmp(pack.data, "PACK", 4) != 0) {
        warn_cgc("remote did not send a valid packfile");
        buf_free(&pack);
        return -1;
    }

    int rc = pack_unpack(pack.data, pack.len, gitdir);
    buf_free(&pack);
    if (rc != 0) {
        warn_cgc("failed to unpack packfile");
        return -1;
    }
    return 0;
}
