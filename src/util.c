#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <zlib.h>

int cgc_quiet = 0;

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void buf_reserve(Buf *b, size_t extra)
{
    if (extra > SIZE_MAX - b->len - 1)
        die("buffer too large");
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2)
            cap = need;
        else
            cap *= 2;
    }
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(Buf *b, const void *data, size_t len)
{
    if (!len)
        return;
    buf_reserve(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
}

void buf_appendf(Buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        die("vsnprintf failed");
    buf_reserve(b, (size_t)n);
    vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_putc(Buf *b, int c)
{
    unsigned char ch = (unsigned char)c;
    buf_append(b, &ch, 1);
}

void buf_consume(Buf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
    } else {
        memmove(b->data, b->data + n, b->len - n);
        b->len -= n;
    }
    if (b->data)
        b->data[b->len] = 0;
}

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("cgc: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void warn_cgc(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("cgc: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t m)
{
    if (m && n > SIZE_MAX / m)
        die("out of memory");
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p)
        die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("out of memory");
    return q;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void hex_encode(const unsigned char *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 15];
    }
    out[n * 2] = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int hex_decode(const char *in, unsigned char *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hexval((unsigned char)in[i * 2]);
        int lo = hexval((unsigned char)in[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

int oid_parse(const char *hex, unsigned char oid[OID_RAW])
{
    if (strlen(hex) != OID_HEX)
        return -1;
    return hex_decode(hex, oid, OID_RAW);
}

void oid_hex(const unsigned char oid[OID_RAW], char out[OID_HEX + 1])
{
    hex_encode(oid, OID_RAW, out);
}

void sha1_buf(const void *data, size_t len, unsigned char out[OID_RAW])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        die("EVP_MD_CTX_new failed");
    unsigned int outlen = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, out, &outlen) != 1 ||
        outlen != OID_RAW) {
        EVP_MD_CTX_free(ctx);
        die("SHA-1 computation failed");
    }
    EVP_MD_CTX_free(ctx);
}

void put_be32(Buf *b, uint32_t v)
{
    unsigned char p[4] = {
        (unsigned char)(v >> 24), (unsigned char)(v >> 16),
        (unsigned char)(v >> 8), (unsigned char)v
    };
    buf_append(b, p, 4);
}

uint32_t get_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t get_be64(const unsigned char *p)
{
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

int mkdir_p(const char *path, mode_t mode)
{
    if (!path || !*path)
        return 0;

    char *tmp = xstrdup(path);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
            free(tmp);
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int write_file(const char *path, const void *data, size_t len, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    const unsigned char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    if (close(fd) != 0)
        return -1;
    return 0;
}

char *path_dirname(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return xstrdup(".");
    if (slash == path)
        return xstrdup("/");
    return xstrndup(path, (size_t)(slash - path));
}

int write_file_atomic(const char *path, const void *data, size_t len, mode_t mode)
{
    char *dir = path_dirname(path);
    Buf tmpl;
    buf_init(&tmpl);
    buf_appendf(&tmpl, "%s/.cgc-tmp.XXXXXX", dir);
    free(dir);

    int fd = mkstemp((char *)tmpl.data);
    if (fd < 0) {
        buf_free(&tmpl);
        return -1;
    }
    if (fchmod(fd, mode) != 0) {
        close(fd);
        unlink((char *)tmpl.data);
        buf_free(&tmpl);
        return -1;
    }
    const unsigned char *p = data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink((char *)tmpl.data);
            buf_free(&tmpl);
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    if (close(fd) != 0 || rename((char *)tmpl.data, path) != 0) {
        unlink((char *)tmpl.data);
        buf_free(&tmpl);
        return -1;
    }
    buf_free(&tmpl);
    return 0;
}

int read_file(const char *path, Buf *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    out->len = 0;
    for (;;) {
        buf_reserve(out, 8192);
        ssize_t n = read(fd, out->data + out->len, out->cap - out->len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        out->len += (size_t)n;
    }
    close(fd);
    if (out->data)
        out->data[out->len] = 0;
    return 0;
}

int is_dir_empty(const char *path, int *exists)
{
    *exists = 0;
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            return 1;
        return -1;
    }
    *exists = 1;
    if (!S_ISDIR(st.st_mode))
        return 0;
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *de;
    int empty = 1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
            empty = 0;
            break;
        }
    }
    closedir(d);
    return empty;
}

