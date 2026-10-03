#include "web-server/http_request.h"
#include "web-server/http_response.h"
#include "web-server/web_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WRITING_FILE 1

typedef struct Context {
  char *body;
  size_t length;
  size_t sent;
  bool reading; // still collecting the request body
} Context;

// html/ is two levels above the executable (bin\server\server.exe), so pages
// are found no matter which directory the server is started from.
static char html_dir[MAX_PATH];

static bool init_html_dir(void) {
  DWORD length = GetModuleFileNameA(NULL, html_dir, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    return false;
  }

  char *last_slash = strrchr(html_dir, '\\');
  if (last_slash == NULL) {
    return false;
  }
  last_slash[1] = 0;
  return strcat_s(html_dir, MAX_PATH, "..\\..\\html\\") == 0;
}

typedef struct RootContext {
  char *file_content;
  size_t content_length;
  size_t sent_count;
} RootContext;

void write_server_error(HttpResponse *res) {
  res_set_status_code(res, HTTP_STATUS_SERVER_ERROR);
  res_finish_headers(res);
  res_finish_write(res);
}

void write_not_found(HttpResponse *res) {
  res_set_status_code(res, HTTP_STATUS_NOT_FOUND);
  res_finish_headers(res);
  res_finish_write(res);
}

void write_bad_request(HttpResponse *res) {
  res_set_status_code(res, HTTP_STATUS_BAD_REQUEST);
  res_finish_headers(res);
  res_finish_write(res);
}

void root_cleanup(RootContext *context) {
  if (context != NULL) {
    free(context->file_content);
    free(context);
  }
}

void root(HttpRequest *req, HttpResponse *res) {
  RootContext *context = (RootContext *)res_get_context(res);
  if (context == NULL) {
    context = malloc(sizeof(RootContext));
    if (context == NULL) {
      write_server_error(res);
      return;
    }
    const HttpQuery *query = req_get_query(req);
    const char *lang = query_get(query, "lang");
    const char *page = "index.html";
    const char *lan = "en-US";

    if (lang == NULL) {
    } else if (strcmp(lang, "he") == 0) {
      page = "index.he.html";
      lan = "he-IL";
    } else if (strcmp(lang, "fr") == 0) {
      page = "index.fr.html";
      lan = "fr-FR";
    }

    char file[MAX_PATH];
    if (sprintf_s(file, sizeof(file), "%s%s", html_dir, page) < 0) {
      write_server_error(res);
      free(context);
      return;
    }

    context->file_content = malloc(sizeof(char) * 256);
    if (context->file_content == NULL) {
      write_server_error(res);
      free(context);
      return;
    }

    FILE *stream;
    if (fopen_s(&stream, file, "r") != 0) {
      write_server_error(res);
      free(context->file_content);
      free(context);
      return;
    }
    context->content_length = 0;
    context->sent_count = 0;
    char *buffer = context->file_content;
    while (true) {
      if (fgets(buffer, 255, stream) == NULL && !feof(stream)) {
        write_server_error(res);
        free(context->file_content);
        free(context);
        fclose(stream);
        return;
      }

      size_t read = strlen(buffer);
      context->content_length += read;
      if (read == 0) {
        break;
      }

      char *temp =
          realloc(context->file_content, context->content_length + 255);
      if (temp == NULL) {
        write_server_error(res);
        free(context->file_content);
        free(context);
        fclose(stream);
        return;
      }

      context->file_content = temp;
      buffer = context->file_content + context->content_length;
    }

    fclose(stream);
    res_set_context(res, context);
    res_set_status_code(res, HTTP_STATUS_OK);
    HttpHeaders *headers = res_get_headers(res);
    headers_add(headers, "Content-Type", "text/html; charset=utf-8");
    char cnt_len[255];
    snprintf(cnt_len, sizeof(cnt_len), "%zu", context->content_length);
    headers_add(headers, "Content-Length", cnt_len);
    headers_add(headers, "Content-Language", lan);
    res_finish_headers(res);
  }

  while (res_can_write(res) && context->sent_count != context->content_length) {
    size_t wrote = res_wirte(res, context->file_content + context->sent_count,
                             context->content_length - context->sent_count);
    context->sent_count += wrote;
  }

  if (context->sent_count == context->content_length) {
    res_finish_write(res);
  }
}

