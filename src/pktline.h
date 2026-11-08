#ifndef CGC_PKTLINE_H
#define CGC_PKTLINE_H

#include "util.h"

#define PKT_EOF 0
#define PKT_FLUSH 1
#define PKT_DELIM 2
#define PKT_RESPONSE_END 3
#define PKT_DATA 4

typedef struct {
    const unsigned char *data;
    size_t len;
    size_t pos;
} PktReader;

void pkt_reader_init(PktReader *r, const void *data, size_t len);

/* Returns one of the PKT_* values. For PKT_DATA, *line points at the
 * payload and *linelen is its length. Returns PKT_EOF when exhausted. */
int pkt_read(PktReader *r, const unsigned char **line, size_t *linelen);

void pkt_write(Buf *b, const void *data, size_t len);
void pkt_write_str(Buf *b, const char *s);
void pkt_flush(Buf *b);
void pkt_delim(Buf *b);

#endif
