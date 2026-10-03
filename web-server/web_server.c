#include "http_headers.h"
#include "http_status_code.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#include "http_request.h"
#include "http_response.h"
#include "web-server-internal/http_headers_internal.h"
#include "web-server-internal/http_query_internal.h"
#include "web_server.h"
#include <WS2tcpip.h>
#include <WinSock2.h>

typedef struct WSAData WSAData;
typedef struct sockaddr_in sockaddr_in;
typedef struct sockaddr sockaddr;

#define SERVER_PORT 8080
#define MAX_CONNECTIONS 60
#define TIME_OUT_IN_SEC 30

typedef enum HttpRequestState {
  HTTP_REQUEST_READING_HEADER,
  HTTP_REQUEST_READING_BODY,
  HTTP_REQUEST_DONE
} HttpRequestState;

typedef enum HttpRequestParsingStep {
  HTTP_REQUEST_PRASING_METHOD,
  HTTP_REQUEST_PARSING_REQUEST_TARGET,
  HTTP_REQUEST_PARSING_REQUEST_VERSION,
  HTTP_REQUEST_PARSING_FIELD_LINES,
  HTTP_REQUEST_PARSING_DONE
} HttpRequestParsingStep;

typedef struct HttpRequestParsingContext {
  HttpRequestParsingStep step;
  char *method;
  char *request_target;
  char *version;
  char *field_lines;
} HttpRequestParsingContext;

typedef struct HttpRequest {
  HttpRequestState state;

  HttpMethod method;
  char *route;
  HttpQuery *query;
  HttpHeaders *headers;
  size_t content_length;
  size_t content_read;

  struct SocketState *socket_state;
  HttpRequestParsingContext context;
} HttpRequest;

typedef enum HttpResponseState {
  HTTP_RESPONSE_WRITING_STATUS_CODE,
  HTTP_RESPONSE_WRITING_HEADERS,
  HTTP_RESPONSE_WRITING_BODY,
  HTTP_RESPONSE_DONE
} HttpResponseState;

typedef enum SocketSendState {
  SOCKET_SEND_ACTIVE,
  SOCKET_SEND_IDLE,
  SOCKET_SEND_FINISHED
} SocketSendState;

struct HttpResponse {
  HttpResponseState state;

  HttpStatusCode http_status_code;
  HttpHeaders *headers;
  SocketSendState headers_send_state;
  size_t write_headers_index[3];

  void *context;

  // Set for HEAD requests: the GET handler runs as usual, but body writes are
  // dropped so only the status line and headers are sent.
  bool discard_body;

  struct SocketState *socket_state;
};

typedef enum SocketRecvState {
  SOCKET_RECV_ACTIVE,
  SOCKET_RECV_IDLE,
  SOCKET_RECV_FINISHED
} SocketRecvState;

#define BUFFER_SIZE 256

typedef struct CycleBuffer {
  char buffer[BUFFER_SIZE];
  size_t start;
  size_t end;
  bool empty;
} CycleBuffer;

static size_t cyctle_buffer_used_space(const CycleBuffer *buffer) {
  if (!buffer->empty && buffer->start == buffer->end) {
    return BUFFER_SIZE;
  }

  if (buffer->start == buffer->end) {
    return 0;
  }

  if (buffer->end < buffer->start) {
    return buffer->end + (BUFFER_SIZE - buffer->start);
  }

  return buffer->end - buffer->start;
}

static bool cyctle_buffer_has_space(const CycleBuffer *buffer) {
  return BUFFER_SIZE - cyctle_buffer_used_space(buffer) > 0;
}

static bool cyctle_buffer_add_char(CycleBuffer *buffer, char c) {
  if (!buffer->empty && buffer->end == buffer->start) {
    return false;
  }

  buffer->buffer[buffer->end++] = c;
  buffer->end = buffer->end % BUFFER_SIZE;
  buffer->empty = false;
  return true;
}

static bool cyctle_buffer_take_char(CycleBuffer *buffer, char *out_c) {
  assert(out_c);
  if (buffer->empty) {
    return false;
  }

  *out_c = buffer->buffer[buffer->start++];
  buffer->start = buffer->start % BUFFER_SIZE;
  if (buffer->start == buffer->end) {
    buffer->empty = true;
  }
  return true;
}

static bool cyctle_buffer_is_empty(const CycleBuffer *buffer) {
  return buffer->empty;
}

// Copies up to `length` chars from the front of the buffer without removing
// them. Returns the number of chars copied.
static size_t cyctle_buffer_peek(const CycleBuffer *buffer, char *out,
                                 size_t length) {
  size_t used_space = cyctle_buffer_used_space(buffer);
  size_t count = used_space < length ? used_space : length;
  for (size_t i = 0; i < count; i++) {
    out[i] = buffer->buffer[(buffer->start + i) % BUFFER_SIZE];
  }
  return count;
}

typedef struct SocketState {
  SOCKET socket;
  unsigned long long timeout_time;

  SocketRecvState recv_state;
  CycleBuffer recv_buffer;

  SocketSendState send_state;
  CycleBuffer send_buffer;

  HttpRequest http_request;
  HttpResponse http_response;
  WebServer *server;
} SocketState;

enum HttpMethod req_get_method(const HttpRequest *req) {
  assert(req && "req_get_method: req is null.");
  assert(
      req->state != HTTP_REQUEST_READING_HEADER &&
      "req_get_method: cannot accesse method bofore finished reading headers.");
  return req->method;
}
const HttpQuery *req_get_query(const HttpRequest *req) {
  assert(req && "req_get_query: req is null.");
  assert(
      req->state != HTTP_REQUEST_READING_HEADER &&
      "req_get_query: cannot accesse query bofore finished reading headers.");
  return req->query;
}
const HttpHeaders *req_get_headers(const HttpRequest *req) {
  assert(req && "req_get_headers: req is null.");
  assert(req->state != HTTP_REQUEST_READING_HEADER &&
         "req_get_headers: cannot accesse headers bofore finished reading "
         "headers.");
  return req->headers;
}
size_t req_get_content_length(const HttpRequest *req) {
  assert(req && "req_get_content_length: req is null.");
  assert(req->state != HTTP_REQUEST_READING_HEADER &&
         "req_get_content_length: cannot access content length before "
         "finished reading headers.");
  return req->content_length;
}