typedef struct Student {
  int id;
  char fname[50];
  char lname[50];
  int grade;
} Student;

static Student *students = NULL;
static size_t students_capacity = 0;
static size_t students_length = 0;

static void cleanup(Context *context) {
  if (context != NULL) {
    free(context->body);
    free(context);
  }
}

// Writes as much of context->body as the send buffer can take, and finishes the
// response once all of it was written.
static void write_context_body(HttpResponse *res, Context *context) {
  while (res_can_write(res) && context->sent < context->length) {
    size_t wrote = res_wirte(res, context->body + context->sent,
                             context->length - context->sent);
    context->sent += wrote;
  }

  if (context->sent == context->length) {
    res_finish_write(res);
  }
}

static bool parse_id(const char *text, int *out_id) {
  return text != NULL && sscanf_s(text, "%9d", out_id) == 1;
}

static int student_to_json(char *buffer, size_t size, const Student *s) {
  return sprintf_s(buffer, size,
                   "{\"id\":%d,\"fname\":\"%s\",\"lname\":\"%s\",\"grade\":%d}",
                   s->id, s->fname, s->lname, s->grade);
}

// Parses a student object. Whitespace between tokens is allowed, the keys must
// be in this order, and "id" may be left out when require_id is false.
// Returns false if the JSON does not match.
static bool parse_student(const char *json, Student *s, bool require_id,
                          bool *out_has_id) {
  *out_has_id = false;
  int scanned = sscanf_s(json,
                         " { \"id\" : %9d , \"fname\" : \"%49[a-zA-Z ]\" ,"
                         " \"lname\" : \"%49[a-zA-Z ]\" , \"grade\" : %3d }",
                         &s->id, s->fname, (unsigned)sizeof(s->fname), s->lname,
                         (unsigned)sizeof(s->lname), &s->grade);
  if (scanned == 4) {
    *out_has_id = true;
    return true;
  }

  if (require_id) {
    return false;
  }

  scanned = sscanf_s(json,
                     " { \"fname\" : \"%49[a-zA-Z ]\" , \"lname\" : "
                     "\"%49[a-zA-Z ]\" , \"grade\" : %3d }",
                     s->fname, (unsigned)sizeof(s->fname), s->lname,
                     (unsigned)sizeof(s->lname), &s->grade);
  return scanned == 3;
}

typedef enum BodyStatus { BODY_PENDING, BODY_DONE, BODY_ERROR } BodyStatus;

#define MAX_BODY_SIZE 1024

// Collects the request body into context->body. The body can arrive in several
// packets, so the handler calls this on every invocation until it returns
// BODY_DONE.
static BodyStatus read_body(HttpRequest *req, Context *context) {
  size_t total = req_get_content_length(req);
  if (total == 0 || total > MAX_BODY_SIZE) {
    return BODY_ERROR;
  }

  if (context->body == NULL) {
    if (!req_start_read(req)) {
      return BODY_ERROR;
    }
    context->body = malloc(total + 1);
    if (context->body == NULL) {
      return BODY_ERROR;
    }
    context->length = 0;
  }

  context->length += req_read(req, context->body + context->length,
                              total - context->length);
  if (context->length < total) {
    return BODY_PENDING;
  }

  context->body[context->length] = 0;
  return BODY_DONE;
}

