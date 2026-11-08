#include "http.h"
#include "url.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

void http_response_free(HttpResponse *r)
{
    buf_free(&r->body);
    free(r->content_type);
    free(r->location);
    memset(r, 0, sizeof(*r));
}

static int tcp_connect(const char *host, int port)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        warn_cgc("cannot resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        warn_cgc("cannot connect to %s:%d: %s", host, port, strerror(errno));
    return fd;
}

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
} Conn;

static int conn_open(Conn *c, const Url *u)
{
    memset(c, 0, sizeof(*c));
    c->fd = tcp_connect(u->host, u->port);
    if (c->fd < 0)
        return -1;

    if (strcmp(u->scheme, "https") != 0)
        return 0;

    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) {
        warn_cgc("cannot create TLS context");
        return -1;
    }
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
    if (!SSL_CTX_set_default_verify_paths(c->ctx)) {
        warn_cgc("cannot load system CA certificates");
        return -1;
    }

    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) {
        warn_cgc("cannot create TLS session");
        return -1;
    }
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, u->host);
    SSL_set1_host(c->ssl, u->host);

    if (SSL_connect(c->ssl) != 1) {
        long vr = SSL_get_verify_result(c->ssl);
        if (vr != X509_V_OK)
            warn_cgc("TLS certificate verification failed: %s",
                     X509_verify_cert_error_string(vr));
        else
            warn_cgc("TLS handshake with %s failed", u->host);
        return -1;
    }
    if (SSL_get_verify_result(c->ssl) != X509_V_OK) {
        warn_cgc("TLS certificate verification failed for %s", u->host);
        return -1;
    }
    return 0;
}

static void conn_close(Conn *c)
{
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ctx)
        SSL_CTX_free(c->ctx);
    if (c->fd >= 0)
        close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static ssize_t conn_read(Conn *c, void *buf, size_t len)
{
    if (c->ssl) {
        int n = SSL_read(c->ssl, buf, (int)len);
        if (n > 0)
            return n;
        int err = SSL_get_error(c->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN)
            return 0;
        if (err == SSL_ERROR_SYSCALL && errno == 0)
            return 0;
        if (cgc_interrupted)
            return -1;
        return -1;
    }
    for (;;) {
        ssize_t n = read(c->fd, buf, len);
        if (n < 0 && errno == EINTR) {
            if (cgc_interrupted)
                return -1;
            continue;
        }
        return n;
    }
}

static int conn_write_all(Conn *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len) {
        int n;
        if (c->ssl) {
            n = SSL_write(c->ssl, p, (int)(len > INT_MAX ? INT_MAX : len));
            if (n <= 0)
                return -1;
        } else {
            ssize_t w = write(c->fd, p, len);
            if (w < 0) {
                if (errno == EINTR) {
                    if (cgc_interrupted)
                        return -1;
                    continue;
                }
                return -1;
            }
            n = (int)w;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int contains_ci(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, n) == 0)
            return 1;
    }
    return 0;
}

static size_t find_header_end(const unsigned char *p, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}

typedef struct {
    int chunked;
    long long content_length;
    size_t body_seen;
    int state;
    size_t chunk_remaining;
    int done;
    Buf pending;
    http_body_fn sink;
    void *ud;
} BodyDecoder;

enum {
    CHUNK_SIZE,
    CHUNK_DATA,
    CHUNK_CRLF,
    CHUNK_TRAILER,
};

static int append_sink(void *ud, const void *data, size_t len)
{
    buf_append((Buf *)ud, data, len);
    return 0;
}

static int discard_sink(void *ud, const void *data, size_t len)
{
    (void)ud;
    (void)data;
    (void)len;
    return 0;
}

/* Feed raw body bytes to the decoder, which strips Transfer-Encoding chunk
 * framing and hands payload to sink in order. Returns 0 or -1 if malformed. */
