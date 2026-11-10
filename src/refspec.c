#include "refspec.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void refspec_set_init(RefspecSet *set)
{
    set->items = NULL;
    set->count = 0;
    set->cap = 0;
}

void refspec_set_free(RefspecSet *set)
{
    for (size_t i = 0; i < set->count; i++) {
        free(set->items[i].src);
        free(set->items[i].dst);
    }
    free(set->items);
    refspec_set_init(set);
}

/* Validate a ref pattern: the same rules as ref_name_is_valid(), except that
 * one '*' wildcard is allowed. */
static int pattern_is_valid(const char *pattern)
{
    if (!pattern || strncmp(pattern, "refs/", 5) != 0)
        return 0;
    if (pattern[5] == '\0')
        return 0;

    size_t total = strlen(pattern);
    if (pattern[total - 1] == '/' || pattern[total - 1] == '.')
        return 0;
    if (strstr(pattern, "..") || strstr(pattern, "@{"))
        return 0;

    int stars = 0;
    for (const char *p = pattern; *p;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        if (len == 0 || p[0] == '.' || p[len - 1] == '.')
            return 0;
        if (len >= 5 && memcmp(p + len - 5, ".lock", 5) == 0)
            return 0;

        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c == '*') {
                if (++stars > 1)
                    return 0;
                continue;
            }
            if (c < 0x20 || c == 0x7f || c == ' ' || c == '~' ||
                c == '^' || c == ':' || c == '?' || c == '[' || c == '\\')
                return 0;
        }
        if (!slash)
            break;
        p = slash + 1;
    }
    return 1;
}

/* Match pattern against name. When the pattern contains '*', the portion of
 * name matched by the wildcard is returned through cap/caplen. */
static int pattern_match(const char *pattern, const char *name,
                         const char **cap, size_t *caplen)
{
    const char *star = strchr(pattern, '*');
    if (!star)
        return strcmp(pattern, name) == 0;

    size_t plen = (size_t)(star - pattern);
    size_t slen = strlen(star + 1);
    size_t nlen = strlen(name);
    if (nlen < plen + slen)
        return 0;
    if (memcmp(name, pattern, plen) != 0)
        return 0;
    if (slen && memcmp(name + nlen - slen, star + 1, slen) != 0)
        return 0;

    if (cap)
        *cap = name + plen;
    if (caplen)
        *caplen = nlen - plen - slen;
    return 1;
}

static char *substitute_wildcard(const char *dst, const char *cap, size_t caplen)
{
    const char *star = strchr(dst, '*');
    if (!star)
        return xstrdup(dst);

    Buf b;
    buf_init(&b);
    buf_append(&b, dst, (size_t)(star - dst));
    buf_append(&b, cap, caplen);
    buf_append(&b, star + 1, strlen(star + 1));
    return (char *)b.data;
}

int refspec_set_add(RefspecSet *set, const char *text)
{
    if (!text)
        return -1;

    const char *p = text;
    Refspec rs;
    memset(&rs, 0, sizeof(rs));

    if (*p == '+') {
        rs.force = 1;
        p++;
    }
    if (*p == '^') {
        rs.negative = 1;
        p++;
    }
    if (!*p)
        return -1;

    const char *colon = strchr(p, ':');
    if (colon && strchr(colon + 1, ':'))
        return -1;

    if (colon) {
        if (colon == p || colon[1] == '\0')
            return -1;
        rs.src = xstrndup(p, (size_t)(colon - p));
        rs.dst = xstrdup(colon + 1);
    } else {
        rs.src = xstrdup(p);
    }

    if (rs.negative && rs.dst) {
        free(rs.src);
        free(rs.dst);
        return -1;
    }
    if (!pattern_is_valid(rs.src) || (rs.dst && !pattern_is_valid(rs.dst))) {
        free(rs.src);
        free(rs.dst);
        return -1;
    }

    int src_star = strchr(rs.src, '*') != NULL;
    int dst_star = rs.dst && strchr(rs.dst, '*') != NULL;
    if (src_star != dst_star) {
        free(rs.src);
        free(rs.dst);
        return -1;
    }

    if (set->count == set->cap) {
        set->cap = set->cap ? set->cap * 2 : 4;
        set->items = xrealloc(set->items, set->cap * sizeof(Refspec));
    }
    set->items[set->count++] = rs;
    return 0;
}

int refspec_set_map(const RefspecSet *set, const char *refname, char **out_dst,
                    int *out_force)
{
    if (out_dst)
        *out_dst = NULL;
    if (out_force)
        *out_force = 0;

    for (size_t i = 0; i < set->count; i++) {
        const Refspec *rs = &set->items[i];
        if (rs->negative && pattern_match(rs->src, refname, NULL, NULL))
            return REFSPEC_EXCLUDED;
    }

    for (size_t i = 0; i < set->count; i++) {
        const Refspec *rs = &set->items[i];
        if (rs->negative)
            continue;

        const char *cap = NULL;
        size_t caplen = 0;
        if (!pattern_match(rs->src, refname, &cap, &caplen))
            continue;

        if (out_dst && rs->dst)
            *out_dst = substitute_wildcard(rs->dst, cap, caplen);
        if (out_force)
            *out_force = rs->force;
        return REFSPEC_MATCH;
    }
    return REFSPEC_NO_MATCH;
}

int refspec_dst_matches(const Refspec *rs, const char *refname)
{
    if (!rs->dst)
        return 0;
    return pattern_match(rs->dst, refname, NULL, NULL);
}
