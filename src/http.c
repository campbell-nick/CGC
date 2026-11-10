#include "http.h"
#include "url.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

void http_response_free(HttpResponse *r)
{
    buf_free(&r->body);
    free(r->reason);
    free(r->content_type);
    free(r->location);
    memset(r, 0, sizeof(*r));
}

static char *g_http_token = NULL;

void http_set_token(const char *token)
{
    free(g_http_token);
    g_http_token = token && *token ? xstrdup(token) : NULL;
}

#define HTTP_DEFAULT_TIMEOUT_SECONDS 60
#define HTTP_DEFAULT_RETRIES 2
#define HTTP_MAX_RETRY_DELAY_MS 4000

static long env_long(const char *name, long def, long min, long max)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return def;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return def;
    return v;
}

static int http_timeout_ms(void)
{
    long seconds = env_long("CGC_HTTP_TIMEOUT", HTTP_DEFAULT_TIMEOUT_SECONDS,
                            1, 3600);
    return (int)(seconds * 1000);
}

static int http_retries(void)
{
    return (int)env_long("CGC_HTTP_RETRIES", HTTP_DEFAULT_RETRIES, 0, 10);
}

typedef struct {
    int enabled;
    char *host;
    int port;
    char *auth; /* base64-encoded user:password, or NULL */
} Proxy;

static void proxy_free(Proxy *p)
{
    free(p->host);
    free(p->auth);
    memset(p, 0, sizeof(*p));
}

static int host_matches_no_proxy(const char *host, int port, const char *token)
{
    char *copy = xstrdup(token);
    char *p = copy;
    while (*p && isspace((unsigned char)*p))
        p++;
    char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1]))
        *--end = 0;

    if (strcmp(p, "*") == 0) {
        free(copy);
        return 1;
    }

    int token_port = 0;
    char *colon = strrchr(p, ':');
    if (colon && colon[1]) {
        char *num_end = NULL;
        long v = strtol(colon + 1, &num_end, 10);
        if (num_end && *num_end == '\0' && v > 0 && v <= 65535) {
            token_port = (int)v;
            *colon = 0;
        }
    }
    if (token_port && token_port != port) {
        free(copy);
        return 0;
    }

    while (p[0] == '.' || (p[0] == '*' && p[1] == '.'))
        p += p[0] == '*' ? 2 : 1;
    if (!*p) {
        free(copy);
        return 0;
    }

    size_t n = strlen(p);
    size_t hn = strlen(host);
    int match = strcasecmp(host, p) == 0 ||
                (hn > n && host[hn - n - 1] == '.' &&
                 strcasecmp(host + hn - n, p) == 0);
    free(copy);
    return match;
}

static int no_proxy_matches(const char *host, int port)
{
    const char *env = getenv("no_proxy");
    if (!env || !*env)
        env = getenv("NO_PROXY");
    if (!env || !*env)
        return 0;

    char *copy = xstrdup(env);
    char *save = NULL;
    int match = 0;
    for (char *tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        if (host_matches_no_proxy(host, port, tok)) {
            match = 1;
            break;
        }
    }
    free(copy);
    return match;
}

