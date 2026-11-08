#ifndef CGC_UTIL_H
#define CGC_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CGC_VERSION_STRING "dev"

#ifndef CGC_VERSION
#define CGC_VERSION CGC_VERSION_STRING
#endif
#define OID_RAW 20
#define OID_HEX 40

extern int cgc_quiet;

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_append(Buf *b, const void *data, size_t len);
void buf_appendf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_putc(Buf *b, int c);
void buf_consume(Buf *b, size_t n);

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn_cgc(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

void hex_encode(const unsigned char *in, size_t n, char *out);
int hex_decode(const char *in, unsigned char *out, size_t n);
int oid_parse(const char *hex, unsigned char oid[OID_RAW]);
void oid_hex(const unsigned char oid[OID_RAW], char out[OID_HEX + 1]);

void sha1_buf(const void *data, size_t len, unsigned char out[OID_RAW]);

void put_be32(Buf *b, uint32_t v);
uint32_t get_be32(const unsigned char *p);
uint64_t get_be64(const unsigned char *p);

int mkdir_p(const char *path, mode_t mode);
int write_file(const char *path, const void *data, size_t len, mode_t mode);
int write_file_atomic(const char *path, const void *data, size_t len, mode_t mode);
int read_file(const char *path, Buf *out);
int is_dir_empty(const char *path, int *exists);
char *path_join(const char *a, const char *b);
char *path_dirname(const char *path);

/* Check a full ref name (e.g. refs/heads/main) against Git's rules. */
int ref_name_is_valid(const char *name);

/* Check a repository-relative path: no absolute paths, no empty, "." or ".."
 * components, no ".git" component (case-insensitive), no backslashes or
 * control characters. */
int path_is_safe_relative(const char *path);

/* zlib helpers. `consumed` may be NULL. */
int zlib_inflate(const unsigned char *in, size_t inlen, size_t expected,
                 Buf *out, size_t *consumed);
int zlib_deflate(const void *in, size_t inlen, Buf *out);

#endif