char *path_join(const char *a, const char *b)
{
    if (!b || !*b)
        return xstrdup(a);
    if (b[0] == '/')
        return xstrdup(b);
    Buf out;
    buf_init(&out);
    buf_append(&out, a, strlen(a));
    if (out.len && out.data[out.len - 1] != '/')
        buf_putc(&out, '/');
    buf_append(&out, b, strlen(b));
    return (char *)out.data;
}

int ref_name_is_valid(const char *name)
{
    if (!name || strncmp(name, "refs/", 5) != 0)
        return 0;
    if (name[5] == 0)
        return 0;
    size_t total = strlen(name);
    if (name[total - 1] == '/' || name[total - 1] == '.')
        return 0;

    for (const char *p = name; *p;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        if (len == 0 || p[0] == '.' || p[len - 1] == '.')
            return 0;
        if (len >= 5 && memcmp(p + len - 5, ".lock", 5) == 0)
            return 0;

        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c < 0x20 || c == 0x7f || c == ' ' || c == '~' ||
                c == '^' || c == ':' || c == '?' || c == '*' ||
                c == '[' || c == '\\')
                return 0;
        }
        if (!slash)
            break;
        p = slash + 1;
    }

    if (strstr(name, "..") || strstr(name, "@{"))
        return 0;
    return 1;
}

int path_is_safe_relative(const char *path)
{
    if (!path || !*path || path[0] == '/' || path[0] == '\\')
        return 0;

    const char *p = path;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        if (len == 0)
            return 0;
        if ((len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.') ||
            (len == 4 && strncasecmp(p, ".git", 4) == 0))
            return 0;

        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c < 0x20 || c == 0x7f || c == '\\')
                return 0;
        }

        if (!slash)
            break;
        p = slash + 1;
    }
    return 1;
}

int zlib_inflate(const unsigned char *in, size_t inlen, size_t expected,
                 Buf *out, size_t *consumed)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK)
        return -1;

    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inlen;
    out->len = 0;
    if (expected)
        buf_reserve(out, expected);

    int ret;
    for (;;) {
        if (out->cap - out->len < 4096)
            buf_reserve(out, 4096);
        zs.next_out = out->data + out->len;
        zs.avail_out = (uInt)(out->cap - out->len);
        size_t before = out->len;
        ret = inflate(&zs, Z_NO_FLUSH);
        out->len = before + (out->cap - out->len - zs.avail_out);
        if (out->len >= out->cap)
            buf_reserve(out, 1);
        out->data[out->len] = 0;
        if (ret == Z_STREAM_END)
            break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return -1;
        }
        /* No input left but the stream is not finished: truncated. */
        if (zs.avail_in == 0) {
            inflateEnd(&zs);
            return -1;
        }
    }
    if (consumed)
        *consumed = inlen - zs.avail_in;
    inflateEnd(&zs);
    return 0;
}

int zlib_deflate(const void *in, size_t inlen, Buf *out)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK)
        return -1;

    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inlen;
    out->len = 0;

    int ret;
    do {
        if (out->cap - out->len < 8192)
            buf_reserve(out, 8192);
        zs.next_out = out->data + out->len;
        zs.avail_out = (uInt)(out->cap - out->len);
        size_t before = out->len;
        ret = deflate(&zs, Z_FINISH);
        out->len = before + (out->cap - out->len - zs.avail_out);
    } while (ret == Z_OK || (ret == Z_BUF_ERROR && zs.avail_out == 0));

    if (out->len >= out->cap)
        buf_reserve(out, 1);
    out->data[out->len] = 0;
    deflateEnd(&zs);
    return ret == Z_STREAM_END ? 0 : -1;
}
