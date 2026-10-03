# HTTP server in C

An HTTP/1.1 server written from scratch in C on top of Winsock: non-blocking sockets, a single-threaded `select()` event loop, its own request parser and a small routing layer. No HTTP libraries.

It started as a project for my university course *Introduction to Computer Communication* and serves a static page in three languages and a small JSON REST API.

## Features

- **Event loop:** one thread, non-blocking sockets and `select()`; up to 60 connections at once
- **Incremental request parsing:** the method, target, version, headers and body are parsed as bytes arrive, so requests split across TCP packets work
- **Fixed-size ring buffers** (256 bytes) per connection for receiving and sending; partial `send()`s keep the unsent bytes for the next round
- **Routing** by method and path, with a handler API for streaming responses (`res_can_write` / `res_wirte` / `res_finish_write`)
- **Automatic `OPTIONS`** (with an `Allow` header) and **`HEAD`** for every route that has a `GET`, plus **`TRACE`**
- **Timeouts:** connections are closed after 30 seconds
- Methods: `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, `OPTIONS`, `TRACE`

## Architecture

```
            ┌──────────────────────── select() loop ────────────────────────┐
 accept ──► │ per connection:                                               │
            │   recv() ──► recv ring buffer ──► request parser (state       │
            │                                   machine: method → target →  │
            │                                   version → headers → body)   │
            │                                          │                    │
            │                                   route lookup                │
            │                                          ▼                    │
            │   send() ◄── send ring buffer ◄── handler (called again until │
            │                                   it finishes the response)   │
            └───────────────────────────────────────────────────────────────┘
```

Handlers never block. A handler that is waiting for more body data, or for space in the send buffer, returns and is called again on the next loop iteration. State that has to survive between calls is kept in a per-response context that the server frees with the route's cleanup function.

| File | Contents |
|---|---|
| `web-server/web_server.c` | Event loop, ring buffers, request parser, response writer, routing, built-in `OPTIONS` / `HEAD` / `TRACE` |
| `web-server/web-server-internal/` | Header and query-string collections |
| `web-server/*.h` | Public API: request, response, headers, query, methods, status codes |
| `main.c` | The example application: the static page and the `/students` API |
| `html/` | The page in English, Hebrew and French |

## Building and running

Requirements: Windows and [clang](https://releases.llvm.org/) or MSVC.

```bat
build.bat
bin\server\server.exe
```

Or with MSVC from a Developer Command Prompt:

```bat
mkdir bin\server
cl /W4 /Fe:bin\server\server.exe main.c web-server\web_server.c web-server\web-server-internal\*.c
bin\server\server.exe
```

The server listens on `http://localhost:8080`.

## API

### `GET /?lang={en|he|fr}`

Returns the static page in the requested language (default English), with a matching `Content-Language` header.

### Students

A student looks like this:

```json
{"id":123456789,"fname":"Dana","lname":"Levi","grade":95}
```

| Request | Result |
|---|---|
| `GET /students` | `200` with all students as a JSON array |
| `GET /students?id={id}` | `200` with the student, or `404` |
| `POST /students` with a student in the body | `201` with the created student and a `Content-Location` header; `400` if the id already exists or the body is invalid |
| `PUT /students?id={id}` with `{"fname":…,"lname":…,"grade":…}` | `204` if the student was updated, `201` if it was created. The body may also contain `"id"`, but it must match the query string. |
| `DELETE /students?id={id}` | `204`, or `404` if there is no such student |
| `OPTIONS /students` | `204` with `Allow: GET, OPTIONS, HEAD, POST, PUT, DELETE` |

Examples:

```sh
curl http://localhost:8080/students
curl -X POST http://localhost:8080/students -d '{"id":123456789,"fname":"Dana","lname":"Levi","grade":95}'
curl -X PUT "http://localhost:8080/students?id=123456789" -d '{"fname":"Dana","lname":"Cohen","grade":99}'
curl -X DELETE "http://localhost:8080/students?id=123456789"
curl -I http://localhost:8080/
```

## Limitations

- Windows only (Winsock).
- One request per connection: the connection is closed after the response (no keep-alive or pipelining).
- The JSON parser is minimal: whitespace is allowed, but the keys must be in the order shown above, names may contain only letters and spaces, and bodies are limited to 1024 bytes.
- Students are kept in memory and are lost when the server stops.
- `select()` waits without a timeout, so idle connections are only checked for the 30-second timeout when other activity wakes the loop.
