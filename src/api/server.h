#ifndef API_SERVER_H
#define API_SERVER_H

#include <stddef.h>

/* --- HTTP API server ---
 *
 * Embedded HTTP server (libmicrohttpd) serving a JSON REST API.
 * Runs on both TCP (for web UI / remote) and unix socket (for CLI).
 * Routes are registered by subsystems at startup.
 * Static files served from a configurable directory (React bundle). */

/* HTTP methods */
typedef enum {
    API_GET,
    API_POST,
    API_PUT,
    API_DELETE,
} api_method_t;

/* Request context passed to route handlers */
typedef struct {
    api_method_t  method;
    const char   *url;
    const char   *body;        /* POST/PUT body (JSON), NULL for GET/DELETE */
    size_t        body_len;
    const char   *auth_token;  /* from Authorization header, NULL if absent */
} api_request_t;

/* Response built by route handlers */
typedef struct {
    int           status;      /* HTTP status code (200, 400, 404, etc.) */
    char         *body;        /* JSON response body (malloc'd, server frees) */
    const char   *content_type; /* default: "application/json" */
} api_response_t;

/* Route handler function */
typedef api_response_t (*api_handler_fn)(const api_request_t *req, void *userdata);

/* Opaque server handle */
typedef struct api_server api_server_t;

/* Create the API server.
 * tcp_port: port for web UI (0 = disabled).
 * socket_path: unix socket for CLI (NULL = disabled).
 * static_dir: directory for static files (NULL = disabled).
 * Returns NULL on failure. */
api_server_t *api_server_create(int tcp_port, const char *socket_path,
                                const char *static_dir);

/* Register a route handler.
 * pattern: URL pattern (exact match, e.g., "/api/status").
 * method: HTTP method to match.
 * handler: function to call.
 * userdata: passed to handler on each request. */
int api_server_route(api_server_t *srv, const char *pattern,
                     api_method_t method, api_handler_fn handler,
                     void *userdata);

/* Start serving (non-blocking — runs in background threads via libmicrohttpd). */
int api_server_start(api_server_t *srv);

/* Stop the server and free resources. */
void api_server_stop(api_server_t *srv);

/* --- Response helpers --- */

/* Build a JSON success response. Caller provides the JSON body string (malloc'd). */
api_response_t api_ok(char *json_body);

/* Build a JSON success response with status code. */
api_response_t api_ok_status(int status, char *json_body);

/* Build a JSON error response. Message is copied internally. */
api_response_t api_error(int status, const char *message);

/* Build a 204 No Content response. */
api_response_t api_no_content(void);

#endif