bool req_start_read(HttpRequest *req) {
  assert(req && "req_can_read: req is null.");
  assert(req->state != HTTP_REQUEST_READING_HEADER &&
         "req_can_read: cannot accesse body bofore finished reading headers.");
  assert(req->socket_state &&
         "req_can_read: req has a null socket_state, something went wrong!");

  SocketState *socket_state = req->socket_state;

  if (req->content_length > 0 &&
      socket_state->recv_state != SOCKET_RECV_FINISHED) {
    socket_state->recv_state = SOCKET_RECV_ACTIVE;
    return true;
  }

  return false;
}
size_t req_read(HttpRequest *req, char *buffer, size_t length) {
  assert(req && "req_read: req is null.");
  assert(req->state != HTTP_REQUEST_READING_HEADER &&
         "req_read: cannot access body before finished reading headers.");
  assert(req->socket_state &&
         "req_read: req has a null socket_state, something went wrong!");

  if (length == 0) {
    return 0;
  }

  SocketState *state = req->socket_state;

  size_t chars_read_count = 0;
  while (!cyctle_buffer_is_empty(&state->recv_buffer)) {
    if (chars_read_count == length) {
      break;
    }

    cyctle_buffer_take_char(&state->recv_buffer, &buffer[chars_read_count++]);
  }

  req->content_read += chars_read_count;

  if (req->content_read == req->content_length) {
    state->recv_state = SOCKET_RECV_FINISHED;
    state->http_request.state = HTTP_REQUEST_DONE;
  }

  return chars_read_count;
}
void res_set_status_code(HttpResponse *res, enum HttpStatusCode code) {
  assert(res && "res_set_status_code: res is null");
  assert(res->state == HTTP_RESPONSE_WRITING_STATUS_CODE &&
         "res_set_status_code: response all ready set status code.");

  SocketState *state = res->socket_state;
  size_t used_space = cyctle_buffer_used_space(&state->send_buffer);
  size_t free_space = BUFFER_SIZE - used_space;

  char status_line[BUFFER_SIZE + 1];
  size_t length = sprintf_s(status_line, BUFFER_SIZE, "HTTP/1.1 %d %s\r\n",
                            code, http_status_reason_phrase(code));
  assert(free_space > length && "there should be space for the status line...");

  size_t chars_wrote_count = 0;
  while (cyctle_buffer_has_space(&state->send_buffer)) {
    if (chars_wrote_count == length) {
      break;
    }

    cyctle_buffer_add_char(&state->send_buffer,
                           status_line[chars_wrote_count++]);
  }

  res->http_status_code = code;
  res->state = HTTP_RESPONSE_WRITING_HEADERS;
  state->send_state = SOCKET_SEND_ACTIVE;
}
HttpHeaders *res_get_headers(const HttpResponse *res) {
  assert(res && "res_get_headers: res is null");
  assert((res->state == HTTP_RESPONSE_WRITING_STATUS_CODE ||
          res->state == HTTP_RESPONSE_WRITING_HEADERS) &&
         "res_get_headers: response all ready finished writing headers.");
  assert(res->headers && "res_get_headers: response headers is null");

  return res->headers;
}
void res_finish_headers(HttpResponse *res) {
  assert(res && "res_finish_headers: res is null");
  assert(res->state == HTTP_RESPONSE_WRITING_HEADERS &&
         "res_finish_headers: response status doesn't allow to finish headers. "
         "Did you forgot to set status code? or did you all ready finished "
         "headers?");

  res->state = HTTP_RESPONSE_WRITING_BODY;
  res->headers_send_state = SOCKET_SEND_ACTIVE;
}
void res_finish_write(HttpResponse *res) {
  assert(res && "res_finish_write: res is null");
  assert(res->state == HTTP_RESPONSE_WRITING_BODY &&
         "res_finish_headers request");

  res->state = HTTP_RESPONSE_DONE;
}
void res_set_context(HttpResponse *res, void *context) {
  assert(res && "res_set_context: res is null");

  res->context = context;
}
void *res_get_context(const HttpResponse *res) {
  assert(res && "res_get_context: res is null");

  return res->context;
}

bool res_can_write(const HttpResponse *res) {
  assert(res && "res_can_write: res is null.");
  assert(res->socket_state &&
         "res_can_write: res has a null socket_state, something went wrong!");

  if (res->state != HTTP_RESPONSE_WRITING_BODY ||
      res->headers_send_state == SOCKET_SEND_ACTIVE) {
    return false;
  }

  const SocketState *socket_state = res->socket_state;

  return cyctle_buffer_has_space(&socket_state->send_buffer);
}
size_t res_wirte(HttpResponse *res, const char *buffer, size_t length) {
  assert(res && "res_write: res is null.");
  assert(res->socket_state &&
         "res_write: res has a null socket_state, something went wrong!");

  res->socket_state->send_state = SOCKET_SEND_ACTIVE;
  if (length == 0) {
    return 0;
  }

  if (res->state != HTTP_RESPONSE_WRITING_BODY) {
    return 0;
  }

  if (res->discard_body) {
    return length;
  }

  SocketState *state = res->socket_state;

  size_t chars_wrote_count = 0;
  while (cyctle_buffer_has_space(&state->send_buffer)) {
    if (chars_wrote_count == length) {
      break;
    }

    cyctle_buffer_add_char(&state->send_buffer, buffer[chars_wrote_count++]);
  }

  return chars_wrote_count;
}

typedef void (*RequestHandlerFunc)(HttpRequest *req, HttpResponse *res);
typedef void (*RequestHandlerCleanup)(void *context);

typedef struct WebServer {
  SocketState socket_states[MAX_CONNECTIONS];
  struct RouteReqHandler {
    HttpMethod method;
    const char *route;
    RequestHandlerFunc handler;
    RequestHandlerCleanup cleanup;
  } *request_handlers;
  size_t request_handlers_count;
} WebServer;

static void accept_connection(WebServer *server, SOCKET listen_socket);
static bool add_connection(WebServer *server, SOCKET socket_to_connect);
static void remove_connection(WebServer *server, size_t index);
static void receive_data(SocketState *socket_state);
static void send_data(SocketState *socket_state);
static unsigned long long get_currect_time_stamp();
static void parse_request_header(SocketState *socket_state);
static void fill_send_with_headers(SocketState *socket_state);
static void zero_socket_state(SocketState *state);
static RequestHandlerFunc find_matching_handler(WebServer *server,
                                                HttpRequest *req);
static RequestHandlerCleanup find_matching_cleanup(WebServer *server,
                                                   HttpRequest *req);