static int proxy_parse(const char *value, Proxy *p)
{
    const char *s = value;
    const char *sep = strstr(s, "://");
    if (sep) {
        char *scheme = xstrndup(s, (size_t)(sep - s));
        int ok = strcasecmp(scheme, "http") == 0;
        free(scheme);
        if (!ok) {
            warn_cgc("unsupported proxy scheme in '%s' "
                     "(only http proxies are supported)", value);
            return -1;
        }
        s = sep + 3;
    }

    const char *end = s + strlen(s);
    const char *slash = strpbrk(s, "/?#");
    if (slash)
        end = slash;

    const char *at = memchr(s, '@', (size_t)(end - s));
    char *auth = at ? xstrndup(s, (size_t)(at - s)) : NULL;
    if (at)
        s = at + 1;

    char *host = NULL;
    int port = 80;
    if (s < end && *s == '[') {
        const char *close = memchr(s, ']', (size_t)(end - s));
        if (!close) {
            free(auth);
            return -1;
        }
        host = xstrndup(s + 1, (size_t)(close - s - 1));
        if (close + 1 < end && close[1] == ':') {
            char *num_end = NULL;
            long v = strtol(close + 2, &num_end, 10);
            if (num_end != close + 2 && num_end == end && v > 0 && v <= 65535)
                port = (int)v;
            else {
                free(host);
                free(auth);
                return -1;
            }
        }
    } else {
        const char *colon = memchr(s, ':', (size_t)(end - s));
        const char *host_end = colon ? colon : end;
        if (host_end == s) {
            free(auth);
            return -1;
        }
        host = xstrndup(s, (size_t)(host_end - s));
        if (colon) {
            char *num_end = NULL;
            long v = strtol(colon + 1, &num_end, 10);
            if (num_end != colon + 1 && num_end == end && v > 0 && v <= 65535)
                port = (int)v;
            else {
                free(host);
                free(auth);
                return -1;
            }
        }
    }

    if (!*host) {
        free(host);
        free(auth);
        return -1;
    }

    p->host = host;
    p->port = port;
    if (auth && *auth) {
        size_t n = 4 * ((strlen(auth) + 2) / 3) + 1;
        char *b64 = xmalloc(n);
        int encoded = EVP_EncodeBlock((unsigned char *)b64,
                                      (const unsigned char *)auth,
                                      (int)strlen(auth));
        b64[encoded] = '\0';
        p->auth = b64;
    }
    free(auth);
    p->enabled = 1;
    return 0;
}

static int proxy_for(const Url *u, Proxy *p)
{
    memset(p, 0, sizeof(*p));

    const char *env = NULL;
    if (strcmp(u->scheme, "https") == 0) {
        env = getenv("https_proxy");
        if (!env || !*env)
            env = getenv("HTTPS_PROXY");
    } else {
        env = getenv("http_proxy");
        if (!env || !*env)
            env = getenv("HTTP_PROXY");
    }
    if (!env || !*env) {
        env = getenv("all_proxy");
        if (!env || !*env)
            env = getenv("ALL_PROXY");
    }
    if (!env || !*env || no_proxy_matches(u->host, u->port))
        return 0;

    return proxy_parse(env, p) == 0 ? 1 : 0;
}

static void set_socket_timeouts(int fd)
{
    int ms = http_timeout_ms();
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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
    int timeout = http_timeout_ms();
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        int connected = connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr;
            do {
                pr = poll(&pfd, 1, timeout);
            } while (pr < 0 && errno == EINTR && !cgc_interrupted);

            if (pr > 0) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 &&
                    err == 0) {
                    connected = 1;
                } else {
                    errno = err ? err : ECONNREFUSED;
                }
            } else if (pr == 0) {
                errno = ETIMEDOUT;
            }
        }

        if (connected) {
            fcntl(fd, F_SETFL, flags);
            set_socket_timeouts(fd);
            break;
        }
        close(fd);
        fd = -1;
        if (cgc_interrupted)
            break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (errno == ETIMEDOUT)
            warn_cgc("timed out connecting to %s:%d", host, port);
        else
            warn_cgc("cannot connect to %s:%d: %s", host, port, strerror(errno));
    }
    return fd;
}

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
} Conn;

static ssize_t conn_read(Conn *c, void *buf, size_t len);
static int conn_write_all(Conn *c, const void *data, size_t len);
static int proxy_connect(Conn *c, const Url *u, const Proxy *p);

