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

static Ref *ref_find(RemoteRefs *refs, const char *name)
{
    for (size_t i = 0; i < refs->count; i++)
        if (strcmp(refs->refs[i].name, name) == 0)
            return &refs->refs[i];
    return NULL;
}

static Ref *ref_add(RemoteRefs *refs, const char *name, size_t *cap)
{
    if (refs->count == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        refs->refs = xrealloc(refs->refs, *cap * sizeof(Ref));
    }
    Ref *ref = &refs->refs[refs->count++];
    memset(ref, 0, sizeof(*ref));
    ref->name = xstrdup(name);
    return ref;
}

static int oid_is_zero(const unsigned char oid[OID_RAW])
{
    for (size_t i = 0; i < OID_RAW; i++)
        if (oid[i])
            return 0;
    return 1;
}

/* Parse the capability list from a v0 ref advertisement. The capabilities
 * appear after a NUL byte on the first advertised ref. */
static int parse_v0_caps(RemoteRefs *out, const char *caps, char **symref_head)
{
    char *copy = xstrdup(caps);
    size_t len = strlen(copy);
    while (len > 0 && (copy[len - 1] == '\n' || copy[len - 1] == '\r'))
        copy[--len] = '\0';

    int rc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, " ", &save); tok;
         tok = strtok_r(NULL, " ", &save)) {
        if (strcmp(tok, "side-band-64k") == 0) {
            out->sideband = 1;
            out->sideband_64k = 1;
        } else if (strcmp(tok, "side-band") == 0) {
            out->sideband = 1;
        } else if (strcmp(tok, "ofs-delta") == 0) {
            out->ofs_delta = 1;
        } else if (strcmp(tok, "no-progress") == 0) {
            out->no_progress = 1;
        } else if (strcmp(tok, "shallow") == 0) {
            out->shallow = 1;
        } else if (strncmp(tok, "symref=HEAD:", 12) == 0) {
            const char *target = tok + 12;
            if (ref_name_is_valid(target)) {
                free(*symref_head);
                *symref_head = xstrdup(target);
            }
        } else if (strncmp(tok, "object-format=", 14) == 0) {
            if (strcmp(tok + 14, "sha1") != 0) {
                warn_cgc("only SHA-1 repositories are supported");
                rc = -1;
            }
        }
    }

    free(copy);
    return rc;
}

/* Parse a v0 ref advertisement into the same shape used by protocol v2. */
static int parse_v0_refs(PktReader *pr, RemoteRefs *out, int all_refs)
{
    const unsigned char *line;
    size_t linelen;
    char *symref_head = NULL;
    size_t cap = 0;
    int first = 1;

    while (pkt_read(pr, &line, &linelen) == PKT_DATA) {
        size_t namelen = linelen;
        const unsigned char *nul = memchr(line, '\0', linelen);
        if (nul) {
            namelen = (size_t)(nul - line);
            if (first) {
                char *caps = xstrndup((const char *)nul + 1,
                                      linelen - namelen - 1);
                int rc = parse_v0_caps(out, caps, &symref_head);
                free(caps);
                if (rc != 0)
                    goto fail;
            }
        }
        first = 0;

        while (namelen > 0 &&
               (line[namelen - 1] == '\n' || line[namelen - 1] == '\r'))
            namelen--;
        if (namelen < OID_HEX + 2 || line[OID_HEX] != ' ')
            goto fail;

        char hex[OID_HEX + 1];
        memcpy(hex, line, OID_HEX);
        hex[OID_HEX] = '\0';
        unsigned char oid[OID_RAW];
        if (oid_parse(hex, oid) != 0)
            goto fail;

        char *name = xstrndup((const char *)line + OID_HEX + 1,
                              namelen - OID_HEX - 1);
        size_t n = strlen(name);

        if (strcmp(name, "capabilities^{}") == 0) {
            out->empty = 1;
            free(name);
            continue;
        }

        if (n > 3 && strcmp(name + n - 3, "^{}") == 0) {
            name[n - 3] = '\0';
            Ref *tag = ref_find(out, name);
            if (tag) {
                memcpy(tag->peeled, oid, OID_RAW);
                tag->has_peeled = 1;
            }
            free(name);
            continue;
        }

        if (strcmp(name, "HEAD") != 0 &&
            (!ref_name_is_valid(name) ||
             (!all_refs && strncmp(name, "refs/heads/", 11) != 0 &&
              strncmp(name, "refs/tags/", 10) != 0))) {
            free(name);
            continue;
        }

        Ref *ref = ref_add(out, name, &cap);
        memcpy(ref->oid, oid, OID_RAW);
        free(name);
    }

    Ref *head = ref_find(out, "HEAD");
    if (head && oid_is_zero(head->oid))
        out->empty = 1;
    if (symref_head) {
        if (!head) {
            /* An empty repository advertises only capabilities^{}, so create
             * a synthetic HEAD to carry the symref for the default branch. */
            head = ref_add(out, "HEAD", &cap);
        }
        free(head->symref);
        head->symref = symref_head;
    }
    return 0;

fail:
    free(symref_head);
    return -1;
}