static int decoder_feed(BodyDecoder *d, const unsigned char *in, size_t len)
{
    if (d->done || len == 0)
        return 0;

    if (!d->chunked) {
        size_t take = len;
        if (d->content_length >= 0) {
            size_t left = (size_t)d->content_length - d->body_seen;
            if (take > left)
                take = left;
        }
        if (take && d->sink(d->ud, in, take) != 0)
            return -1;
        d->body_seen += take;
        if (d->content_length >= 0 &&
            d->body_seen >= (size_t)d->content_length)
            d->done = 1;
        return 0;
    }

    buf_append(&d->pending, in, len);
    size_t pos = 0;

    while (pos < d->pending.len) {
        if (d->state == CHUNK_SIZE) {
            size_t e = pos;
            while (e + 1 < d->pending.len &&
                   !(d->pending.data[e] == '\r' &&
                     d->pending.data[e + 1] == '\n'))
                e++;
            if (e + 1 >= d->pending.len)
                break;

            size_t size = 0;
            int digits = 0;
            for (size_t q = pos; q < e; q++) {
                int c = d->pending.data[q];
                int v;
                if (c >= '0' && c <= '9')
                    v = c - '0';
                else if (c >= 'a' && c <= 'f')
                    v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    v = c - 'A' + 10;
                else
                    break;
                size = size * 16 + (size_t)v;
                digits = 1;
            }
            if (!digits || size > (1u << 30))
                return -1;

            pos = e + 2;
            d->chunk_remaining = size;
            d->state = size ? CHUNK_DATA : CHUNK_TRAILER;
        } else if (d->state == CHUNK_DATA) {
            size_t avail = d->pending.len - pos;
            size_t take = avail < d->chunk_remaining ? avail : d->chunk_remaining;
            if (take && d->sink(d->ud, d->pending.data + pos, take) != 0)
                return -1;
            pos += take;
            d->body_seen += take;
            d->chunk_remaining -= take;
            if (d->chunk_remaining)
                break;
            d->state = CHUNK_CRLF;
        } else if (d->state == CHUNK_CRLF) {
            if (d->pending.len - pos < 2)
                break;
            if (d->pending.data[pos] != '\r' ||
                d->pending.data[pos + 1] != '\n')
                return -1;
            pos += 2;
            d->state = CHUNK_SIZE;
        } else {
            size_t e = pos;
            while (e + 1 < d->pending.len &&
                   !(d->pending.data[e] == '\r' &&
                     d->pending.data[e + 1] == '\n'))
                e++;
            if (e + 1 >= d->pending.len)
                break;
            int blank = (e == pos);
            pos = e + 2;
            if (blank) {
                d->done = 1;
                break;
            }
        }
    }

    buf_consume(&d->pending, pos);
    return 0;
}

static char *header_value(char *headers, const char *name)
{
    char *line = headers;
    while (line && *line) {
        char *eol = strstr(line, "\r\n");
        if (!eol)
            break;
        *eol = 0;
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = 0;
            if (strcasecmp(line, name) == 0) {
                char *v = colon + 1;
                while (*v == ' ' || *v == '\t')
                    v++;
                char *end = v + strlen(v);
                while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
                    *--end = 0;
                char *dup = xstrdup(v);
                *colon = ':';
                *eol = '\r';
                return dup;
            }
            *colon = ':';
        }
        *eol = '\r';
        line = eol + 2;
    }
    return NULL;
}

static char *resolve_location(const Url *base, const char *loc)
{
    if (strstr(loc, "://"))
        return xstrdup(loc);

    Buf b;
    buf_init(&b);
    if (loc[0] == '/' && loc[1] == '/') {
        buf_appendf(&b, "%s:%s", base->scheme, loc);
        return (char *)b.data;
    }

    int default_port = strcmp(base->scheme, "https") == 0 ? 443 : 80;
    if (base->port == default_port)
        buf_appendf(&b, "%s://%s", base->scheme, base->host);
    else
        buf_appendf(&b, "%s://%s:%d", base->scheme, base->host, base->port);

    if (loc[0] == '/') {
        buf_append(&b, loc, strlen(loc));
    } else {
        const char *slash = strrchr(base->path, '/');
        if (slash)
            buf_append(&b, base->path, (size_t)(slash - base->path) + 1);
        else
            buf_putc(&b, '/');
        buf_append(&b, loc, strlen(loc));
    }
    return (char *)b.data;
}

