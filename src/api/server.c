#include "api/server.h"
#include <cutils/log.h>
#include <cutils/error.h>
#include <cJSON.h>

#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* --- Route table --- */

#define MAX_ROUTES 64

typedef struct {
    char          pattern[128];
    api_method_t  method;
    api_handler_fn handler;
    void         *userdata;
} route_entry_t;

/* --- POST body accumulator --- */

typedef struct {
    char  *data;
    size_t len;
    size_t capacity;
} post_body_t;

/* --- Server state --- */

struct api_server {
    struct MHD_Daemon *tcp_daemon;
    struct MHD_Daemon *unix_daemon;
    char              *socket_path;
    char              *static_dir;
    route_entry_t      routes[MAX_ROUTES];
    int                nroutes;
};

/* --- Helpers --- */

static api_method_t method_from_string(const char *method)
{
    if (strcmp(method, "GET") == 0)    return API_GET;
    if (strcmp(method, "POST") == 0)   return API_POST;
    if (strcmp(method, "PUT") == 0)    return API_PUT;
    if (strcmp(method, "DELETE") == 0) return API_DELETE;
    return API_GET;
}

static const route_entry_t *find_route(const api_server_t *srv,
                                       const char *url, api_method_t method)
{
    for (int i = 0; i < srv->nroutes; i++) {
        if (srv->routes[i].method == method &&
            strcmp(srv->routes[i].pattern, url) == 0)
            return &srv->routes[i];
    }
    return NULL;
}

static const char *get_header(struct MHD_Connection *conn, const char *name)
{
    return MHD_lookup_connection_value(conn, MHD_HEADER_KIND, name);
}