static int discover(const char *url, int *unborn_ok, RemoteRefs *out,
                    int all_refs)
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

    if (kind == PKT_DATA && linelen >= 9 &&
        memcmp(line, "version 2", 9) == 0) {
        /* Drain the v2 capability advertisement. */
        while (pkt_read(&pr, &line, &linelen) == PKT_DATA) {
            if (linelen >= 7 && memcmp(line, "ls-refs", 7) == 0 &&
                line_has(line, linelen, "unborn"))
                *unborn_ok = 1;
            if (linelen >= 14 && memcmp(line, "object-format=", 14) == 0) {
                size_t rest = linelen - 14;
                while (rest > 0 &&
                       (line[14 + rest - 1] == '\n' ||
                        line[14 + rest - 1] == '\r'))
                    rest--;
                if (rest != 4 || memcmp(line + 14, "sha1", 4) != 0) {
                    warn_cgc("only SHA-1 repositories are supported");
                    http_response_free(&resp);
                    return -1;
                }
            }
        }
        http_response_free(&resp);
        return 0;
    }

    /* No v2 banner: fall back to the protocol v0 ref advertisement. */
    if (kind == PKT_DATA) {
        pr.pos -= 4 + linelen;
        if (parse_v0_refs(&pr, out, all_refs) != 0) {
            warn_cgc("malformed ref advertisement");
            http_response_free(&resp);
            return -1;
        }
        out->protocol_v0 = 1;
        http_response_free(&resp);
        return 1;
    }

    if (kind == PKT_FLUSH || kind == PKT_EOF) {
        out->empty = 1;
        out->protocol_v0 = 1;
        http_response_free(&resp);
        return 1;
    }

    http_response_free(&resp);
    warn_cgc("unexpected ref advertisement");
    return -1;
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