int web_server_run(WebServer *server) {
  WSAData wsa_data;

  if (NO_ERROR != WSAStartup(MAKEWORD(2, 2), &wsa_data)) {
    printf("Server: Error at WSAStartup()\n");
    return 1;
  }

  SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (INVALID_SOCKET == listen_socket) {
    printf("Server: Error at socket(): %d\n", WSAGetLastError());
    WSACleanup();
    return 1;
  }

  sockaddr_in server_service;
  server_service.sin_family = AF_INET;
  /*if (!inet_pton(AF_INET, "127.0.0.1", &server_service.sin_addr.s_addr)) {
    printf("Server: Error at converting string ip to number: %d\n",
           WSAGetLastError());
    closesocket(listen_socket);
    WSACleanup();
    return 1;
  }*/
  server_service.sin_addr.s_addr = INADDR_ANY;
  server_service.sin_port = htons(SERVER_PORT);

  if (SOCKET_ERROR == bind(listen_socket, (SOCKADDR *)&server_service,
                           sizeof(server_service))) {
    printf("Server: Error at bind(): %d\n", WSAGetLastError());
    closesocket(listen_socket);
    WSACleanup();
    return 1;
  }

  if (SOCKET_ERROR == listen(listen_socket, 5)) {
    printf("Server: Error at listen(): %d\n", WSAGetLastError());
    closesocket(listen_socket);
    WSACleanup();
    return 1;
  }

  char address_buff[16];
  inet_ntop(AF_INET, &server_service.sin_addr.s_addr, address_buff, 16);
  printf("Server: listening on %s:%d\n", address_buff, SERVER_PORT);
  fd_set wait_recv;
  fd_set wait_send;

  SocketState *sockets = server->socket_states;

  for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
    zero_socket_state(&sockets[i]);
  }

  int res = 0;

  while (true) {
    unsigned long long current_time = get_currect_time_stamp();
    FD_ZERO(&wait_recv);
    FD_ZERO(&wait_send);

    FD_SET(listen_socket, &wait_recv);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
      if (sockets[i].socket == 0) {
        continue;
      }
      if (sockets[i].recv_state == SOCKET_RECV_ACTIVE)
        FD_SET(sockets[i].socket, &wait_recv);
      if (sockets[i].send_state == SOCKET_SEND_ACTIVE)
        FD_SET(sockets[i].socket, &wait_send);
    }

    int nfd = select(0, &wait_recv, &wait_send, NULL, NULL);
    if (nfd == SOCKET_ERROR) {
      printf("Server: Error at select(): %d\n", WSAGetLastError());
      res = 1;
      break;
    }

    if (FD_ISSET(listen_socket, &wait_recv)) {
      accept_connection(server, listen_socket);
      nfd--;
    }

    for (size_t i = 0; i < MAX_CONNECTIONS && nfd > 0; i++) {
      if (!sockets[i].socket) {
        continue;
      }

      // read data
      if (FD_ISSET(sockets[i].socket, &wait_recv)) {
        nfd--;
        if (sockets[i].recv_state == SOCKET_RECV_ACTIVE) {
          receive_data(&sockets[i]);
        }
      }

      // handling states
      if (sockets[i].http_request.state == HTTP_REQUEST_READING_HEADER) {
        parse_request_header(&sockets[i]);
      }

      if (sockets[i].http_request.state &
              (HTTP_REQUEST_READING_BODY | HTTP_REQUEST_DONE) &&
          sockets[i].http_response.state != HTTP_RESPONSE_DONE) {
        RequestHandlerFunc handler =
            find_matching_handler(server, &sockets[i].http_request);
        handler(&sockets[i].http_request, &sockets[i].http_response);
      }

      // fill send buffer with headers
      if (sockets[i].http_response.headers_send_state == SOCKET_SEND_ACTIVE) {
        fill_send_with_headers(&sockets[i]);
      }

      // send data
      if (FD_ISSET(sockets[i].socket, &wait_send)) {
        nfd--;
        if (sockets[i].send_state == SOCKET_SEND_ACTIVE) {
          send_data(&sockets[i]);
        }
      }

      // if done sending close connection
      if ((sockets[i].http_response.state == HTTP_RESPONSE_DONE &&
           sockets[i].send_state != SOCKET_SEND_ACTIVE) ||
          sockets[i].timeout_time < current_time) {
        RequestHandlerCleanup cleanup =
            find_matching_cleanup(server, &sockets[i].http_request);
        if (cleanup) {
          cleanup(sockets[i].http_response.context);
        }
        remove_connection(server, i);
      }
    }
  }

  for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
    if (sockets[i].socket != 0) {
      remove_connection(server, i);
    }
  }

  free(server->request_handlers);
  free(server);

  printf("Server: Closing Connection.\n");
  closesocket(listen_socket);
  WSACleanup();

  return res;
}

static void accept_connection(WebServer *server, SOCKET listen_socket) {
  struct sockaddr_in from;
  int from_size = sizeof(from);

  SOCKET msg_socket =
      accept(listen_socket, (struct sockaddr *)&from, &from_size);
  if (INVALID_SOCKET == msg_socket) {
    printf("Server: Error at accept(): %d\n", WSAGetLastError());
    return;
  }

  char ipv4[16];
  printf("Server: Client %s:%d is connected.\n",
         inet_ntop(AF_INET, &from.sin_addr, ipv4, 16), ntohs(from.sin_port));

  unsigned long flag = 1;
  if (ioctlsocket(msg_socket, FIONBIO, &flag) != 0) {
    printf("Server: Error at ioctlsocket(): %d\n", WSAGetLastError());
    closesocket(msg_socket);
    return;
  }

  if (add_connection(server, msg_socket) == false) {
    char ipv4[16];
    printf("Server: Client %s:%d has disconnected.\n\t\tToo many connections, "
           "dropped!\n",
           inet_ntop(AF_INET, &from.sin_addr, ipv4, 16), ntohs(from.sin_port));
    closesocket(msg_socket);
  }
}

static bool add_connection(WebServer *server, SOCKET connection_socket) {
  assert(server && "add_connection: server is null.");
  assert(connection_socket != 0 && "add_connection: connection_socket is 0.");

  for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
    if (!server->socket_states[i].socket) {
      SocketState *state = &server->socket_states[i];

      state->socket = connection_socket;

      state->recv_state = SOCKET_RECV_ACTIVE;
      state->recv_buffer.start = 0;
      state->recv_buffer.end = 0;
      state->recv_buffer.empty = true;
      state->send_state = SOCKET_SEND_IDLE;
      state->send_buffer.start = 0;
      state->send_buffer.end = 0;
      state->send_buffer.empty = true;

      state->http_request.state = HTTP_REQUEST_READING_HEADER;
      state->http_request.route = NULL;
      state->http_request.query = create_http_query();
      state->http_request.headers = create_http_headers();
      state->http_request.content_length = 0;
      state->http_request.content_read = 0;
      state->http_request.context.step = HTTP_REQUEST_PRASING_METHOD;
      state->http_request.context.method = NULL;
      state->http_request.context.request_target = NULL;
      state->http_request.context.version = NULL;
      state->http_request.context.field_lines = NULL;
      state->http_request.socket_state = state;

      state->http_response.state = HTTP_RESPONSE_WRITING_STATUS_CODE;
      state->http_response.headers_send_state = SOCKET_SEND_IDLE;
      state->http_response.headers = create_http_headers();
      state->http_response.write_headers_index[0] = 0;
      state->http_response.write_headers_index[1] = 0;
      state->http_response.write_headers_index[2] = 0;
      state->http_response.context = NULL;
      state->http_response.discard_body = false;
      state->http_response.socket_state = state;

      state->timeout_time =
          get_currect_time_stamp() + (TIME_OUT_IN_SEC * 1000000000ull);
      state->server = server;
      return true;
    }
  }
  return false;
}

