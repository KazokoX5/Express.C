#ifndef REQPARSER_H
#define REQPARSER_H

#include <openssl/ssl.h>
#include <stddef.h>
#include <sys/types.h>
#include "stringstu.h"



#define MAX_HEADERS 32
#define HEADER_NAME_SIZE 64
#define HEADER_VALUE_SIZE 256
#define MAX_ROUTES 64
#define MAX_MIDDLEWARE 16
#define QUERIES_MAX 16
#define PARAMS_MAX 16

#ifndef HTTP_PATH_SIZE
#define HTTP_PATH_SIZE 1024
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// DEFAULT ERROR BODIES
#define default_ERROR_400 "<html><body><h1>400 Bad Request</h1></body></html>"
#define default_ERROR_403 "<html><body><h1>403 Forbidden</h1></body></html>"
#define default_ERROR_404 "<html><body><h1>404 Not Found</h1></body></html>"
#define default_ERROR_405 "<html><body><h1>405 Method Not Allowed</h1></body></html>"
#define default_ERROR_500 "<html><body><h1>500 Internal Server Error</h1></body></html>"

// ─── INTERNAL ────────────────────────────────────────────────────────────────

typedef struct {
    int fd;
    SSL *ssl;
} connection_t;

ssize_t conn_read(connection_t *conn, void *buf, size_t len);
ssize_t conn_write(connection_t *conn, const void *buf, size_t len);
void    conn_close(connection_t *conn);

// ─── HTTP TYPES ──────────────────────────────────────────────────────────────

typedef struct {
    char name[HEADER_NAME_SIZE];
    char value[HEADER_VALUE_SIZE];
} http_header_t;

typedef struct {
    char name[255];
    char value[255];
} path_query_t;

typedef struct {
    path_query_t queries[QUERIES_MAX];
    int query_count;
} path_queries_t;

typedef struct {
    char method[16];
    char path[HTTP_PATH_SIZE];
    char version[16];
    http_header_t headers[MAX_HEADERS];
    int header_count;
    path_queries_t queries;
    string_t body;
} http_request_t;

typedef struct {
    int status_code;
    const char *status_text;
    http_header_t headers[MAX_HEADERS];
    int header_count;
    string_t body;
} http_response_t;

typedef struct {
    char key[64];
    char value[256];
} route_param_t;

typedef struct {
    route_param_t params[PARAMS_MAX];
    int count;
} route_params_t;

// ─── PARSER ──────────────────────────────────────────────────────────────────

int         http_request_init(http_request_t *req);
int         http_parse_request(const char *raw, size_t raw_len, http_request_t *req);
const char *http_request_get_header_value(const http_request_t *req, const char *name);
void        http_request_free(http_request_t *req);
void        parse_path_queries(char *path, http_request_t *req);
void        url_decode(char *str);

// ─── RESPONSE BUILDER ────────────────────────────────────────────────────────

void        http_response_init(http_response_t *res);
void        http_response_set_status(http_response_t *res, int code, const char *text);
void        http_response_add_header(http_response_t *res, const char *name, const char *value);
void        http_response_set_body(http_response_t *res, const char *data, size_t length);
int         http_response_send(http_response_t *res, connection_t *conn);
void        http_response_free(http_response_t *res);
const char *http_response_get_header_value(const http_response_t *res, const char *name);
int         delete_header(http_response_t *res, const char *name);

// ─── ROUTE PARAMS ────────────────────────────────────────────────────────────

const char *route_params_get(route_params_t *params, const char *key);

// ─── SERVER FORWARD DECLARE(Cuz they refference eachother) ──────────────────────────────────────────────────


typedef struct server_t server_t;
typedef struct route_t route_t;

// ─── FUNCTION POINTER TYPES ──────────────────────────────────────────────────

typedef void (*route_handler_t)(connection_t *conn, http_request_t *req, http_response_t *res, route_params_t *params);

typedef void (*error_handler_t)(connection_t *conn, http_response_t *res, int status_code);

typedef void (*middleware_t)(http_request_t *req, http_response_t *res, server_t *srv, route_t *route, int current);

// ─── ROUTE ───────────────────────────────────────────────────────────────────

struct route_t {
    char method[16];
    char path[HTTP_PATH_SIZE];
    route_handler_t handler;
    route_params_t params;
    middleware_t middleware[MAX_MIDDLEWARE];
    int middleware_count;
    int use_global;    // 0 = opt out, 1 = opt in (default 1)
    int global_first;  // 1 = global runs before route middleware (default 1)
};

// ─── SERVER ──────────────────────────────────────────────────────────────────

typedef struct {
    int http_port;
    int https_port;
    const char *cert;
    const char *key;
    const char *static_dir;
    int max_clients;
} server_config_t;

struct server_t {
    server_config_t config;
    int http_fd;
    int https_fd;
    SSL_CTX *ctx;
    connection_t curr_conn;
    route_t routes[MAX_ROUTES];
    int route_count;
    error_handler_t error_handlers[512];
    middleware_t global_middleware[MAX_MIDDLEWARE];
    int global_middleware_count;
};

// ─── LIFECYCLE ───────────────────────────────────────────────────────────────

server_t *server_create(server_config_t *config);
int       server_prepare(server_t *srv);
void      server_run(server_t *srv);
void      server_close(server_t *srv);

// ─── ROUTING ─────────────────────────────────────────────────────────────────
void serve_static(server_t *srv, const char * path, http_response_t *res);
void server_get(server_t *srv, const char *path, route_handler_t handler);
void server_post(server_t *srv, const char *path, route_handler_t handler);
void server_put(server_t *srv, const char *path, route_handler_t handler);
void server_delete(server_t *srv, const char *path, route_handler_t handler);

// ─── MIDDLEWARE ───────────────────────────────────────────────────────────────

void server_use(server_t *srv, middleware_t m);
void route_use(server_t *srv, const char *method, const char *path, middleware_t m);
void chain_next(http_request_t *req, http_response_t *res, server_t *srv, route_t *route, int current);
void route_set_global(server_t *srv, const char *method, const char *path, int use_global);
void route_set_global_order(server_t *srv, const char *method, const char *path, int global_first);

// ─── ERROR HANDLERS ──────────────────────────────────────────────────────────

void check_error(server_t *srv, http_response_t *res, int status_code);
void server_set_error_handler(server_t *srv, int status_code, error_handler_t handler);

#endif