// Returns a context for a handler that reads a body, creating it on the first
// call. Returns NULL (after writing a 500) if allocation fails.
static Context *get_or_create_context(HttpResponse *res) {
  Context *context = res_get_context(res);
  if (context != NULL) {
    return context;
  }

  context = calloc(1, sizeof(Context));
  if (context == NULL) {
    write_server_error(res);
    return NULL;
  }
  context->reading = true;
  res_set_context(res, context);
  return context;
}

static bool add_student(const Student *s) {
  if (students_length == students_capacity) {
    size_t capacity = students_capacity + 2;
    Student *temp = realloc(students, sizeof(Student) * capacity);
    if (temp == NULL) {
      return false;
    }
    students = temp;
    students_capacity = capacity;
  }

  students[students_length++] = *s;
  return true;
}

static void add_location_header(HttpResponse *res, int id) {
  char location[32] = {0};
  sprintf_s(location, sizeof(location), "/students?id=%d", id);
  headers_add(res_get_headers(res), "Content-Location", location);
}

static void students_get(HttpRequest *req, HttpResponse *res) {
  Context *context = res_get_context(res);
  if (context != NULL) {
    write_context_body(res, context);
    return;
  }

  const HttpQuery *query = req_get_query(req);
  const char *id = query_get(query, "id");
  int id_num = -1;
  if (id != NULL && !parse_id(id, &id_num)) {
    write_bad_request(res);
    return;
  }

  const Student *match = NULL;
  if (id != NULL) {
    for (size_t i = 0; i < students_length; i++) {
      if (students[i].id == id_num) {
        match = &students[i];
        break;
      }
    }
    if (match == NULL) {
      write_not_found(res);
      return;
    }
  }

  // Longest student object: {"id":-123456789,"fname":"<49>","lname":"<49>",
  // "grade":-99} is under 160 chars.
  const size_t max_student_json = 160;
  size_t count = match != NULL ? 1 : students_length;
  size_t size = 2 + count * (max_student_json + 1) + 1;

  context = malloc(sizeof(Context));
  char *json = malloc(size);
  if (context == NULL || json == NULL) {
    free(context);
    free(json);
    write_server_error(res);
    return;
  }

  size_t index = 0;
  if (match != NULL) {
    index += student_to_json(json, size, match);
  } else {
    json[index++] = '[';
    for (size_t i = 0; i < students_length; i++) {
      if (i > 0) {
        json[index++] = ',';
      }
      index += student_to_json(json + index, size - index, &students[i]);
    }
    json[index++] = ']';
    json[index] = 0;
  }

  context->body = json;
  context->length = index;
  context->sent = 0;
  context->reading = false;
  res_set_context(res, context);

  res_set_status_code(res, HTTP_STATUS_OK);
  HttpHeaders *headers = res_get_headers(res);
  headers_add(headers, "Content-Type", "application/json; charset=utf-8");
  char len[50] = {0};
  sprintf_s(len, sizeof(len), "%zu", context->length);
  headers_add(headers, "Content-Length", len);
  res_finish_headers(res);
}

static void students_post(HttpRequest *req, HttpResponse *res) {
  Context *context = get_or_create_context(res);
  if (context == NULL) {
    return;
  }

  if (!context->reading) {
    write_context_body(res, context);
    return;
  }

  BodyStatus status = read_body(req, context);
  if (status == BODY_PENDING) {
    return;
  }
  context->reading = false;

  Student s = {0};
  bool has_id = false;
  if (status == BODY_ERROR || !parse_student(context->body, &s, true, &has_id)) {
    write_bad_request(res);
    return;
  }

  for (size_t i = 0; i < students_length; i++) {
    if (students[i].id == s.id) {
      write_bad_request(res);
      return;
    }
  }

  if (!add_student(&s)) {
    write_server_error(res);
    return;
  }

  // Respond with the created student, in the same format as GET.
  char json[160] = {0};
  int json_length = student_to_json(json, sizeof(json), &s);
  if (json_length < 0) {
    write_server_error(res);
    return;
  }
  if ((size_t)json_length > context->length) {
    char *temp = realloc(context->body, (size_t)json_length + 1);
    if (temp == NULL) {
      write_server_error(res);
      return;
    }
    context->body = temp;
  }
  strcpy_s(context->body, (size_t)json_length + 1, json);
  context->length = (size_t)json_length;
  context->sent = 0;

  res_set_status_code(res, HTTP_STATUS_CREATED);
  HttpHeaders *headers = res_get_headers(res);
  headers_add(headers, "Content-Type", "application/json; charset=utf-8");
  char len[50] = {0};
  sprintf_s(len, sizeof(len), "%zu", context->length);
  headers_add(headers, "Content-Length", len);
  add_location_header(res, s.id);
  headers_add(headers, "Cache-Control", "no-cache");
  res_finish_headers(res);
  write_context_body(res, context);
}