static void zero_socket_state(SocketState *state) {
  state->socket = 0;
  state->recv_state = 0;
  state->recv_buffer.start = 0;
  state->recv_buffer.end = 0;
  state->recv_buffer.empty = true;
  state->send_state = 0;
  state->send_buffer.start = 0;
  state->send_buffer.end = 0;
  state->send_buffer.empty = true;
  state->http_request.state = 0;
  state->http_request.route = NULL;
  state->http_request.query = NULL;
  state->http_request.headers = NULL;
  state->http_request.content_length = 0;
  state->http_request.content_read = 0;
  state->http_request.context.step = 0;
  state->http_request.context.method = NULL;
  state->http_request.context.request_target = NULL;
  state->http_request.context.version = NULL;
  state->http_request.context.field_lines = NULL;
  state->http_request.socket_state = NULL;
  state->http_response.state = 0;
  state->http_response.headers_send_state = 0;
  state->http_response.write_headers_index[0] = 0;
  state->http_response.write_headers_index[1] = 0;
  state->http_response.write_headers_index[2] = 0;
  state->http_response.headers = NULL;
  state->http_response.discard_body = false;
  state->http_response.socket_state = NULL;
  state->timeout_time = 0;
  state->server = 0;
}

static void remove_connection(WebServer *server, size_t index) {
  assert(server && "remove_connection: server is null.");
  assert(index < MAX_CONNECTIONS &&
         "remove_connection: index is larger then MAX_CONNECTION.");
  assert(server->socket_states[index].socket != 0 &&
         "remove_connection: no connection in this index.");

  SocketState *state = &server->socket_states[index];
  struct sockaddr_in from;
  int from_size = sizeof(from);
  getpeername(state->socket, (struct sockaddr *)&from, &from_size);
  char ipv4[16];
  printf("Server: Client %s:%d has disconnected.\n",
         inet_ntop(AF_INET, &from.sin_addr, ipv4, 16), ntohs(from.sin_port));
  closesocket(state->socket);
  free(state->http_request.route);
  destroy_http_query(state->http_request.query);
  destroy_http_headers(state->http_request.headers);
  free(state->http_request.context.method);
  free(state->http_request.context.request_target);
  free(state->http_request.context.version);
  free(state->http_request.context.field_lines);
  destroy_http_headers(state->http_response.headers);
  zero_socket_state(state);
}

static unsigned long long get_currect_time_stamp() {
  FILETIME curr;
  GetSystemTimePreciseAsFileTime(&curr);
  ULARGE_INTEGER curr_ull = {.u.HighPart = curr.dwHighDateTime,
                             .u.LowPart = curr.dwLowDateTime};
  return curr_ull.QuadPart;
}

static void receive_data(SocketState *socket_state) {
  assert(socket_state && "recive_data: socket_state is null.");
  assert(socket_state->socket != 0 &&
         "recive_data: socket of socket_state is 0.");

  SOCKET msg_socket = socket_state->socket;
  size_t used_space = cyctle_buffer_used_space(&socket_state->recv_buffer);
  size_t free_space = BUFFER_SIZE - used_space;
  if (free_space == 0) {
    return;
  }

  char temp_buffer[BUFFER_SIZE];
  size_t bytes_recv = recv(msg_socket, temp_buffer, (int)free_space, 0);

  if (SOCKET_ERROR == bytes_recv) {
    printf("Server: Error at recv(): %d\n", WSAGetLastError());
    socket_state->send_state = SOCKET_SEND_FINISHED;
    socket_state->http_response.state = HTTP_RESPONSE_DONE;
    return;
  }

  if (bytes_recv == 0) {
    printf("Server: Error at recv(), read 0 bytes: %d\n", WSAGetLastError());
    socket_state->send_state = SOCKET_SEND_FINISHED;
    socket_state->http_response.state = HTTP_RESPONSE_DONE;
    return;
  }

  for (size_t i = 0; i < bytes_recv; i++) {
    cyctle_buffer_add_char(&socket_state->recv_buffer, temp_buffer[i]);
  }
}

static void send_data(SocketState *socket_state) {
  assert(socket_state && "send_data: socket_state is null.");
  assert(socket_state->socket != 0 &&
         "send_data: socket of socket_state is 0.");

  SOCKET msg_socket = socket_state->socket;
  size_t used_space = cyctle_buffer_used_space(&socket_state->send_buffer);
  if (used_space == 0) {
    socket_state->send_state = SOCKET_SEND_IDLE;
    return;
  }

  // Peek instead of take: send() may accept only part of the data, and the
  // rest has to stay in the buffer for the next send.
  char temp_buffer[BUFFER_SIZE];
  cyctle_buffer_peek(&socket_state->send_buffer, temp_buffer, used_space);

  int sent = send(msg_socket, temp_buffer, (int)used_space, 0);
  if (SOCKET_ERROR == sent) {
    printf("Server: Error at send(): %d\n", WSAGetLastError());
    socket_state->send_state = SOCKET_SEND_FINISHED;
    socket_state->http_response.state = HTTP_RESPONSE_DONE;
    return;
  }

  size_t bytes_sent = (size_t)sent;
  char discarded;
  for (size_t i = 0; i < bytes_sent; i++) {
    cyctle_buffer_take_char(&socket_state->send_buffer, &discarded);
  }

  if (bytes_sent == 0) {
    printf("Server: Error at sent(), sent 0 bytes: %d\n", WSAGetLastError());
    socket_state->send_state = SOCKET_SEND_FINISHED;
    socket_state->http_response.state = HTTP_RESPONSE_DONE;
    return;
  }

  if (socket_state->http_response.state == HTTP_RESPONSE_DONE &&
      used_space == bytes_sent) {
    socket_state->send_state = SOCKET_SEND_FINISHED;
  }
}