static int tls_start(Conn *c, const Url *u)
{
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

static int conn_open(Conn *c, const Url *u, const Proxy *p)
{
    memset(c, 0, sizeof(*c));

    if (p->enabled) {
        c->fd = tcp_connect(p->host, p->port);
        if (c->fd < 0)
            return -1;
        if (strcmp(u->scheme, "https") == 0) {
            if (proxy_connect(c, u, p) != 0)
                return -1;
            return tls_start(c, u);
        }
        return 0;
    }

    c->fd = tcp_connect(u->host, u->port);
    if (c->fd < 0)
        return -1;
    if (strcmp(u->scheme, "https") != 0)
        return 0;
    return tls_start(c, u);
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
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
            (err == SSL_ERROR_SYSCALL &&
             (errno == EAGAIN || errno == EWOULDBLOCK)))
            errno = ETIMEDOUT;
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
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            errno = ETIMEDOUT;
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
            if (n <= 0) {
                int err = SSL_get_error(c->ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
                    (err == SSL_ERROR_SYSCALL &&
                     (errno == EAGAIN || errno == EWOULDBLOCK)))
                    errno = ETIMEDOUT;
                return -1;
            }
        } else {
            ssize_t w = write(c->fd, p, len);
            if (w < 0) {
                if (errno == EINTR) {
                    if (cgc_interrupted)
                        return -1;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    errno = ETIMEDOUT;
                return -1;
            }
            n = (int)w;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int proxy_connect(Conn *c, const Url *u, const Proxy *p)
{
    Buf req;
    buf_init(&req);
    buf_appendf(&req, "CONNECT %s:%d HTTP/1.1\r\n", u->host, u->port);
    buf_appendf(&req, "Host: %s:%d\r\n", u->host, u->port);
    if (p->auth)
        buf_appendf(&req, "Proxy-Authorization: Basic %s\r\n", p->auth);
    buf_append(&req, "\r\n", 2);

    int rc = conn_write_all(c, req.data, req.len);
    buf_free(&req);
    if (rc != 0) {
        warn_cgc("failed to send CONNECT to proxy %s:%d", p->host, p->port);
        return -1;
    }

    char header[8192];
    size_t len = 0;
    while (len < sizeof(header) - 1) {
        ssize_t n = conn_read(c, header + len, 1);
        if (n <= 0) {
            warn_cgc("proxy %s:%d closed the connection during CONNECT",
                     p->host, p->port);
            return -1;
        }
        len++;
        header[len] = 0;
        if (len >= 4 && memcmp(header + len - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    if (len < 4 || memcmp(header + len - 4, "\r\n\r\n", 4) != 0) {
        warn_cgc("proxy response is too large");
        return -1;
    }

    char *sp = strchr(header, ' ');
    int status = 0;
    if (!sp || sscanf(sp + 1, "%d", &status) != 1) {
        warn_cgc("malformed response from proxy %s:%d", p->host, p->port);
        return -1;
    }
    if (status != 200) {
        warn_cgc("proxy CONNECT to %s:%d failed with HTTP %d",
                 u->host, u->port, status);
        return -1;
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

typedef struct {
    Buf *buf;
    size_t limit;
} Capture;

static int capture_sink(void *ud, const void *data, size_t len)
{
    Capture *c = ud;
    if (c->buf->len >= c->limit)
        return 0;
    size_t room = c->limit - c->buf->len;
    if (len > room)
        len = room;
    if (len)
        buf_append(c->buf, data, len);
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

    Proxy proxy;
    proxy_for(&u, &proxy);

    Conn c;
    if (conn_open(&c, &u, &proxy) != 0) {
        conn_close(&c);
        proxy_free(&proxy);
        url_free(&u);
        return -1;
    }

    int default_port = strcmp(u.scheme, "https") == 0 ? 443 : 80;
    Buf req;
    buf_init(&req);
    if (proxy.enabled && strcmp(u.scheme, "http") == 0) {
        buf_appendf(&req, "%s %s://%s", method, u.scheme, u.host);
        if (u.port != default_port)
            buf_appendf(&req, ":%d", u.port);
        buf_appendf(&req, "%s HTTP/1.1\r\n", u.path);
    } else {
        buf_appendf(&req, "%s %s HTTP/1.1\r\n", method, u.path);
    }
    if (u.port == default_port)
        buf_appendf(&req, "Host: %s\r\n", u.host);
    else
        buf_appendf(&req, "Host: %s:%d\r\n", u.host, u.port);
    buf_appendf(&req, "User-Agent: cgc/%s\r\n", CGC_VERSION);
    buf_append(&req, "Accept: */*\r\n", strlen("Accept: */*\r\n"));
    buf_append(&req, "Accept-Encoding: identity\r\n", strlen("Accept-Encoding: identity\r\n"));
    buf_append(&req, "Connection: close\r\n", strlen("Connection: close\r\n"));
    if (g_http_token) {
        buf_appendf(&req, "Authorization: Bearer %s\r\n", g_http_token);
    }
    if (proxy.enabled && strcmp(u.scheme, "http") == 0 && proxy.auth)
        buf_appendf(&req, "Proxy-Authorization: Basic %s\r\n", proxy.auth);
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
    proxy_free(&proxy);

    if (conn_write_all(&c, req.data, req.len) != 0) {
        if (errno == ETIMEDOUT)
            warn_cgc("timed out sending request to %s", u.host);
        else
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
    char *reason_resp = NULL;
    char *content_type_resp = NULL;
    char *location = NULL;
    int failed = 0;

    BodyDecoder dec;
    memset(&dec, 0, sizeof(dec));
    buf_init(&dec.pending);
    dec.sink = append_sink;
    dec.ud = &resp->body;

    Capture capture;
    capture.buf = &resp->body;
    capture.limit = 4096;

    for (;;) {
        buf_reserve(&raw, 16384);
        ssize_t n = conn_read(&c, raw.data + raw.len, raw.cap - raw.len - 1);
        if (n < 0) {
            if (errno == ETIMEDOUT)
                warn_cgc("timed out waiting for a response from %s", u.host);
            else
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
            char *sp1 = line_end ? strchr(headers, ' ') : NULL;
            if (!line_end || !sp1 ||
                sscanf(sp1 + 1, "%d", &status) != 1 ||
                status < 100 || status > 599) {
                warn_cgc("malformed HTTP response from %s", u.host);
                free(headers);
                failed = 1;
                break;
            }
            *line_end = 0;
            char *sp2 = strchr(sp1 + 1, ' ');
            if (sp2 && sp2[1])
                reason_resp = xstrdup(sp2 + 1);
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
                    dec.sink = capture_sink;
                    dec.ud = &capture;
                }
            } else if (status != 200) {
                /* Do not buffer a large HTML error page just to print a hint. */
                dec.sink = capture_sink;
                dec.ud = &capture;
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
        free(reason_resp);
        free(content_type_resp);
        free(location);
        return -1;
    }

    resp->status = status;
    resp->reason = reason_resp;
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

static const char *status_hint(int status)
{
    switch (status) {
    case 401:
        return "authentication required; pass a token with --token or CGC_TOKEN";
    case 403:
        return "access denied; check that the token can read this repository";
    case 404:
        return "repository not found; check the URL and that it is not private";
    case 407:
        return "proxy authentication required; add credentials to the proxy URL";
    case 429:
        return "rate limited by the server; try again later";
    case 500:
    case 502:
    case 503:
    case 504:
        return "the server had a temporary problem; try again later";
    default:
        return NULL;
    }
}

static void warn_error_body(const HttpResponse *r)
{
    if (!r->body.len)
        return;
    if (r->content_type && contains_ci(r->content_type, "html"))
        return;

    size_t n = r->body.len < 200 ? r->body.len : 200;
    char buf[201];
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = r->body.data[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            if (w > 0 && buf[w - 1] != ' ')
                buf[w++] = ' ';
        } else if ((c >= 0x20 && c < 0x7f) || c >= 0x80) {
            buf[w++] = (char)c;
        }
    }
    while (w > 0 && buf[w - 1] == ' ')
        w--;
    buf[w] = 0;
    if (w)
        warn_cgc("server response: %s", buf);
}

void http_warn_status(const HttpResponse *r, const char *action)
{
    if (r->reason && *r->reason)
        warn_cgc("remote returned HTTP %d %s while %s",
                 r->status, r->reason, action);
    else
        warn_cgc("remote returned HTTP %d while %s", r->status, action);

    const char *hint = status_hint(r->status);
    if (hint)
        warn_cgc("%s", hint);
    warn_error_body(r);
}

static void retry_sleep(int attempt)
{
    long ms = 250L << (attempt < 4 ? attempt : 4);
    if (ms > HTTP_MAX_RETRY_DELAY_MS)
        ms = HTTP_MAX_RETRY_DELAY_MS;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !cgc_interrupted)
        ;
}

int http_request(const char *method, const char *url,
                 const char *content_type,
                 const void *body, size_t bodylen,
                 const char *extra_headers,
                 HttpResponse *resp)
{
    int retryable = strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0;
    int retries = retryable ? http_retries() : 0;

    for (int attempt = 0;; attempt++) {
        int rc = do_request(method, url, content_type, body, bodylen,
                            extra_headers, NULL, NULL, resp);
        if (rc == 0) {
            int transient = resp->status == 408 || resp->status == 429 ||
                            resp->status == 500 || resp->status == 502 ||
                            resp->status == 503 || resp->status == 504;
            if (!transient || attempt >= retries)
                return 0;
            http_response_free(resp);
        } else if (attempt >= retries || cgc_interrupted) {
            return -1;
        }
        retry_sleep(attempt);
    }
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