static enum MHD_Result send_response(struct MHD_Connection *conn,
                                     int status, const char *content_type,
                                     const char *body)
{
    size_t len = body ? strlen(body) : 0;
    /* MHD copies the buffer with RESPMEM_MUST_COPY, safe to discard const */
    char *buf = body ? strdup(body) : NULL;
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        len, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) return MHD_NO;

    if (content_type)
        MHD_add_response_header(resp, "Content-Type", content_type);
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");

    enum MHD_Result ret = MHD_queue_response(conn, (unsigned int)status, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* --- Static file serving --- */

static enum MHD_Result try_serve_static(struct MHD_Connection *conn,
                                        const char *url,
                                        const char *static_dir)
{
    if (!static_dir) return MHD_NO;

    /* Security: reject paths with ".." */
    if (strstr(url, "..")) return MHD_NO;

    /* Build file path */
    char path[512];
    if (strcmp(url, "/") == 0)
        snprintf(path, sizeof(path), "%s/index.html", static_dir);
    else
        snprintf(path, sizeof(path), "%s%s", static_dir, url);

    FILE *f = fopen(path, "rb");
    if (!f) return MHD_NO;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        return MHD_NO;
    }

    char *buf = malloc((size_t)fsize);
    if (!buf) {
        fclose(f);
        return MHD_NO;
    }

    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    /* Guess content type from extension */
    const char *ct = "application/octet-stream";
    const char *ext = strrchr(path, '.');
    if (ext) {
        if (strcmp(ext, ".html") == 0) ct = "text/html";
        else if (strcmp(ext, ".js") == 0) ct = "application/javascript";
        else if (strcmp(ext, ".css") == 0) ct = "text/css";
        else if (strcmp(ext, ".json") == 0) ct = "application/json";
        else if (strcmp(ext, ".png") == 0) ct = "image/png";
        else if (strcmp(ext, ".svg") == 0) ct = "image/svg+xml";
        else if (strcmp(ext, ".ico") == 0) ct = "image/x-icon";
    }

    struct MHD_Response *resp = MHD_create_response_from_buffer(
        nread, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) {
        free(buf);
        return MHD_NO;
    }

    MHD_add_response_header(resp, "Content-Type", ct);
    enum MHD_Result ret = MHD_queue_response(conn, 200, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* --- Request handler (called by libmicrohttpd) --- */

static enum MHD_Result request_handler(void *cls,
                                       struct MHD_Connection *conn,
                                       const char *url,
                                       const char *method,
                                       const char *version,
                                       const char *upload_data,
                                       size_t *upload_data_size,
                                       void **req_cls)
{
    (void)version;
    api_server_t *srv = cls;

    /* First call: allocate POST body accumulator */
    if (*req_cls == NULL) {
        post_body_t *pb = calloc(1, sizeof(*pb));
        *req_cls = pb;
        return MHD_YES;
    }

    post_body_t *pb = *req_cls;

    /* Accumulate POST/PUT body */
    if (*upload_data_size > 0) {
        size_t needed = pb->len + *upload_data_size + 1;
        if (needed > pb->capacity) {
            size_t newcap = needed > pb->capacity * 2 ? needed : pb->capacity * 2;
            if (newcap < 1024) newcap = 1024;
            char *tmp = realloc(pb->data, newcap);
            if (!tmp) {
                *upload_data_size = 0;
                return MHD_YES;
            }
            pb->data = tmp;
            pb->capacity = newcap;
        }
        memcpy(pb->data + pb->len, upload_data, *upload_data_size);
        pb->len += *upload_data_size;
        pb->data[pb->len] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* Body complete — dispatch */
    api_method_t api_method = method_from_string(method);

    /* Try API routes first */
    const route_entry_t *route = find_route(srv, url, api_method);
    if (route) {
        api_request_t req = {
            .method     = api_method,
            .url        = url,
            .body       = pb->data,
            .body_len   = pb->len,
            .auth_token = get_header(conn, "Authorization"),
        };

        api_response_t resp = route->handler(&req, route->userdata);

        enum MHD_Result ret = send_response(conn, resp.status,
            resp.content_type ? resp.content_type : "application/json",
            resp.body);

        free(resp.body);
        free(pb->data);
        free(pb);
        *req_cls = NULL;
        return ret;
    }

    /* Try static files for GET requests */
    if (api_method == API_GET) {
        enum MHD_Result ret = try_serve_static(conn, url, srv->static_dir);
        if (ret == MHD_YES) {
            free(pb->data);
            free(pb);
            *req_cls = NULL;
            return ret;
        }
    }

    /* 404 */
    free(pb->data);
    free(pb);
    *req_cls = NULL;
    return send_response(conn, 404, "application/json",
                         "{\"error\":\"not found\"}");
}

/* --- Public API --- */

api_server_t *api_server_create(int tcp_port, const char *socket_path,
                                const char *static_dir)
{
    api_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    if (socket_path)
        srv->socket_path = strdup(socket_path);
    if (static_dir)
        srv->static_dir = strdup(static_dir);

    /* TCP daemon */
    if (tcp_port > 0) {
        srv->tcp_daemon = MHD_start_daemon(
            MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_EPOLL,
            (uint16_t)tcp_port,
            NULL, NULL,
            request_handler, srv,
            MHD_OPTION_END);

        if (!srv->tcp_daemon) {
            log_error("failed to start HTTP server on port %d", tcp_port);
            api_server_stop(srv);
            return NULL;
        }
        log_info("HTTP server listening on port %d", tcp_port);
    }

    /* Unix socket daemon */
    if (socket_path) {
        /* Remove stale socket */
        unlink(socket_path);

        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            log_error("failed to create unix socket");
            api_server_stop(srv);
            return NULL;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            log_error("failed to bind unix socket at %s", socket_path);
            close(sock);
            api_server_stop(srv);
            return NULL;
        }

        if (listen(sock, 8) < 0) {
            log_error("failed to listen on unix socket");
            close(sock);
            unlink(socket_path);
            api_server_stop(srv);
            return NULL;
        }

        srv->unix_daemon = MHD_start_daemon(
            MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_EPOLL,
            0,  /* port ignored for pre-bound socket */
            NULL, NULL,
            request_handler, srv,
            MHD_OPTION_LISTEN_SOCKET, sock,
            MHD_OPTION_END);

        if (!srv->unix_daemon) {
            log_error("failed to start HTTP server on unix socket %s", socket_path);
            close(sock);
            unlink(socket_path);
            api_server_stop(srv);
            return NULL;
        }
        log_info("API socket listening at %s", socket_path);
    }

    return srv;
}

int api_server_route(api_server_t *srv, const char *pattern,
                     api_method_t method, api_handler_fn handler,
                     void *userdata)
{
    if (srv->nroutes >= MAX_ROUTES)
        return set_error(CUTILS_ERR, "too many API routes");

    route_entry_t *r = &srv->routes[srv->nroutes++];
    snprintf(r->pattern, sizeof(r->pattern), "%s", pattern);
    r->method = method;
    r->handler = handler;
    r->userdata = userdata;

    return CUTILS_OK;
}

int api_server_start(api_server_t *srv)
{
    /* libmicrohttpd daemons are already running from create */
    (void)srv;
    return CUTILS_OK;
}

void api_server_stop(api_server_t *srv)
{
    if (!srv) return;

    if (srv->tcp_daemon) {
        MHD_stop_daemon(srv->tcp_daemon);
        srv->tcp_daemon = NULL;
    }

    if (srv->unix_daemon) {
        MHD_stop_daemon(srv->unix_daemon);
        srv->unix_daemon = NULL;
    }

    if (srv->socket_path) {
        unlink(srv->socket_path);
        free(srv->socket_path);
    }

    free(srv->static_dir);
    free(srv);
}

/* --- Response helpers --- */

api_response_t api_ok(char *json_body)
{
    return (api_response_t){
        .status = 200,
        .body = json_body,
        .content_type = "application/json",
    };
}

api_response_t api_ok_status(int status, char *json_body)
{
    return (api_response_t){
        .status = status,
        .body = json_body,
        .content_type = "application/json",
    };
}

api_response_t api_error(int status, const char *message)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "error", message);
    char *body = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    return (api_response_t){
        .status = status,
        .body = body,
        .content_type = "application/json",
    };
}

api_response_t api_no_content(void)
{
    return (api_response_t){
        .status = 204,
        .body = NULL,
        .content_type = NULL,
    };
}