static void fill_send_with_headers(SocketState *socket_state) {
  size_t used_space = cyctle_buffer_used_space(&socket_state->send_buffer);
  size_t free_space = BUFFER_SIZE - used_space;
  if (free_space == 0) {
    return;
  }

  if (socket_state->http_response.state != HTTP_RESPONSE_WRITING_BODY &&
      socket_state->http_response.state != HTTP_RESPONSE_DONE) {
    return;
  }

  if (socket_state->http_response.headers_send_state != SOCKET_SEND_ACTIVE) {
    return;
  }

  HttpHeaders *headers = socket_state->http_response.headers;
  size_t total_count = headers_count(headers);
  size_t headers_index = socket_state->http_response.write_headers_index[0];
  bool is_value = socket_state->http_response.write_headers_index[1];
  size_t word_index = socket_state->http_response.write_headers_index[2];
  while (free_space > 0) {
    if (headers_index == total_count) {
      break;
    }

    const char *word = is_value
                           ? headers_get_value_at_index(headers, headers_index)
                           : headers_get_key_at_index(headers, headers_index);
    size_t word_len = strlen(word);
    for (; word_index < word_len && free_space > 0; word_index++) {
      cyctle_buffer_add_char(&socket_state->send_buffer, word[word_index]);
      free_space--;
    }

    if (word_index == word_len) {
      if (is_value) {
        if (free_space > 1) {
          cyctle_buffer_add_char(&socket_state->send_buffer, '\r');
          cyctle_buffer_add_char(&socket_state->send_buffer, '\n');
          is_value = 0;
          headers_index++;
          word_index = 0;
        }
      } else {
        if (free_space > 1) {
          cyctle_buffer_add_char(&socket_state->send_buffer, ':');
          cyctle_buffer_add_char(&socket_state->send_buffer, ' ');
          free_space = free_space - 2;
          is_value = 1;
          word_index = 0;
        }
      }
    }
  }

  socket_state->http_response.write_headers_index[0] = headers_index;
  socket_state->http_response.write_headers_index[1] = is_value;
  socket_state->http_response.write_headers_index[2] = word_index;

  if (headers_index == total_count) {
    if (free_space > 1) {
      cyctle_buffer_add_char(&socket_state->send_buffer, '\r');
      cyctle_buffer_add_char(&socket_state->send_buffer, '\n');
      socket_state->http_response.headers_send_state = SOCKET_SEND_FINISHED;
    }
  }
}

static void set_response_server_error(SocketState *socket_state) {
  assert(socket_state && "parse_method_header: socket_state is null.");

  res_set_status_code(&socket_state->http_response, HTTP_STATUS_SERVER_ERROR);
  headers_add(socket_state->http_response.headers, "Content-Type",
              "text/html; charset=UTF-8");
  headers_add(socket_state->http_response.headers, "Content-Length", "0");
  res_finish_headers(&socket_state->http_response);
  res_finish_write(&socket_state->http_response);
}

static void set_response_method_not_allowed(SocketState *socket_state) {
  assert(socket_state && "parse_method_header: socket_state is null.");

  res_set_status_code(&socket_state->http_response,
                      HTTP_STATUS_METHOD_NOT_ALLOWED);
  headers_add(socket_state->http_response.headers, "Content-Type",
              "text/html; charset=UTF-8");
  headers_add(socket_state->http_response.headers, "Content-Length", "0");
  res_finish_headers(&socket_state->http_response);
  res_finish_write(&socket_state->http_response);
}

static void set_response_bad_request(SocketState *socket_state) {
  assert(socket_state && "parse_method_header: socket_state is null.");

  res_set_status_code(&socket_state->http_response, HTTP_STATUS_BAD_REQUEST);
  headers_add(socket_state->http_response.headers, "Content-Type",
              "text/html; charset=UTF-8");
  headers_add(socket_state->http_response.headers, "Content-Length", "0");
  res_finish_headers(&socket_state->http_response);
  res_finish_write(&socket_state->http_response);
}

static void set_response_version_not_supported(SocketState *socket_state) {
  assert(socket_state &&
         "set_response_version_not_supported: socket_state is null.");

  res_set_status_code(&socket_state->http_response,
                      HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED);
  headers_add(socket_state->http_response.headers, "Content-Type",
              "text/html; charset=UTF-8");
  headers_add(socket_state->http_response.headers, "Content-Length", "0");
  res_finish_headers(&socket_state->http_response);
  res_finish_write(&socket_state->http_response);
}

static void parse_method(SocketState *socket_state) {
  assert(socket_state && "parse_method_header: socket_state is null.");

  HttpRequestParsingContext *context = &socket_state->http_request.context;

  if (context->method == NULL) {
    context->method = malloc(sizeof(char) * 1);
    if (context->method == NULL) {
      set_response_server_error(socket_state);
      return;
    }
    context->method[0] = 0;
  }

  size_t used_space = cyctle_buffer_used_space(&socket_state->recv_buffer);

  char temp_buffer[BUFFER_SIZE + 1];
  size_t chars_read;
  bool hit_space = false;
  for (chars_read = 0; chars_read < used_space;) {
    char c;
    cyctle_buffer_take_char(&socket_state->recv_buffer, &c);
    if (c == ' ') {
      hit_space = true;
      break;
    }
    temp_buffer[chars_read++] = c;
  }

  size_t current_len = strlen(context->method);
  if ((chars_read + current_len) > MAX_HTTP_METHOD_LENGTH) {
    set_response_bad_request(socket_state);
    return;
  }

  size_t new_size = (current_len + chars_read + 1);
  char *temp = realloc(context->method, sizeof(char) * new_size);
  if (temp == NULL) {
    set_response_server_error(socket_state);
    return;
  }
  for (size_t i = 0; current_len < new_size - 1;) {
    temp[current_len++] = temp_buffer[i++];
  }
  temp[current_len] = 0;
  context->method = temp;

  if (!hit_space) {
    return;
  }

  for (int i = 0; i < HTTP_METHOD_LENGTH; i++) {
    const char *method_name = http_method_name(i);
    if (strcmp(method_name, context->method) == 0) {
      context->step = HTTP_REQUEST_PARSING_REQUEST_TARGET;
      socket_state->http_request.method = i;
      return;
    }
  }

  set_response_method_not_allowed(socket_state);
}

