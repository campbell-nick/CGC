#include "protocol.h"
#include "http.h"
#include "odb.h"
#include "pack.h"
#include "pktline.h"
#include "progress.h"

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
    while (pkt_read(&pr, &line, &linelen) == PKT_DATA) {
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
    if (!tok)
        goto fail;

    if (strcmp(tok, "unborn") == 0) {
        unborn = 1;
        tok = strtok_r(NULL, " ", &save);
        if (!tok)
            goto fail;
    } else {
        if (oid_parse(tok, ref->oid) != 0)
            goto fail;
        tok = strtok_r(NULL, " ", &save);
        if (!tok)
            goto fail;
    }

    ref->name = xstrdup(tok);
    if (ref->name[0] == 0)
        goto fail;
    if (strcmp(ref->name, "HEAD") != 0 && !ref_name_is_valid(ref->name))
        goto fail;

    while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
        if (strncmp(tok, "symref-target:", 14) == 0) {
            if (!ref_name_is_valid(tok + 14))
                goto fail;
            free(ref->symref);
            ref->symref = xstrdup(tok + 14);
        } else if (strncmp(tok, "peeled:", 7) == 0) {
            if (oid_parse(tok + 7, ref->peeled) == 0)
                ref->has_peeled = 1;
        }
    }

    free(s);
    return unborn ? 1 : 0;

fail:
    free(s);
    free(ref->name);
    free(ref->symref);
    ref->name = NULL;
    ref->symref = NULL;
    return -1;
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
    size_t cap = 0;

    while (pkt_read(&pr, &line, &linelen) == PKT_DATA) {
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

typedef struct {
    int section;   /* 0 none, 1 packfile, 2 other */
    int sideband;  /* -1 undecided, 0 raw, 1 sideband */
    int done;
    int failed;
    int shallow_section;
    unsigned char (*shallow)[OID_RAW];
    size_t nshallow;
    size_t capshallow;
    Buf pack;
    Buf pending;
    Buf error;
} FetchDemux;

static int is_section_name(const unsigned char *line, size_t len,
                           const char *name)
{
    size_t l = len;
    while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
        l--;
    return l == strlen(name) && memcmp(line, name, l) == 0;
}

static void demux_line(FetchDemux *d, const unsigned char *line, size_t len)
{
    if (d->section == 0) {
        if (is_section_name(line, len, "packfile")) {
            d->section = 1;
            d->sideband = -1;
            d->shallow_section = 0;
        } else if (is_section_name(line, len, "shallow-info")) {
            d->section = 2;
            d->shallow_section = 1;
        } else if (is_section_name(line, len, "acknowledgments") ||
                   is_section_name(line, len, "wanted-refs") ||
                   is_section_name(line, len, "packfile-uris")) {
            d->section = 2;
            d->shallow_section = 0;
        } else if (len >= 4 && memcmp(line, "PACK", 4) == 0) {
            d->section = 1;
            d->sideband = 0;
            d->shallow_section = 0;
            buf_append(&d->pack, line, len);
            progress_receive_update(d->pack.len);
        } else {
            d->section = 2;
            d->shallow_section = 0;
        }
        return;
    }

    if (d->section == 2) {
        if (d->shallow_section) {
            char *s = trim_newline(line, len);
            const char *p = s;
            if (strncmp(p, "shallow ", 8) == 0)
                p += 8;
            unsigned char oid[OID_RAW];
            if (oid_parse(p, oid) == 0) {
                if (d->nshallow == d->capshallow) {
                    d->capshallow = d->capshallow ? d->capshallow * 2 : 8;
                    d->shallow = xrealloc(d->shallow,
                                          d->capshallow * OID_RAW);
                }
                memcpy(d->shallow[d->nshallow++], oid, OID_RAW);
            }
            free(s);
        }
        return;
    }

    if (d->section != 1)
        return;

    if (d->sideband < 0)
        d->sideband = (len >= 1 && line[0] >= 1 && line[0] <= 3);

    if (!d->sideband) {
        buf_append(&d->pack, line, len);
        progress_receive_update(d->pack.len);
        return;
    }

    if (len < 1)
        return;
    int band = line[0];
    const unsigned char *payload = line + 1;
    size_t plen = len - 1;

    if (band == 1) {
        buf_append(&d->pack, payload, plen);
        progress_receive_update(d->pack.len);
    } else if (band == 2) {
        progress_remote(payload, plen);
    } else {
        progress_clear();
        if (d->error.len)
            buf_putc(&d->error, '\n');
        buf_append(&d->error, payload, plen);
    }
}

static int fetch_feed(void *ud, const void *data, size_t len)
{
    FetchDemux *d = ud;
    if (d->failed || d->done)
        return 0;

    buf_append(&d->pending, data, len);

    size_t pos = 0;
    while (!d->done && !cgc_interrupted && d->pending.len - pos >= 4) {
        const unsigned char *p = d->pending.data + pos;
        int v = 0;
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            int c = p[i];
            int h;
            if (c >= '0' && c <= '9')
                h = c - '0';
            else if (c >= 'a' && c <= 'f')
                h = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                h = c - 'A' + 10;
            else {
                ok = 0;
                break;
            }
            v = (v << 4) | h;
        }
        if (!ok || v == 3) {
            d->failed = 1;
            return -1;
        }
        if (v == 0) {
            if (d->section == 1)
                d->done = 1;
            d->section = 0;
            pos += 4;
            continue;
        }
        if (v == 1 || v == 2) {
            d->section = 0;
            pos += 4;
            continue;
        }
        if (v < 4) {
            d->failed = 1;
            return -1;
        }
        size_t plen = (size_t)v - 4;
        if (d->pending.len - pos - 4 < plen)
            break;
        demux_line(d, p + 4, plen);
        pos += 4 + plen;
    }

    buf_consume(&d->pending, pos);
    if (cgc_interrupted)
        return -1;
    return d->failed ? -1 : 0;
}

