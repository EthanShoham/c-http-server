#ifndef HTTP_REQUEST
#define HTTP_REQUEST
#include "http_headers.h"
#include "http_method.h"
#include "http_query.h"
#include <stdbool.h>

typedef struct HttpRequest HttpRequest;

enum HttpMethod req_get_method(const HttpRequest *req);
const HttpQuery *req_get_query(const HttpRequest *req);
const HttpHeaders *req_get_headers(const HttpRequest *req);
size_t req_get_content_length(const HttpRequest *req);

bool req_start_read(HttpRequest *req);
size_t req_read(HttpRequest *req, char *buffer, size_t length);

#endif