static int request_once(const char *method, const char *url,
                        const char *content_type,
                        const void *body, size_t bodylen,
                        const char *extra_headers,
                        http_body_fn on_body, void *ud,
                        HttpResponse *resp)
{
    Url u;
    if (url_parse(url, &u) != 0) {
        warn_cgc("invalid URL: %s", url);
        return -1;
    }

    Conn c;
    if (conn_open(&c, &u) != 0) {
        conn_close(&c);
        url_free(&u);
        return -1;
    }

    int default_port = strcmp(u.scheme, "https") == 0 ? 443 : 80;
    Buf req;
    buf_init(&req);
    buf_appendf(&req, "%s %s HTTP/1.1\r\n", method, u.path);
    if (u.port == default_port)
        buf_appendf(&req, "Host: %s\r\n", u.host);
    else
        buf_appendf(&req, "Host: %s:%d\r\n", u.host, u.port);
    buf_appendf(&req, "User-Agent: cgc/%s\r\n", CGC_VERSION);
    buf_append(&req, "Accept: */*\r\n", strlen("Accept: */*\r\n"));
    buf_append(&req, "Accept-Encoding: identity\r\n", strlen("Accept-Encoding: identity\r\n"));
    buf_append(&req, "Connection: close\r\n", strlen("Connection: close\r\n"));
    if (extra_headers) {
        buf_append(&req, extra_headers, strlen(extra_headers));
        if (req.len >= 2 && !(req.data[req.len - 2] == '\r' && req.data[req.len - 1] == '\n'))
            buf_append(&req, "\r\n", 2);
    }
    if (content_type)
        buf_appendf(&req, "Content-Type: %s\r\n", content_type);
    if (bodylen)
        buf_appendf(&req, "Content-Length: %zu\r\n", bodylen);
    buf_append(&req, "\r\n", 2);
    if (bodylen)
        buf_append(&req, body, bodylen);

    if (conn_write_all(&c, req.data, req.len) != 0) {
        warn_cgc("failed to send request to %s", u.host);
        buf_free(&req);
        conn_close(&c);
        url_free(&u);
        return -1;
    }
    buf_free(&req);

    Buf raw;
    buf_init(&raw);
    buf_init(&resp->body);

    size_t hdr_end = 0;
    int status = 0;
    long long content_length = -1;
    int chunked = 0;
    char *content_type_resp = NULL;
    char *location = NULL;
    int failed = 0;

    BodyDecoder dec;
    memset(&dec, 0, sizeof(dec));
    buf_init(&dec.pending);
    dec.sink = append_sink;
    dec.ud = &resp->body;

    for (;;) {
        buf_reserve(&raw, 16384);
        ssize_t n = conn_read(&c, raw.data + raw.len, raw.cap - raw.len - 1);
        if (n < 0) {
            warn_cgc("error reading response from %s", u.host);
            failed = 1;
            break;
        }

        if (n == 0) {
            if (!hdr_end) {
                warn_cgc("connection closed before HTTP headers arrived");
                failed = 1;
            }
            break;
        }
        raw.len += (size_t)n;
        raw.data[raw.len] = 0;

        if (!hdr_end) {
            hdr_end = find_header_end(raw.data, raw.len);
            if (!hdr_end)
                continue;

            char *headers = xstrndup((char *)raw.data, hdr_end);
            char *line_end = strstr(headers, "\r\n");
            if (!line_end || sscanf(headers, "HTTP/%*d.%*d %d", &status) != 1) {
                warn_cgc("malformed HTTP response from %s", u.host);
                free(headers);
                failed = 1;
                break;
            }
            *line_end = 0;
            content_type_resp = header_value(line_end + 2, "Content-Type");
            char *cl = header_value(line_end + 2, "Content-Length");
            char *te = header_value(line_end + 2, "Transfer-Encoding");
            location = header_value(line_end + 2, "Location");
            if (te && contains_ci(te, "chunked"))
                chunked = 1;
            if (cl) {
                char *end = NULL;
                long long v = strtoll(cl, &end, 10);
                if (end != cl && v >= 0)
                    content_length = v;
            }
            free(cl);
            free(te);
            free(headers);

            dec.chunked = chunked;
            dec.content_length = content_length;
            dec.state = CHUNK_SIZE;
            if (!chunked && content_length == 0)
                dec.done = 1;

            if (on_body) {
                if (status == 200) {
                    dec.sink = on_body;
                    dec.ud = ud;
                } else {
                    dec.sink = discard_sink;
                    dec.ud = NULL;
                }
            }
        }

        size_t have = raw.len - hdr_end;
        if (have > 0) {
            if (decoder_feed(&dec, raw.data + hdr_end, have) != 0) {
                warn_cgc("malformed response body from %s", u.host);
                failed = 1;
                break;
            }
            raw.len = hdr_end;
        }
        if (dec.done)
            break;
    }

    if (!failed && hdr_end && !dec.done) {
        if (dec.chunked) {
            warn_cgc("truncated chunked response from %s", u.host);
            failed = 1;
        } else if (dec.content_length >= 0 &&
                   dec.body_seen < (size_t)dec.content_length) {
            warn_cgc("truncated response from %s", u.host);
            failed = 1;
        }
    }

    buf_free(&dec.pending);
    buf_free(&raw);
    conn_close(&c);
    url_free(&u);

    if (failed) {
        buf_free(&resp->body);
        free(content_type_resp);
        free(location);
        return -1;
    }

    resp->status = status;
    resp->content_type = content_type_resp;
    resp->location = location;
    return 0;
}

