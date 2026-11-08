#include "pktline.h"

#include <string.h>

void pkt_reader_init(PktReader *r, const void *data, size_t len)
{
    r->data = data;
    r->len = len;
    r->pos = 0;
}

static int hex4(const unsigned char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i];
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return -1;
        v = (v << 4) | d;
    }
    return v;
}

int pkt_read(PktReader *r, const unsigned char **line, size_t *linelen)
{
    if (r->pos >= r->len)
        return PKT_EOF;
    if (r->len - r->pos < 4)
        die("truncated pkt-line");

    int n = hex4(r->data + r->pos);
    if (n < 0)
        die("malformed pkt-line length");
    r->pos += 4;

    if (n == 0)
        return PKT_FLUSH;
    if (n == 1)
        return PKT_DELIM;
    if (n == 2)
        return PKT_RESPONSE_END;
    if (n < 4 || (size_t)n - 4 > r->len - r->pos)
        die("malformed pkt-line");

    *line = r->data + r->pos;
    *linelen = (size_t)n - 4;
    r->pos += (size_t)n - 4;
    return PKT_DATA;
}

void pkt_write(Buf *b, const void *data, size_t len)
{
    if (len > 65516)
        die("pkt-line payload too large");
    buf_appendf(b, "%04zx", len + 4);
    buf_append(b, data, len);
}

void pkt_write_str(Buf *b, const char *s)
{
    pkt_write(b, s, strlen(s));
}

void pkt_flush(Buf *b)
{
    buf_append(b, "0000", 4);
}

void pkt_delim(Buf *b)
{
    buf_append(b, "0001", 4);
}
