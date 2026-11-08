#include "pktline.h"
#include "url.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want)                                                 \
    do {                                                                     \
        const char *g_ = (got);                                              \
        const char *w_ = (want);                                             \
        if (!g_ || strcmp(g_, w_) != 0) {                                    \
            fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n",         \
                    __FILE__, __LINE__, g_ ? g_ : "(null)", w_);             \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_url_parse(void)
{
    Url u;

    CHECK(url_parse("https://github.com/owner/repo.git", &u) == 0);
    CHECK_STR(u.scheme, "https");
    CHECK_STR(u.host, "github.com");
    CHECK(u.port == 443);
    CHECK_STR(u.path, "/owner/repo.git");
    url_free(&u);

    CHECK(url_parse("http://127.0.0.1:8080/x", &u) == 0);
    CHECK_STR(u.scheme, "http");
    CHECK_STR(u.host, "127.0.0.1");
    CHECK(u.port == 8080);
    CHECK_STR(u.path, "/x");
    url_free(&u);

    CHECK(url_parse("https://user@example.com/x", &u) == 0);
    CHECK_STR(u.host, "example.com");
    url_free(&u);

    CHECK(url_parse("ftp://example.com/x", &u) != 0);
    CHECK(url_parse("example.com/x", &u) != 0);
    CHECK(url_parse("https://example.com/a\nb", &u) != 0);
    CHECK(url_parse("https://example.com/a\rb", &u) != 0);
}

static void test_url_resolve(void)
{
    static const struct {
        const char *arg;
        const char *host;
        const char *url;
        const char *name;
    } cases[] = {
        {"gh:owner/repo", "github.com",
         "https://github.com/owner/repo.git", "repo"},
        {"gl:owner/repo", "github.com",
         "https://gitlab.com/owner/repo.git", "repo"},
        {"bb:owner/repo", "github.com",
         "https://bitbucket.org/owner/repo.git", "repo"},
        {"sym:owner/repo", "github.com",
         "https://git.symlinx.net/owner/repo.git", "repo"},
        {"owner/repo", "github.com",
         "https://github.com/owner/repo.git", "repo"},
        {"owner/repo", "git.example.com",
         "https://git.example.com/owner/repo.git", "repo"},
        {"git.example.com/owner/repo", "github.com",
         "https://git.example.com/owner/repo.git", "repo"},
        {"owner/repo.git", "github.com",
         "https://github.com/owner/repo.git", "repo"},
        {"https://github.com/owner/repo", "github.com",
         "https://github.com/owner/repo", "repo"},
        {"https://github.com/owner/repo.git/", "github.com",
         "https://github.com/owner/repo.git", "repo"},
        {"gh:owner/repo/", "github.com",
         "https://github.com/owner/repo.git", "repo"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *url = NULL;
        char *name = NULL;
        int rc = url_resolve(cases[i].arg, cases[i].host, &url, &name);
        CHECK(rc == 0);
        if (rc == 0) {
            CHECK_STR(url, cases[i].url);
            CHECK_STR(name, cases[i].name);
        }
        free(url);
        free(name);
    }
}

static void test_pktline(void)
{
    Buf b;
    buf_init(&b);
    pkt_write_str(&b, "hello\n");
    pkt_delim(&b);
    pkt_write(&b, "", 0);
    pkt_flush(&b);

    CHECK(b.len >= 4 && memcmp(b.data, "000a", 4) == 0);

    PktReader r;
    pkt_reader_init(&r, b.data, b.len);
    const unsigned char *line;
    size_t len;

    CHECK(pkt_read(&r, &line, &len) == PKT_DATA);
    CHECK(len == 6 && memcmp(line, "hello\n", 6) == 0);
    CHECK(pkt_read(&r, &line, &len) == PKT_DELIM);
    CHECK(pkt_read(&r, &line, &len) == PKT_DATA);
    CHECK(len == 0);
    CHECK(pkt_read(&r, &line, &len) == PKT_FLUSH);
    CHECK(pkt_read(&r, &line, &len) == PKT_EOF);

    buf_free(&b);
    buf_init(&b);
    pkt_reader_init(&r, "0002", 4);
    CHECK(pkt_read(&r, &line, &len) == PKT_RESPONSE_END);
    CHECK(pkt_read(&r, &line, &len) == PKT_EOF);
    buf_free(&b);
}

static void test_sha1(void)
{
    unsigned char digest[OID_RAW];
    char hex[OID_HEX + 1];

    sha1_buf("abc", 3, digest);
    oid_hex(digest, hex);
    CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

    unsigned char parsed[OID_RAW];
    CHECK(oid_parse(hex, parsed) == 0);
    CHECK(memcmp(digest, parsed, OID_RAW) == 0);
    CHECK(oid_parse("not-a-sha", parsed) != 0);
}

static void test_zlib(void)
{
    static const char msg[] =
        "the quick brown fox jumps over the lazy dog, repeatedly and loudly";
    Buf compressed, output;
    buf_init(&compressed);
    buf_init(&output);

    CHECK(zlib_deflate(msg, sizeof(msg) - 1, &compressed) == 0);
    CHECK(compressed.len > 0);

    size_t consumed = 0;
    CHECK(zlib_inflate(compressed.data, compressed.len, sizeof(msg) - 1,
                       &output, &consumed) == 0);
    CHECK(consumed == compressed.len);
    CHECK(output.len == sizeof(msg) - 1);
    CHECK(memcmp(output.data, msg, sizeof(msg) - 1) == 0);

    buf_free(&output);
    buf_init(&output);
    CHECK(zlib_inflate(compressed.data, compressed.len - 1, sizeof(msg) - 1,
                       &output, NULL) != 0);
    buf_free(&output);
    buf_free(&compressed);
}

static void test_paths(void)
{
    char *p;

    p = path_join("a", "b");
    CHECK_STR(p, "a/b");
    free(p);
    p = path_join("a/", "b");
    CHECK_STR(p, "a/b");
    free(p);
    p = path_join("a", "/b");
    CHECK_STR(p, "/b");
    free(p);
    p = path_join("a", "");
    CHECK_STR(p, "a");
    free(p);

    p = path_dirname("/a");
    CHECK_STR(p, "/");
    free(p);
    p = path_dirname("a");
    CHECK_STR(p, ".");
    free(p);
    p = path_dirname("a/b");
    CHECK_STR(p, "a");
    free(p);
}

static void test_safe_paths(void)
{
    static const char *valid[] = {
        "a",
        "a/b",
        "src/deep/a.txt",
        "naïve.txt",
        "file with space.txt",
        ".gitmodules",
        ".gitattributes",
        "a/.gitignore",
    };
    static const char *invalid[] = {
        "",
        "/a",
        "\\a",
        ".",
        "..",
        "../a",
        "a/../b",
        "a/./b",
        "a//b",
        ".git",
        ".GIT",
        "a/.git/config",
        "a\\.git\\config",
        "a\nb",
        "a\tb",
    };

    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        CHECK(path_is_safe_relative(valid[i]));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!path_is_safe_relative(invalid[i]));
}

static void test_ref_names(void)
{
    static const char *valid[] = {
        "refs/heads/main",
        "refs/heads/feature/x",
        "refs/tags/v1.0.0",
        "refs/tags/release-1",
    };
    static const char *invalid[] = {
        "HEAD",
        "refs/heads/",
        "refs//x",
        "refs/heads/../evil",
        "refs/heads/.hidden",
        "refs/heads/trailing.",
        "refs/heads/x.lock",
        "refs/heads/a b",
        "refs/heads/a~b",
        "refs/heads/a^b",
        "refs/heads/a:b",
        "refs/heads/a?b",
        "refs/heads/a*b",
        "refs/heads/a[b",
        "refs/heads/a\\b",
        "refs/heads/a@{b",
        "refs/heads/a\nb",
        "heads/main",
    };

    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        CHECK(ref_name_is_valid(valid[i]));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!ref_name_is_valid(invalid[i]));
}

int main(void)
{
    test_url_parse();
    test_url_resolve();
    test_pktline();
    test_sha1();
    test_zlib();
    test_paths();
    test_safe_paths();
    test_ref_names();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("unit tests passed\n");
    return 0;
}