static void parse_request_target(SocketState *socket_state) {
  assert(socket_state && "parse_request_target: socket_state is null.");

  HttpRequestParsingContext *context = &socket_state->http_request.context;

  if (context->request_target == NULL) {
    context->request_target = malloc(sizeof(char) * 1);
    if (context->request_target == NULL) {
      set_response_server_error(socket_state);
      return;
    }
    context->request_target[0] = 0;
  }

  size_t used_space = cyctle_buffer_used_space(&socket_state->recv_buffer);

  char temp_buffer[BUFFER_SIZE + 1];
  size_t chars_read;
  bool hit_space = false;
  for (chars_read = 0; chars_read < used_space;) {
    char c;
    cyctle_buffer_take_char(&socket_state->recv_buffer, &c);
    if (c == ' ') {
      hit_space = true;
      break;
    }
    temp_buffer[chars_read++] = c;
  }

  size_t current_len = strlen(context->request_target);
  size_t new_len = (current_len + chars_read);
  char *temp = realloc(context->request_target, sizeof(char) * (new_len + 1));
  if (temp == NULL) {
    set_response_server_error(socket_state);
    return;
  }
  for (size_t i = 0; current_len < new_len;) {
    temp[current_len++] = temp_buffer[i++];
  }
  temp[current_len] = 0;
  context->request_target = temp;

  if (!hit_space) {
    return;
  }

  char *q_mark = strchr(context->request_target, '?');
  size_t route_len = 0;
  if (q_mark != NULL) {
    q_mark[0] = 0;
    route_len = strlen(context->request_target);

    char *key = q_mark + 1;

    while (1) {
      char *eq_mark = strchr(key, '=');
      if (eq_mark == NULL) {
        break;
      }
      eq_mark[0] = 0;
      char *value = eq_mark + 1;
      char *and_mark = strchr(value, '&');
      if (and_mark != NULL) {
        and_mark[0] = 0;
      }

      if (!query_add(socket_state->http_request.query, key, value)) {
        // TODO: do something?
      }

      eq_mark[0] = '=';

      if (and_mark == NULL) {
        break;
      } else {
        key = and_mark + 1;
        and_mark[0] = '&';
      }
    }

    q_mark[0] = '?';
  } else {
    route_len = strlen(context->request_target);
  }

  socket_state->http_request.route = malloc(sizeof(char) * (route_len + 1));
  if (socket_state->http_request.route == NULL) {
    set_response_server_error(socket_state);
    return;
  }

  size_t request_target_len = strlen(context->request_target);
  if (request_target_len > route_len) {
    context->request_target[route_len] = 0;
  }
  if (strcpy_s(socket_state->http_request.route, route_len + 1,
               context->request_target)) {
    set_response_server_error(socket_state);
    return;
  }
  if (request_target_len > route_len) {
    context->request_target[route_len] = '?';
  }
  // TODO: do proper validation on route
  context->step = HTTP_REQUEST_PARSING_REQUEST_VERSION;
}
static void parse_request_version(SocketState *socket_state) {
  assert(socket_state && "parse_method_header: socket_state is null.");

  HttpRequestParsingContext *context = &socket_state->http_request.context;

  if (context->version == NULL) {
    context->version = malloc(sizeof(char) * 1);
    if (context->version == NULL) {
      set_response_server_error(socket_state);
      return;
    }
    context->version[0] = 0;
  }

  size_t used_space = cyctle_buffer_used_space(&socket_state->recv_buffer);

  char temp_buffer[BUFFER_SIZE + 1] = {0};
  size_t chars_read;
  bool hit_lf = false;
  for (chars_read = 0; chars_read < used_space;) {
    char c;
    cyctle_buffer_take_char(&socket_state->recv_buffer, &c);
    if (c == '\n') {
      hit_lf = true;
      break;
    }

    temp_buffer[chars_read++] = c;
  }

  size_t current_len = strlen(context->version);
  if ((chars_read + current_len - 1) > 9) {
    set_response_bad_request(socket_state);
    return;
  }

  size_t new_size = (current_len + chars_read);
  char *temp = realloc(context->version, sizeof(char) * (new_size + 1));
  if (temp == NULL) {
    set_response_server_error(socket_state);
    return;
  }
  for (size_t i = 0; current_len < new_size;) {
    temp[current_len++] = temp_buffer[i++];
  }
  temp[current_len] = 0;
  context->version = temp;

  if (!hit_lf) {
    return;
  }

  for (int i = 0; i < HTTP_METHOD_LENGTH; i++) {
    if (strcmp("HTTP/1.1\r", context->version) == 0) {
      context->step = HTTP_REQUEST_PARSING_FIELD_LINES;
      return;
    }
  }

  set_response_version_not_supported(socket_state);
}
static void parse_request_field_liens(SocketState *socket_state) {
  assert(socket_state);

  HttpRequestParsingContext *context = &socket_state->http_request.context;

  if (context->field_lines == NULL) {
    context->field_lines = malloc(sizeof(char) * 1);
    if (context->field_lines == NULL) {
      set_response_server_error(socket_state);
      return;
    }
    context->field_lines[0] = 0;
  }

  while (!cyctle_buffer_is_empty(&socket_state->recv_buffer)) {
    size_t used_space = cyctle_buffer_used_space(&socket_state->recv_buffer);

    char temp_buffer[BUFFER_SIZE + 1] = {0};
    size_t chars_read;
    bool hit_lf = false;
    for (chars_read = 0; chars_read < used_space;) {
      char c;
      cyctle_buffer_take_char(&socket_state->recv_buffer, &c);
      if (c == '\n') {
        hit_lf = true;
        break;
      }
      temp_buffer[chars_read++] = c;
    }

    size_t current_len = strlen(context->field_lines);
    size_t new_size = (current_len + chars_read);
    char *temp = realloc(context->field_lines, sizeof(char) * (new_size + 1));
    if (temp == NULL) {
      set_response_server_error(socket_state);
      return;
    }
    context->field_lines = temp;
    for (size_t i = 0; current_len < new_size;) {
      temp[current_len++] = temp_buffer[i++];
    }
    temp[current_len] = 0;

    if (!hit_lf) {
      return;
    }

    if (current_len > 0 && context->field_lines[current_len - 1] != '\r') {
      set_response_bad_request(socket_state);
      return;
    }

    if (current_len > 1 && context->field_lines[current_len - 2] != '\r') {
      continue;
    }

    if (current_len == 0) {
      continue;
    }

    // finished reading headers
    context->field_lines[current_len - 2] = 0;

    char *key = context->field_lines;
    while (key[0] != 0) {
      char *cul_mark = strchr(key, ':');
      if (cul_mark == NULL) {
        set_response_bad_request(socket_state);
        return;
      }
      cul_mark[0] = 0;
      for (size_t i = 0; key[i] != 0; i++) {
        if (key[i] == ' ' || key[i] == '\t') {
          set_response_bad_request(socket_state);
          return;
        }
      }

      char *value = cul_mark + 1;
      char *cr_mark = strchr(value, '\r');
      if (cr_mark != NULL) {
        cr_mark[0] = 0;
      }

      while (value[0] == ' ' || value[0] == '\t') {
        value = value + 1;
      }

      size_t value_len = strlen(value);
      for (size_t i = value_len; i >= 0; i--) {
        if (value[i] != ' ' && value[i] != '\t') {
          break;
        }

        value[i] = 0;
      }

      if (!headers_add(socket_state->http_request.headers, key, value)) {
        // TODO: do something?
      }

      cul_mark[0] = ':';

      if (cr_mark == NULL) {
        break;
      } else {
        key = cr_mark + 1;
        cr_mark[0] = '\r';
      }
    }

    context->field_lines[current_len - 2] = '\r';

    context->step = HTTP_REQUEST_PARSING_DONE;
    return;
  }
}

