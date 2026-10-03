#ifndef HTTP_STATUS_CODE
#define HTTP_STATUS_CODE

typedef enum HttpStatusCode {
  HTTP_STATUS_OK = 200,
  HTTP_STATUS_CREATED = 201,
  HTTP_STATUS_NO_CONTENT = 204,
  HTTP_STATUS_BAD_REQUEST = 400,
  HTTP_STATUS_NOT_FOUND = 404,
  HTTP_STATUS_METHOD_NOT_ALLOWED = 405,
  HTTP_STATUS_SERVER_ERROR = 500,
  HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED = 505
} HttpStatusCode;

inline const char *http_status_reason_phrase(HttpStatusCode code) {
  switch (code) {
  case HTTP_STATUS_OK:
    return "OK";
  case HTTP_STATUS_CREATED:
    return "Created";
  case HTTP_STATUS_BAD_REQUEST:
    return "Bad Request";
  case HTTP_STATUS_NOT_FOUND:
    return "Not Found";
  case HTTP_STATUS_METHOD_NOT_ALLOWED:
    return "Method Not Allowed";
  case HTTP_STATUS_SERVER_ERROR:
    return "Internal Server Error";
  case HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED:
    return "HTTP Version Not Supported";
  case HTTP_STATUS_NO_CONTENT:
    return "No Content";
  default:
    return "";
  }
}

#endif // !HTTP_STATUS_CODE