static int fetch_request(const char *url, Buf *req, FetchDemux *d,
                         HttpResponse *resp)
{
    char *u = endpoint(url, "/git-upload-pack");
    int rc = http_request_stream("POST", u,
                                 "application/x-git-upload-pack-request",
                                 req->data, req->len,
                                 "Git-Protocol: version=2\r\n"
                                 "Accept: application/x-git-upload-pack-result\r\n",
                                 fetch_feed, d, resp);
    free(u);
    return rc;
}

static void delta_progress(void *ud, uint32_t done, uint32_t total)
{
    (void)ud;
    progress_deltas_update(done, total);
}

int protocol_fetch(const char *url, const RemoteRefs *refs, const char *gitdir,
                   int depth)
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
    if (!progress_live_enabled())
        pkt_write_str(&req, "no-progress\n");
    if (depth > 0) {
        char line[64];
        snprintf(line, sizeof(line), "deepen %d\n", depth);
        pkt_write_str(&req, line);
    }
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

    FetchDemux demux;
    memset(&demux, 0, sizeof(demux));
    demux.section = 0;
    demux.sideband = -1;
    buf_init(&demux.pack);
    buf_init(&demux.pending);
    buf_init(&demux.error);

    HttpResponse resp;
    progress_receive_begin();
    int rc = fetch_request(url, &req, &demux, &resp);
    buf_free(&req);
    if (rc != 0) {
        buf_free(&demux.pack);
        buf_free(&demux.pending);
        buf_free(&demux.error);
        free(demux.shallow);
        return -1;
    }

    progress_remote_end();

    if (resp.status != 200) {
        warn_cgc("remote returned HTTP %d while fetching", resp.status);
        http_response_free(&resp);
        buf_free(&demux.pack);
        buf_free(&demux.pending);
        buf_free(&demux.error);
        free(demux.shallow);
        return -1;
    }

    if (demux.failed || demux.pending.len > 0) {
        warn_cgc("malformed fetch response");
        http_response_free(&resp);
        buf_free(&demux.pack);
        buf_free(&demux.pending);
        buf_free(&demux.error);
        free(demux.shallow);
        return -1;
    }

    if (demux.error.len > 0) {
        char *msg = trim_newline(demux.error.data, demux.error.len);
        warn_cgc("remote error: %s", msg);
        free(msg);
        http_response_free(&resp);
        buf_free(&demux.pack);
        buf_free(&demux.pending);
        buf_free(&demux.error);
        free(demux.shallow);
        return -1;
    }

    http_response_free(&resp);

    Buf pack;
    pack = demux.pack;
    buf_init(&demux.pack);
    buf_free(&demux.pending);
    buf_free(&demux.error);

    if (pack.len < 12 || memcmp(pack.data, "PACK", 4) != 0) {
        warn_cgc("remote did not send a valid packfile");
        buf_free(&pack);
        return -1;
    }

    uint32_t objects = get_be32(pack.data + 8);
    progress_receive_end(objects, pack.len);

    PackStats stats;
    memset(&stats, 0, sizeof(stats));
    rc = pack_unpack(pack.data, pack.len, gitdir, &stats,
                     delta_progress, NULL);
    buf_free(&pack);
    if (rc != 0) {
        progress_clear();
        warn_cgc("failed to unpack packfile");
        /* fall through to cleanup shallow list */
    } else {
        progress_deltas_end(stats.deltas);

        if (depth > 0 && demux.nshallow > 0) {
            char *shallow_path = path_join(gitdir, "shallow");
            Buf sb;
            buf_init(&sb);
            for (size_t i = 0; i < demux.nshallow; i++) {
                char hex[OID_HEX + 1];
                oid_hex(demux.shallow[i], hex);
                buf_appendf(&sb, "%s\n", hex);
            }
            if (write_file(shallow_path, sb.data, sb.len, 0644) != 0)
                warn_cgc("could not write .git/shallow");
            buf_free(&sb);
            free(shallow_path);
        }
    }

    /* cleanup demux shallow list */
    free(demux.shallow);
    demux.shallow = NULL;
    demux.nshallow = 0;
    demux.capshallow = 0;

    if (rc != 0)
        return -1;
    return 0;
}