static void parse_request_header(SocketState *socket_state) {
  assert(socket_state && "parse_request_header: socket_state is null.");
  HttpRequestParsingContext *context = &socket_state->http_request.context;

  if (context->step == HTTP_REQUEST_PRASING_METHOD &&
      socket_state->http_response.state != HTTP_RESPONSE_DONE) {
    parse_method(socket_state);
  }

  if (context->step == HTTP_REQUEST_PARSING_REQUEST_TARGET &&
      socket_state->http_response.state != HTTP_RESPONSE_DONE) {
    parse_request_target(socket_state);
  }

  if (context->step == HTTP_REQUEST_PARSING_REQUEST_VERSION &&
      socket_state->http_response.state != HTTP_RESPONSE_DONE) {
    parse_request_version(socket_state);
  }

  if (context->step == HTTP_REQUEST_PARSING_FIELD_LINES &&
      socket_state->http_response.state != HTTP_RESPONSE_DONE) {
    parse_request_field_liens(socket_state);
  }

  if (context->step == HTTP_REQUEST_PARSING_DONE &&
      socket_state->http_response.state != HTTP_RESPONSE_DONE) {
    const char *content_length =
        headers_get(socket_state->http_request.headers, "Content-Length");
    if (content_length != NULL) {
      int length = atoi(content_length);
      if (length > 0) {
        socket_state->http_request.state = HTTP_REQUEST_READING_BODY;
        socket_state->recv_state = SOCKET_RECV_IDLE;
        socket_state->http_request.content_length = length;
        return;
      }
    }

    socket_state->http_request.state = HTTP_REQUEST_DONE;
    socket_state->recv_state = SOCKET_RECV_FINISHED;
  }
}

static void not_found_request_handler(HttpRequest *req, HttpResponse *res) {
  res_set_status_code(res, HTTP_STATUS_NOT_FOUND);
  res_finish_headers(res);
  res_finish_write(res);
}

static RequestHandlerFunc find_matching_handler(WebServer *server,
                                                HttpRequest *req) {
  for (size_t i = 1; i < server->request_handlers_count; i++) {

    if (server->request_handlers[i].method == req->method &&
        _stricmp(server->request_handlers[i].route, req->route) == 0) {
      return server->request_handlers[i].handler;
    }
  }

  return server->request_handlers[0].handler;
}

static RequestHandlerCleanup find_matching_cleanup(WebServer *server,
                                                   HttpRequest *req) {
  if (req->state == HTTP_REQUEST_READING_BODY ||
      req->state == HTTP_REQUEST_DONE) {
    for (size_t i = 1; i < server->request_handlers_count; i++) {

      if (server->request_handlers[i].method == req->method &&
          _stricmp(server->request_handlers[i].route, req->route) == 0) {
        return server->request_handlers[i].cleanup;
      }
    }
  }

  // error happend
  return NULL;
}

typedef struct TraceContext {
  size_t sent;
  size_t length;
  char *body;
} TraceContext;

static void trace_cleanup(TraceContext *context) {
  if (context != NULL) {
    free(context->body);
    free(context);
  }
}

static void trace_func(HttpRequest *req, HttpResponse *res) {
  TraceContext *context = res_get_context(res);
  if (context == NULL) {
    const char *method = req->context.method;
    const char *request_target = req->context.request_target;
    const char *version = req->context.version;
    const char *field_lines = req->context.field_lines;
    const HttpHeaders *headers = req_get_headers(req);
    size_t header_count = headers_count(headers);

    size_t method_len = strlen(method);
    size_t request_target_len = strlen(request_target);
    size_t version_len = strlen(version);
    size_t field_lines_len = strlen(field_lines);
    //                       TRACE / HTTP/1.1                       \r\n
    //                       \n*headers                      \n\0
    size_t size = method_len + 1 + request_target_len + 1 + version_len + 1 +
                  1 + header_count + field_lines_len + 1 + 1;
    char *buffer = malloc(sizeof(char) * size);
    if (buffer == NULL) {
      set_response_server_error(req->socket_state);
      return;
    }
    size_t wrote = 0;
    for (size_t i = 0; i < method_len; i++) {
      buffer[wrote++] = method[i];
    }

    buffer[wrote++] = ' ';

    for (size_t i = 0; i < request_target_len; i++) {
      buffer[wrote++] = request_target[i];
    }

    buffer[wrote++] = ' ';

    for (size_t i = 0; i < version_len; i++) {
      buffer[wrote++] = version[i];
    }

    buffer[wrote++] = '\r';
    buffer[wrote++] = '\n';

    for (size_t i = 0; i < field_lines_len - 1; i++) {
      buffer[wrote++] = field_lines[i];
      if (field_lines[i] == '\r') {
        buffer[wrote++] = '\n';
      }
    }

    buffer[wrote] = 0;

    context = malloc(sizeof(TraceContext));
    if (context == NULL) {
      free(buffer);
      set_response_server_error(res->socket_state);
      return;
    }

    context->body = buffer;
    context->length = wrote;
    context->sent = 0;
    res_set_status_code(res, HTTP_STATUS_OK);
    HttpHeaders *response_headers = res_get_headers(res);
    char leng[256];
    sprintf_s(leng, 256, "%zu", wrote);
    headers_add(response_headers, "Content-Length", leng);
    headers_add(response_headers, "Content-Type", "message/http");
    res_finish_headers(res);
    res_set_context(res, context);
  }

  while (res_can_write(res) && context->sent != context->length) {
    size_t wrote = res_wirte(res, context->body + context->sent,
                             context->length - context->sent);
    context->sent += wrote;
  }

  if (context->sent == context->length) {
    res_finish_write(res);
  }
}

WebServer *create_web_server() {
  WebServer *server = malloc(sizeof(WebServer));
  assert(server);

  server->request_handlers_count = 3;
  server->request_handlers =
      malloc(sizeof(struct RouteReqHandler) * server->request_handlers_count);

  assert(server->request_handlers);

  server->request_handlers[0].route = "*NOT FOUND*";
  server->request_handlers[0].handler = not_found_request_handler;
  server->request_handlers[0].cleanup = NULL;

  server->request_handlers[1].method = HTTP_TRACE;
  server->request_handlers[1].route = "/";
  server->request_handlers[1].handler = trace_func;
  server->request_handlers[1].cleanup = (void (*)(void *))trace_cleanup;

  server->request_handlers[2].method = HTTP_TRACE;
  server->request_handlers[2].route = "*";
  server->request_handlers[2].handler = trace_func;
  server->request_handlers[2].cleanup = (void (*)(void *))trace_cleanup;
  return server;
}