static void select_default_ref(RemoteRefs *out)
{
    /* Pick the default branch from HEAD. */
    for (size_t i = 0; i < out->count; i++) {
        Ref *r = &out->refs[i];
        if (strcmp(r->name, "HEAD") != 0)
            continue;
        if (r->symref && strncmp(r->symref, "refs/heads/", 11) == 0)
            out->default_ref = xstrdup(r->symref);
        if (!out->empty && !oid_is_zero(r->oid)) {
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
}

int protocol_ls_refs(const char *url, int all_refs, RemoteRefs *out)
{
    memset(out, 0, sizeof(*out));

    int unborn_ok = 0;
    int version = discover(url, &unborn_ok, out, all_refs);
    if (version < 0)
        return -1;
    if (version == 1) {
        select_default_ref(out);
        return 0;
    }

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
    if (all_refs) {
        pkt_write_str(&req, "ref-prefix refs/\n");
    } else {
        pkt_write_str(&req, "ref-prefix refs/heads/\n");
        pkt_write_str(&req, "ref-prefix refs/tags/\n");
    }
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

    select_default_ref(out);
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

static void write_shallow_file(const char *gitdir,
                               const unsigned char (*shallow)[OID_RAW],
                               size_t nshallow)
{
    char *path = path_join(gitdir, "shallow");
    Buf sb;
    buf_init(&sb);
    for (size_t i = 0; i < nshallow; i++) {
        char hex[OID_HEX + 1];
        oid_hex(shallow[i], hex);
        buf_appendf(&sb, "%s\n", hex);
    }
    if (write_file(path, sb.data, sb.len, 0644) != 0)
        warn_cgc("could not write .git/shallow");
    buf_free(&sb);
    free(path);
}

/* Validate and unpack a received pack, then record shallow boundaries. */
static int finish_pack(Buf *pack, const char *gitdir, int depth,
                       const unsigned char (*shallow)[OID_RAW],
                       size_t nshallow)
{
    if (pack->len == 0) {
        /* The server had nothing to send because every want was already
         * reachable from a have. This is success, not a malformed response. */
        progress_clear();
        return 0;
    }

    if (pack->len < 12 || memcmp(pack->data, "PACK", 4) != 0) {
        warn_cgc("remote did not send a valid packfile");
        return -1;
    }

    uint32_t objects = get_be32(pack->data + 8);
    if (objects == 0) {
        /* An empty pack (header plus trailer) means there was nothing new.
         * Do not print a receive/delta summary for a no-op fetch. */
        progress_clear();
        return 0;
    }
    progress_receive_end(objects, pack->len);

    PackStats stats;
    memset(&stats, 0, sizeof(stats));
    int rc = pack_unpack(pack->data, pack->len, gitdir, &stats,
                         delta_progress, NULL);
    if (rc != 0) {
        progress_clear();
        warn_cgc("failed to unpack packfile");
        return -1;
    }

    progress_deltas_end(stats.deltas);
    if (depth > 0 && nshallow > 0)
        write_shallow_file(gitdir, shallow, nshallow);
    return 0;
}

static int collect_wants(const RemoteRefs *refs,
                         unsigned char (**out)[OID_RAW], size_t *out_count)
{
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
    /* The default branch is normally part of the ref list, so its tip is
     * already a want. Only fall back to default_oid when no refs were listed
     * (for example a HEAD-only advertisement); adding it after filtering out
     * the default branch would fetch history the caller did not ask for. */
    if (nwants == 0 && refs->has_default_oid)
        memcpy(wants[nwants++], refs->default_oid, OID_RAW);

    *out = wants;
    *out_count = nwants;
    return 0;
}

typedef struct {
    int sideband;
    int raw;
    int done;
    int failed;
    int got_ack;
    Buf pack;
    Buf pending;
    Buf error;
    unsigned char (*shallow)[OID_RAW];
    size_t nshallow;
    size_t capshallow;
} FetchV0;

static void v0_add_shallow(FetchV0 *d, const unsigned char oid[OID_RAW])
{
    if (d->nshallow == d->capshallow) {
        d->capshallow = d->capshallow ? d->capshallow * 2 : 8;
        d->shallow = xrealloc(d->shallow, d->capshallow * OID_RAW);
    }
    memcpy(d->shallow[d->nshallow++], oid, OID_RAW);
}

static void v0_demux_line(FetchV0 *d, const unsigned char *line, size_t len)
{
    size_t l = len;
    while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
        l--;

    if (l > 8 && memcmp(line, "shallow ", 8) == 0) {
        char *s = xstrndup((const char *)line + 8, l - 8);
        unsigned char oid[OID_RAW];
        if (oid_parse(s, oid) == 0)
            v0_add_shallow(d, oid);
        free(s);
        return;
    }
    if (l > 10 && memcmp(line, "unshallow ", 10) == 0)
        return;

    if ((l == 3 && memcmp(line, "NAK", 3) == 0) ||
        (l >= 4 && memcmp(line, "ACK ", 4) == 0)) {
        d->got_ack = 1;
        return;
    }

    if (!d->sideband)
        return; /* raw pack bytes are detected by framing. */

    if (len < 1)
        return;
    int band = line[0];
    if (band < 1 || band > 3)
        return;

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

static int fetch_feed_v0(void *ud, const void *data, size_t len)
{
    FetchV0 *d = ud;
    if (d->failed || d->done)
        return 0;

    buf_append(&d->pending, data, len);

    size_t pos = 0;
    while (!d->done && !cgc_interrupted) {
        if (d->raw) {
            buf_append(&d->pack, d->pending.data + pos,
                       d->pending.len - pos);
            progress_receive_update(d->pack.len);
            pos = d->pending.len;
            break;
        }
        if (d->pending.len - pos < 4)
            break;

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

        if (!ok) {
            if (d->sideband) {
                d->failed = 1;
                return -1;
            }
            /* Without side-band, the packfile follows the ACK/NAK lines as
             * raw bytes rather than pkt-lines. */
            d->raw = 1;
            buf_append(&d->pack, p, d->pending.len - pos);
            progress_receive_update(d->pack.len);
            pos = d->pending.len;
            break;
        }
        if (v == 3) {
            d->failed = 1;
            return -1;
        }
        if (v == 0) {
            /* A flush before any ACK/NAK or pack data terminates the shallow
             * negotiation section, not the response. */
            if (d->got_ack || d->pack.len > 0)
                d->done = 1;
            pos += 4;
            continue;
        }
        if (v == 1 || v == 2) {
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
        v0_demux_line(d, p + 4, plen);
        pos += 4 + plen;
    }

    buf_consume(&d->pending, pos);
    if (cgc_interrupted)
        return -1;
    return d->failed ? -1 : 0;
}

static int fetch_request_v0(const char *url, Buf *req, FetchV0 *d,
                            HttpResponse *resp)
{
    char *u = endpoint(url, "/git-upload-pack");
    int rc = http_request_stream("POST", u,
                                 "application/x-git-upload-pack-request",
                                 req->data, req->len,
                                 "Accept: application/x-git-upload-pack-result\r\n",
                                 fetch_feed_v0, d, resp);
    free(u);
    return rc;
}

static int protocol_fetch_v0(const char *url, const RemoteRefs *refs,
                             const unsigned char (*wants)[OID_RAW],
                             size_t nwants, const char *gitdir, int depth,
                             const unsigned char (*haves)[OID_RAW],
                             size_t nhaves)
{
    if (depth > 0 && !refs->shallow) {
        warn_cgc("remote does not support shallow fetches");
        return -1;
    }

    Buf caps;
    buf_init(&caps);
    if (refs->ofs_delta)
        buf_appendf(&caps, " ofs-delta");
    if (refs->sideband_64k)
        buf_appendf(&caps, " side-band-64k");
    else if (refs->sideband)
        buf_appendf(&caps, " side-band");
    if (!progress_live_enabled() && refs->no_progress)
        buf_appendf(&caps, " no-progress");
    buf_appendf(&caps, " agent=cgc/" CGC_VERSION);

    Buf req;
    buf_init(&req);
    for (size_t i = 0; i < nwants; i++) {
        char hex[OID_HEX + 1];
        oid_hex(wants[i], hex);
        Buf line;
        buf_init(&line);
        buf_appendf(&line, "want %s%s\n", hex,
                    i == 0 ? (char *)caps.data : "");
        pkt_write_str(&req, (char *)line.data);
        buf_free(&line);
    }
    if (depth > 0) {
        char line[64];
        snprintf(line, sizeof(line), "deepen %d\n", depth);
        pkt_write_str(&req, line);
    }

    /* The want/deepen section ends with a flush; haves and done follow in a
     * second section. Stateless HTTP servers reject the request otherwise. */
    pkt_flush(&req);

    for (size_t i = 0; i < nhaves; i++) {
        char hex[OID_HEX + 1];
        oid_hex(haves[i], hex);
        char line[64];
        snprintf(line, sizeof(line), "have %s\n", hex);
        pkt_write_str(&req, line);
    }
    pkt_write_str(&req, "done\n");
    buf_free(&caps);

    FetchV0 demux;
    memset(&demux, 0, sizeof(demux));
    demux.sideband = refs->sideband;
    buf_init(&demux.pack);
    buf_init(&demux.pending);
    buf_init(&demux.error);

    HttpResponse resp;
    progress_receive_begin();
    int rc = fetch_request_v0(url, &req, &demux, &resp);
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

    Buf pack = demux.pack;
    buf_init(&demux.pack);
    buf_free(&demux.pending);
    buf_free(&demux.error);

    rc = finish_pack(&pack, gitdir, depth, demux.shallow, demux.nshallow);
    buf_free(&pack);
    free(demux.shallow);
    return rc;
}

int protocol_fetch(const char *url, const RemoteRefs *refs, const char *gitdir,
                   int depth, const unsigned char (*haves)[OID_RAW],
                   size_t nhaves)
{
    if (refs->empty)
        return 0;

    unsigned char(*wants)[OID_RAW];
    size_t nwants = 0;
    collect_wants(refs, &wants, &nwants);
    if (nwants == 0) {
        free(wants);
        return 0;
    }

    if (refs->protocol_v0) {
        int rc = protocol_fetch_v0(url, refs, wants, nwants, gitdir, depth,
                                   haves, nhaves);
        free(wants);
        return rc;
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
    for (size_t i = 0; i < nhaves; i++) {
        char hex[OID_HEX + 1];
        oid_hex(haves[i], hex);
        char line[64];
        snprintf(line, sizeof(line), "have %s\n", hex);
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

    rc = finish_pack(&pack, gitdir, depth, demux.shallow, demux.nshallow);
    buf_free(&pack);
    free(demux.shallow);
    return rc;
}
