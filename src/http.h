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

/* Set a bearer token used for all subsequent requests (or NULL to clear). */
void http_set_token(const char *token);

/* Receives decoded body chunks as they arrive. Return 0 to continue. */
typedef int (*http_body_fn)(void *ud, const void *data, size_t len);

/* Perform an HTTP(S) request. extra_headers, if non-NULL, must contain
 * complete CRLF-terminated header lines. Follows up to 5 redirects.
 * Returns 0 on success (any final HTTP status), -1 on transport error. */
int http_request(const char *method, const char *url,
                 const char *content_type,
                 const void *body, size_t bodylen,
                 const char *extra_headers,
                 HttpResponse *resp);

/* Like http_request, but streams the response body to on_body instead of
 * buffering it in resp->body. Only 200 responses are streamed; redirects
 * (and error bodies) are discarded. */
int http_request_stream(const char *method, const char *url,
                        const char *content_type,
                        const void *body, size_t bodylen,
                        const char *extra_headers,
                        http_body_fn on_body, void *ud,
                        HttpResponse *resp);

#endif