static void options_func(HttpRequest *req, HttpResponse *res) {
  WebServer *server = req->socket_state->server;

  HttpMethod methods[HTTP_METHOD_LENGTH] = {0};
  size_t count = 0;
  for (size_t i = 1; i < server->request_handlers_count; i++) {
    if (_stricmp(server->request_handlers[i].route, req->route) == 0) {
      methods[count++] = server->request_handlers[i].method;
    }
  }

  char allow[HTTP_METHOD_LENGTH * MAX_HTTP_METHOD_LENGTH] = {0};
  size_t wrote = 0;
  for (size_t i = 0; i < count; i++) {
    const char *name = http_method_name(methods[i]);
    size_t len = strlen(name);
    for (size_t j = 0; j < len; j++) {
      allow[wrote++] = name[j];
    }

    if (i < count - 1) {
      allow[wrote++] = ',';
      allow[wrote++] = ' ';
    }
  }
  allow[wrote] = 0;
  res_set_status_code(res, HTTP_STATUS_NO_CONTENT);
  HttpHeaders *headers = res_get_headers(res);
  headers_add(headers, "Allow", allow);
  res_finish_headers(res);
  res_finish_write(res);
}

static void map_options_if_not_exists(WebServer *server, const char *route) {
  for (size_t i = 1; i < server->request_handlers_count; i++) {
    if (server->request_handlers[i].method == HTTP_OPTIONS &&
        _stricmp(server->request_handlers[i].route, route) == 0) {
      return;
    }
  }

  server->request_handlers_count = server->request_handlers_count + 1;
  struct RouteReqHandler *temp =
      realloc(server->request_handlers,
              sizeof(struct RouteReqHandler) * server->request_handlers_count);
  server->request_handlers = temp;
  assert(server->request_handlers);

  server->request_handlers[server->request_handlers_count - 1].method =
      HTTP_OPTIONS;
  server->request_handlers[server->request_handlers_count - 1].route = route;
  server->request_handlers[server->request_handlers_count - 1].handler =
      options_func;
  server->request_handlers[server->request_handlers_count - 1].cleanup = NULL;
}

static void head_func(HttpRequest *req, HttpResponse *res) {
  WebServer *server = req->socket_state->server;

  RequestHandlerFunc handler = NULL;
  for (size_t i = 1; i < server->request_handlers_count; i++) {
    if (server->request_handlers[i].method == HTTP_GET &&
        _stricmp(server->request_handlers[i].route, req->route) == 0) {
      handler = server->request_handlers[i].handler;
    }
  }

  assert(handler);
  // Run the GET handler, possibly over several calls like a normal GET. Its
  // body writes are dropped, and its context is freed by the GET cleanup that
  // map_head registered for this route once the response is done.
  res->discard_body = true;
  req->method = HTTP_GET;
  handler(req, res);
  req->method = HTTP_HEAD;
}

static void map_head(WebServer *server, const char *route,
                     RequestHandlerCleanup cleanup) {
  server->request_handlers_count = server->request_handlers_count + 1;
  struct RouteReqHandler *temp =
      realloc(server->request_handlers,
              sizeof(struct RouteReqHandler) * server->request_handlers_count);
  server->request_handlers = temp;
  assert(server->request_handlers);

  server->request_handlers[server->request_handlers_count - 1].method =
      HTTP_HEAD;
  server->request_handlers[server->request_handlers_count - 1].route = route;
  server->request_handlers[server->request_handlers_count - 1].handler =
      head_func;
  server->request_handlers[server->request_handlers_count - 1].cleanup =
      cleanup;
}

void web_server_map_get(WebServer *server, const char *route,
                        void (*func)(HttpRequest *req, HttpResponse *res),
                        void (*cleanup)(void *context)) {

  server->request_handlers_count = server->request_handlers_count + 1;
  struct RouteReqHandler *temp =
      realloc(server->request_handlers,
              sizeof(struct RouteReqHandler) * server->request_handlers_count);
  server->request_handlers = temp;
  assert(server->request_handlers);

  server->request_handlers[server->request_handlers_count - 1].method =
      HTTP_GET;
  server->request_handlers[server->request_handlers_count - 1].route = route;
  server->request_handlers[server->request_handlers_count - 1].handler = func;
  server->request_handlers[server->request_handlers_count - 1].cleanup =
      cleanup;
  map_options_if_not_exists(server, route);
  map_head(server, route, cleanup);
}
void web_server_map_post(WebServer *server, const char *route,
                         void (*func)(HttpRequest *req, HttpResponse *res),
                         void (*cleanup)(void *context)) {

  server->request_handlers_count = server->request_handlers_count + 1;
  struct RouteReqHandler *temp =
      realloc(server->request_handlers,
              sizeof(struct RouteReqHandler) * server->request_handlers_count);
  server->request_handlers = temp;
  assert(server->request_handlers);

  server->request_handlers[server->request_handlers_count - 1].method =
      HTTP_POST;
  server->request_handlers[server->request_handlers_count - 1].route = route;
  server->request_handlers[server->request_handlers_count - 1].handler = func;
  server->request_handlers[server->request_handlers_count - 1].cleanup =
      cleanup;
  map_options_if_not_exists(server, route);
}
void web_server_map_put(WebServer *server, const char *route,
                        void (*func)(HttpRequest *req, HttpResponse *res),
                        void (*cleanup)(void *context)) {

  server->request_handlers_count = server->request_handlers_count + 1;
  struct RouteReqHandler *temp =
      realloc(server->request_handlers,
              sizeof(struct RouteReqHandler) * server->request_handlers_count);
  server->request_handlers = temp;
  assert(server->request_handlers);

  server->request_handlers[server->request_handlers_count - 1].method =
      HTTP_PUT;
  server->request_handlers[server->request_handlers_count - 1].route = route;
  server->request_handlers[server->request_handlers_count - 1].handler = func;
  server->request_handlers[server->request_handlers_count - 1].cleanup =
      cleanup;
  map_options_if_not_exists(server, route);
}
void web_server_map_delete(WebServer *server, const char *route,
                           void (*func)(HttpRequest *req, HttpResponse *res),
                           void (*cleanup)(void *context)) {

  server->request_handlers_count = server->request_handlers_count + 1;
  struct RouteReqHandler *temp =
      realloc(server->request_handlers,
              sizeof(struct RouteReqHandler) * server->request_handlers_count);
  server->request_handlers = temp;
  assert(server->request_handlers);

  server->request_handlers[server->request_handlers_count - 1].method =
      HTTP_DELETE;
  server->request_handlers[server->request_handlers_count - 1].route = route;
  server->request_handlers[server->request_handlers_count - 1].handler = func;
  server->request_handlers[server->request_handlers_count - 1].cleanup =
      cleanup;
  map_options_if_not_exists(server, route);
}
