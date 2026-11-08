#ifndef CGC_HTTP_H
#define CGC_HTTP_H

#include <stddef.h>

#include "util.h"

typedef struct {
    int status;
    Buf body;
    char *content_type;
    char *location;
} HttpResponse;

void http_response_free(HttpResponse *r);

/* Perform an HTTP(S) request. extra_headers, if non-NULL, must contain
 * complete CRLF-terminated header lines. Follows up to 5 redirects.
 * Returns 0 on success (any final HTTP status), -1 on transport error. */
int http_request(const char *method, const char *url,
                 const char *content_type,
                 const void *body, size_t bodylen,
                 const char *extra_headers,
                 HttpResponse *resp);

#endif