static void students_put(HttpRequest *req, HttpResponse *res) {
  int id_num = -1;
  if (!parse_id(query_get(req_get_query(req), "id"), &id_num)) {
    write_bad_request(res);
    return;
  }

  Context *context = get_or_create_context(res);
  if (context == NULL) {
    return;
  }

  BodyStatus status = read_body(req, context);
  if (status == BODY_PENDING) {
    return;
  }
  context->reading = false;

  // The id comes from the query string. It may also be in the body, but then
  // it has to match.
  Student s = {0};
  bool has_id = false;
  if (status == BODY_ERROR ||
      !parse_student(context->body, &s, false, &has_id) ||
      (has_id && s.id != id_num)) {
    write_bad_request(res);
    return;
  }
  s.id = id_num;

  for (size_t i = 0; i < students_length; i++) {
    if (students[i].id == s.id) {
      students[i] = s;
      res_set_status_code(res, HTTP_STATUS_NO_CONTENT);
      add_location_header(res, s.id);
      res_finish_headers(res);
      res_finish_write(res);
      return;
    }
  }

  if (!add_student(&s)) {
    write_server_error(res);
    return;
  }

  res_set_status_code(res, HTTP_STATUS_CREATED);
  add_location_header(res, s.id);
  res_finish_headers(res);
  res_finish_write(res);
}

static void students_delete(HttpRequest *req, HttpResponse *res) {
  int id_num = -1;
  if (!parse_id(query_get(req_get_query(req), "id"), &id_num)) {
    write_bad_request(res);
    return;
  }

  for (size_t i = 0; i < students_length; i++) {
    if (students[i].id == id_num) {
      Student zero = {0};
      students[i] = students[students_length - 1];
      students[students_length - 1] = zero;
      students_length--;

      res_set_status_code(res, HTTP_STATUS_NO_CONTENT);
      res_finish_headers(res);
      res_finish_write(res);
      return;
    }
  }

  write_not_found(res);
}

int main() {
  if (!init_html_dir()) {
    printf("Server: could not find the executable's directory\n");
    return 1;
  }

  students = malloc(sizeof(Student) * 2);
  if (students == NULL) {
    return 1;
  }
  students_capacity = 2;
  students_length = 2;
  students[0].id = 903488341;
  strcpy_s(students[0].fname, 50, "Jon");
  strcpy_s(students[0].lname, 50, "Doe");
  students[0].grade = 85;
  students[1].id = 522333421;
  strcpy_s(students[1].fname, 50, "Mick");
  strcpy_s(students[1].lname, 50, "Salvaski");
  students[1].grade = 94;

  WebServer *server = create_web_server();
  web_server_map_get(server, "/", root, (void (*)(void *))root_cleanup);
  web_server_map_get(server, "/students", students_get,
                     (void (*)(void *))cleanup);
  web_server_map_post(server, "/students", students_post,
                      (void (*)(void *))cleanup);
  web_server_map_put(server, "/students", students_put,
                     (void (*)(void *))cleanup);
  web_server_map_delete(server, "/students", students_delete, NULL);
  int res = web_server_run(server);

  free(students);
  return res;
}