static int do_request(const char *method, const char *url,
                      const char *content_type,
                      const void *body, size_t bodylen,
                      const char *extra_headers,
                      http_body_fn on_body, void *ud,
                      HttpResponse *resp)
{
    memset(resp, 0, sizeof(*resp));
    char *current = xstrdup(url);
    char *cur_method = xstrdup(method);
    const void *cur_body = body;
    size_t cur_bodylen = bodylen;

    int rc = -1;
    for (int redirects = 0; redirects <= 5; redirects++) {
        HttpResponse r;
        memset(&r, 0, sizeof(r));
        if (request_once(cur_method, current, content_type,
                         cur_body, cur_bodylen, extra_headers,
                         on_body, ud, &r) != 0) {
            http_response_free(&r);
            goto out;
        }

        if (r.status == 301 || r.status == 302 || r.status == 303 ||
            r.status == 307 || r.status == 308) {
            if (redirects == 5 || !r.location) {
                warn_cgc("too many redirects or missing Location header");
                http_response_free(&r);
                goto out;
            }
            Url base;
            if (url_parse(current, &base) != 0) {
                http_response_free(&r);
                goto out;
            }
            char *next = resolve_location(&base, r.location);
            url_free(&base);

            if (r.status == 303 ||
                ((r.status == 301 || r.status == 302) &&
                 strcmp(cur_method, "POST") == 0)) {
                free(cur_method);
                cur_method = xstrdup("GET");
                cur_body = NULL;
                cur_bodylen = 0;
            }

            free(current);
            current = next;
            http_response_free(&r);
            continue;
        }

        *resp = r;
        rc = 0;
        goto out;
    }

out:
    free(current);
    free(cur_method);
    return rc;
}

int http_request(const char *method, const char *url,
                 const char *content_type,
                 const void *body, size_t bodylen,
                 const char *extra_headers,
                 HttpResponse *resp)
{
    return do_request(method, url, content_type, body, bodylen,
                      extra_headers, NULL, NULL, resp);
}

int http_request_stream(const char *method, const char *url,
                        const char *content_type,
                        const void *body, size_t bodylen,
                        const char *extra_headers,
                        http_body_fn on_body, void *ud,
                        HttpResponse *resp)
{
    return do_request(method, url, content_type, body, bodylen,
                      extra_headers, on_body, ud, resp);
}
