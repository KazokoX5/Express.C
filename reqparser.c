#include "reqparser.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <time.h>
#include <openssl/err.h>
#include <sys/wait.h>   
#include <errno.h>      
#include <ctype.h>
#define BUFFER_SIZE 16384




//TODO: Improve file limits
long get_file_size(FILE *f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    return size;
}


const char *get_content_type(const char *path) {
    const char *check = strrchr(path, '/');
    if (!check) check = path;
    const char *ext = strrchr(check, '.');
    if (ext) {
        // text
        if (strcasecmp(ext, ".html") == 0) return "text/html";
        if (strcasecmp(ext, ".css")  == 0) return "text/css";
        if (strcasecmp(ext, ".js")   == 0) return "application/javascript";
        if (strcasecmp(ext, ".json") == 0) return "application/json";
        if (strcasecmp(ext, ".txt")  == 0) return "text/plain";
        if (strcasecmp(ext, ".xml")  == 0) return "application/xml";
        if (strcasecmp(ext, ".csv")  == 0) return "text/csv";
        // images
        if (strcasecmp(ext, ".png")  == 0) return "image/png";
        if (strcasecmp(ext, ".jpg")  == 0) return "image/jpeg";
        if (strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
        if (strcasecmp(ext, ".gif")  == 0) return "image/gif";
        if (strcasecmp(ext, ".webp") == 0) return "image/webp";
        if (strcasecmp(ext, ".svg")  == 0) return "image/svg+xml";
        if (strcasecmp(ext, ".ico")  == 0) return "image/x-icon";
        // fonts
        if (strcasecmp(ext, ".ttf")  == 0) return "font/ttf";
        if (strcasecmp(ext, ".woff") == 0) return "font/woff";
        if (strcasecmp(ext, ".woff2")== 0) return "font/woff2";
        // audio/video
        if (strcasecmp(ext, ".mp3")  == 0) return "audio/mpeg";
        if (strcasecmp(ext, ".mp4")  == 0) return "video/mp4";
        if (strcasecmp(ext, ".webm") == 0) return "video/webm";
        // other
        if (strcasecmp(ext, ".pdf")  == 0) return "application/pdf";
        if (strcasecmp(ext, ".zip")  == 0) return "application/zip";
        if (strcasecmp(ext, ".wasm") == 0) return "application/wasm";
    }
    return "application/octet-stream";  // unknown binary — download
}

// Check if path starts with base
int check_filepath(const char *base, const char *path) {
    char resolved_base[PATH_MAX], resolved_path[PATH_MAX];
    if (!realpath(base, resolved_base) || !realpath(path, resolved_path)) {
        return 0; // Invalid path
    }
    return strncmp(resolved_base, resolved_path, strlen(resolved_base)) == 0;
}

void parse_path_queries(char *path, http_request_t *req) {
    char *q = strstr(path, "?");
    if (!q) return;
    *q = '\0';
    char *query = q + 1;

    char *outer_save;                          // saveptr owned by the outer loop
    char *pair = strtok_r(query, "&", &outer_save);

    while (pair && req->queries.query_count < QUERIES_MAX) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';

            path_query_t *qp = &req->queries.queries[req->queries.query_count];

            strncpy(qp->name, pair, sizeof(qp->name) - 1);
            qp->name[sizeof(qp->name) - 1] = '\0';
            url_decode(qp->name);

            strncpy(qp->value, eq + 1, sizeof(qp->value) - 1);
            qp->value[sizeof(qp->value) - 1] = '\0';
            url_decode(qp->value);

            req->queries.query_count++;
        }
        pair = strtok_r(NULL, "&", &outer_save);  // resume outer loop
    }
}

void url_decode(char *str)
{
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '%' &&
            isxdigit(src[1]) &&
            isxdigit(src[2])) {

            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (*src == '+') {
            *dst++ = ' ';
            src++;
        }
        else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}




/*
PARSER STUFF
*/

int http_request_init(http_request_t *req){
    if (req) {
        memset(req, 0, sizeof(http_request_t));
        return 0;
    }
    return -1;
}


int http_parse_request(const char *raw, size_t raw_len, http_request_t *req){
    if (!raw || !req) return -1;
    memset(req, 0, sizeof(http_request_t)); // Clear the request struct

    const char *line_end = strstr(raw, "\r\n");
    if (!line_end) return -1;
    const char *header_end = strstr(raw, "\r\n\r\n");
    if (!header_end) return -1;

    if (sscanf(raw, "%15s %1023s %15s", req->method, req->path, req->version) != 3) return -1;


    parse_path_queries(req->path,req);
    url_decode(req->path);

    const char *cursor = line_end + 2; // Move past the first line, since we already ate it
    while (cursor < header_end && req->header_count < MAX_HEADERS) {

        const char *end = strstr(cursor, "\r\n");
        if (!end) return -1;
        const char *colon = memchr(cursor, ':', end - cursor);
        if (!colon || colon > end) return -1;

        http_header_t *h = &req->headers[req->header_count++];
        size_t name_len = colon - cursor;
        if (name_len >= HEADER_NAME_SIZE) return -1;
        memcpy(h->name, cursor, name_len);
        h->name[name_len] = '\0';

        while(colon[1] == ' ') colon++; // Skip spaces after colon
        size_t value_len = end - (colon + 1);
        if (value_len >= HEADER_VALUE_SIZE) return -1;
        memcpy(h->value, colon + 1, value_len);
        h->value[value_len] = '\0';


        cursor = end + 2; // Move to the next line(Skip \r\n)
    }

    const char *body_start = header_end + 4;
    size_t remaining = raw_len - (body_start - raw);
    if (remaining > 0) {
        req->body.data = malloc(remaining);
        if (!req->body.data) return -1;
        memcpy(req->body.data, body_start, remaining);
        req->body.length = remaining;
    }

    if (strcmp(req->path, "/") == 0) strcpy(req->path, "/index.html");
    return 0;
}








/*
RESPONSE STUFF
*/


void http_response_init(http_response_t *res){
    if (res) {
        res->status_code = 200;
        res->status_text = "OK";
        res->header_count = 0;
        res->body.data = NULL;
        res->body.length = 0;
    }
}

void http_response_set_status(http_response_t *res, int code, const char *text){
    if (res){
        res->status_code = code;
        res->status_text = text;
    }
}

void http_response_add_header(http_response_t *res, const char *name, const char *value){
    if (res && res->header_count < MAX_HEADERS) {
        strncpy(res->headers[res->header_count].name, name, HEADER_NAME_SIZE - 1);
        res->headers[res->header_count].name[HEADER_NAME_SIZE - 1] = '\0';
        strncpy(res->headers[res->header_count].value, value, HEADER_VALUE_SIZE - 1);
        res->headers[res->header_count].value[HEADER_VALUE_SIZE - 1] = '\0';
        res->header_count++;
    }
}


void http_response_set_body(http_response_t *res, const char *data, size_t length){
    if (res) {
        if (res->body.data) http_response_free(res);

        res->body.data = malloc(length);
        if (res->body.data) {
            memcpy(res->body.data, data, length);
            res->body.length = length;
        } else {
            res->body.length = 0;
        }
    }
}

int  http_response_send(http_response_t *res, connection_t *conn){
    if (!res) return -1;
    //Add the content length header automatically
    if (res->body.data && res->body.length > 0 && !http_response_get_header_value(res, "Content-Length")) {
        char cl[32];
        snprintf(cl, sizeof(cl), "%zu", res->body.length);
        http_response_add_header(res, "Content-Length", cl);
    }
    if (!http_response_get_header_value(res, "Connection")){
        http_response_add_header(res, "Connection", "close");
    }
    char header[4096];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n",
                              res->status_code, res->status_text);

    for (int i = 0; i < res->header_count; i++) {
        header_len += snprintf(header + header_len, sizeof(header) - header_len,
                               "%s: %s\r\n",
                               res->headers[i].name, res->headers[i].value);
    }


    // Final blank line
    header_len += snprintf(header + header_len, sizeof(header) - header_len, "\r\n");
    // Send the header
    if (conn_write(conn, header, header_len) < 0) return -1;
    // Send the body
    if (res->body.data && conn_write(conn, res->body.data, res->body.length) < 0) return -1;
    return 0;
}



/*
HELPERS
*/


const char *http_request_get_header_value(const http_request_t *req, const char *name){
    if (!req || !name) return NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

const char *http_response_get_header_value(const http_response_t *res, const char *name) {
    if (!res || !name) return NULL;
    for (int i = 0; i < res->header_count; i++) {
        if (strcasecmp(res->headers[i].name, name) == 0)
            return res->headers[i].value;
    }
    return NULL;
}

void http_request_free(http_request_t *req){
    if (req) string_free(&req->body);
}

void http_response_free(http_response_t *res){
    if (res) string_free(&res->body);
}


int delete_header(http_response_t *res, const char *name){
    if (!res || !name) return -1;
    for (int i = 0; i < res->header_count; i++) {
        if (strcasecmp(res->headers[i].name, name) == 0) {
            // Shift remaining headers down
            for (int j = i; j < res->header_count - 1; j++) {
                res->headers[j] = res->headers[j + 1];
            }
            res->header_count--;
            return 0;
        }
    }
    return -1;
}



/*
HANDLER
*/








ssize_t conn_read(connection_t *conn, void *buf, size_t len) {
    if (conn->ssl) return SSL_read(conn->ssl, buf, len);
    return recv(conn->fd, buf, len, 0);
}



ssize_t conn_write(connection_t *conn, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n;

        if (conn->ssl) n = SSL_write(conn->ssl, (const char *)buf + sent, len - sent);
        else n = send(conn->fd, (const char *)buf + sent, len - sent, 0);

        if (n <= 0) return -1;
        sent += n;
    }
    return (ssize_t)sent;
}

void conn_close(connection_t *conn) {
    if (conn->ssl) { SSL_shutdown(conn->ssl); SSL_free(conn->ssl); }
    close(conn->fd);
}


// Default error handlers

void default_400(connection_t *conn, http_response_t *res, int status_code) {
    http_response_init(res);
    http_response_set_status(res, 400, "Bad Request");
    http_response_add_header(res, "Content-Type", "text/html");
    http_response_set_body(res, default_ERROR_400, strlen(default_ERROR_400));
    http_response_send(res, conn);
    http_response_free(res);
}

void default_404(connection_t *conn,  http_response_t *res,int status_code) {
    http_response_init(res);
    http_response_set_status(res, 404, "Not Found");
    http_response_add_header(res, "Content-Type", "text/html");
    http_response_set_body(res, default_ERROR_404, strlen(default_ERROR_404));
    http_response_send(res, conn);
    http_response_free(res);
}

void default_405(connection_t *conn,  http_response_t *res, int status_code) {
    http_response_init(res);
    http_response_set_status(res, 405, "Method Not Allowed");
    http_response_add_header(res, "Content-Type", "text/html");
    http_response_set_body(res, default_ERROR_405, strlen(default_ERROR_405));
    http_response_send(res, conn);
    http_response_free(res);
}

void default_500(connection_t *conn,  http_response_t *res, int status_code) {
    http_response_init(res);
    http_response_set_status(res, 500, "Internal Server Error");
    http_response_add_header(res, "Content-Type", "text/html");
    http_response_set_body(res, default_ERROR_500, strlen(default_ERROR_500));
    http_response_send(res, conn);
    http_response_free(res);
}

void default_403(connection_t *conn,  http_response_t *res, int status_code) {
    http_response_init(res);
    http_response_set_status(res, 403, "Forbidden");
    http_response_add_header(res, "Content-Type", "text/html");
    http_response_set_body(res, default_ERROR_403, strlen(default_ERROR_403));
    http_response_send(res, conn);
    http_response_free(res);
}


error_handler_t default_err_handlers[512] = {
    [400] = default_400,
    [404] = default_404,
    [403] = default_403,
    [405] = default_405,
    [500] = default_500,
};

void check_error(server_t *srv, http_response_t *res, int status_code) {
    if (status_code >= 512) goto fallback_err;
    if (srv->error_handlers[status_code]) srv->error_handlers[status_code](&srv->curr_conn, res, status_code);
    else if (default_err_handlers[status_code]) default_err_handlers[status_code](&srv->curr_conn, res ,status_code);

    else {
        fallback_err:
            default_500(&srv->curr_conn, res ,500);
    }
}

// Setting routes

int match_route(const char *pattern, const char *path, route_params_t *params) {
    params->count = 0;
    while (*pattern && *path) {
        if (*pattern == ':') {
            // Eat until next '/' or end
            pattern++;  // skip ':'
            char key[64] = {0};
            int i = 0;
            while (*pattern && *pattern != '/' && (i < sizeof(key)-1)) key[i++] = *pattern++;
            key[i] = '\0';

            char value[256] = {0};
            int j = 0;
            while (*path && *path != '/' && (j < sizeof(value)-1)) value[j++] = *path++;
            value[j] = '\0';

            // store the captured param
            strncpy(params->params[params->count].key, key, sizeof(params->params[0].key) - 1);
            strncpy(params->params[params->count].value, value, sizeof(params->params[0].value) - 1);
            url_decode(params->params[params->count].key);
            url_decode(params->params[params->count].value);


            params->count++;
            if (params->count >= PARAMS_MAX) {fprintf(stderr,"Too many params when parsing request."); return 0;}
        } 
        else if (*pattern == '*') {
            // Eat everything after *
            strncpy(params->params[params->count].key, "*", 2);
            strncpy(params->params[params->count].value, path, sizeof(params->params[0].value) - 1);
            params->count++;
            if (params->count >= PARAMS_MAX) {fprintf(stderr,"Too many params when parsing request."); return 0;}
            return 1;  // always matches rest of path
        } 
        else {
            // literal so must match exactly
            if (*pattern != *path) return 0;
            pattern++;
            path++;
        }
    }
    return *pattern == '\0' && *path == '\0';
}


const char *route_params_get(route_params_t *params, const char *key) {
    for (int i = 0; i < params->count; i++)
        if (strcmp(params->params[i].key, key) == 0)
            return params->params[i].value;
    return NULL;
}



static int route_specificity(const char *path) {
    /* 
    counts non-parameter segments 
    Bsically the more literal(Nor parametized) route there is, it takes priority when choosing a rout for a path
    */
    int score = 0;
    const char *p = path;
    while (*p) {
        if (*p == '/') { p++; if (*p && *p != ':' && *p != '*') score++; }
        else p++;
    }
    return score;
}

static void sort_routes(server_t *srv) {
    for (int i = 0; i < srv->route_count - 1; i++)
        for (int j = i + 1; j < srv->route_count; j++)
            if (route_specificity(srv->routes[j].path) > 
                route_specificity(srv->routes[i].path)) {
                route_t tmp = srv->routes[i];
                srv->routes[i] = srv->routes[j];
                srv->routes[j] = tmp;
            }
}

static void server_add_route(server_t *srv, const char *method, const char *path, route_handler_t handler) {
    if (!srv || !method || !path || !handler) {fprintf(stderr, "Invalid route parameters\n"); return;}
    if (srv->route_count >= MAX_ROUTES) {
        fprintf(stderr, "Max routes reached in 'server_add_route'\n");
        return;
    }
    route_t *r = &srv->routes[srv->route_count++];
    r->use_global = 1;
    r->global_first = 1;
    strncpy(r->method, method, sizeof(r->method) - 1);
    strncpy(r->path, path, sizeof(r->path) - 1);
    r->handler = handler;
    sort_routes(srv);
}

void server_get(server_t *srv, const char *path, route_handler_t handler) {
    server_add_route(srv, "GET", path, handler);
}

void server_post(server_t *srv, const char *path, route_handler_t handler) {
    server_add_route(srv, "POST", path, handler);
}

void server_put(server_t *srv, const char *path, route_handler_t handler) {
    server_add_route(srv, "PUT", path, handler);
}

void server_delete(server_t *srv, const char *path, route_handler_t handler) {
    server_add_route(srv, "DELETE", path, handler);
}

void server_set_error_handler(server_t *srv, int status_code, error_handler_t handler) {
    if (!srv || status_code < 0 || status_code >= 512) {fprintf(stderr, "Invalid Error parameters in 'server_set_error_handler'\n"); return;}
    srv->error_handlers[status_code] = handler;
}



//Server 


int create_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) goto err_create_sock;
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) goto err_create_sock;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto err_create_sock;
    if(listen(fd, SOMAXCONN) < 0) goto err_create_sock;
    return fd;
    err_create_sock:
        fprintf(stderr, "Problem creating sockets in 'server_create'");
        return -1;
}

char* get_file_data(char *filepath, long *size){
    FILE *f = fopen(filepath, "rb");
    if (!f){fprintf(stderr,"Couldn't open file from path %s",filepath); return NULL;}
    *size = get_file_size(f);
    char *data = malloc(*size);
    if(!data){fclose(f);fprintf(stderr,"Couldn't malloc for file data for path %s",filepath); return NULL;};

    size_t read_bytes = fread(data, 1, *size, f);
    if (read_bytes != *size) {
        fclose(f);
        free(data);
        fprintf(stderr, "Reading failed for file data for path %s", filepath);
        return NULL;
    }
    fclose(f);
    return data;
}


void serve_static (server_t *srv, const char* path, http_response_t *res) {
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s%s", srv->config.static_dir, path);
    if (!check_filepath(srv->config.static_dir, filepath)) {
        check_error(srv, res, 403);
        return;
    }
    long size = 0;
    char *data = get_file_data(filepath, &size);
    if(!data){check_error(srv, res ,500); goto serve_end;}


    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", get_content_type(filepath));
    http_response_set_body(res, data, size);
    http_response_send(res, &srv->curr_conn);
    http_response_free(res);
    serve_end:
        free(data);
}

server_t *server_create(server_config_t *config) {
    if (!config) {fprintf(stderr, "Invalid server configuration in 'server_create'\n"); return NULL;}
    if (config->https_port && (!config->cert || !config->key)) {
        fprintf(stderr, "HTTPS enabled but cert/key not provided in 'server_create'\n");
        return NULL;
    }
    if (config->http_port <= 0 && config->https_port <= 0) {
        fprintf(stderr, "At least one of http_port or https_port must be specified in 'server_create'\n");
        return NULL;
    }
    server_t *srv = malloc(sizeof(server_t));
    if (!srv) {fprintf(stderr, "Memory allocation for the server failed in 'server_create'\n"); return NULL;}
    srv->config = *config;
    srv->config.static_dir = srv->config.static_dir ? srv->config.static_dir : "./static";

    srv->http_fd = config->http_port ? create_server_socket(config->http_port) : -1;
    srv->https_fd = config->https_port ? create_server_socket(config->https_port) : -1;
    srv->ctx = NULL;
    srv->route_count = 0;
    memset(srv->error_handlers, 0, sizeof(srv->error_handlers));
    memset(srv->routes, 0, sizeof(srv->routes));
    return srv;
}


int server_prepare(server_t *srv) {
    if (srv->config.https_port) {
        //SSL_library_init(); Depricated apparently. OpenSSL 1.1 should auto int
        // OpenSSL_add_all_algorithms();  same
        // SSL_load_error_strings();  same
        srv->ctx = SSL_CTX_new(TLS_server_method());
        if (!srv->ctx) {
            fprintf(stderr, "Failed to create SSL context in 'server_prepare'\n");
            ERR_print_errors_fp(stderr);
            return -1;
        }
        if (SSL_CTX_use_certificate_file(srv->ctx, srv->config.cert, SSL_FILETYPE_PEM) <= 0) {
            ERR_print_errors_fp(stderr);
            return -1;
        }
        if (SSL_CTX_use_PrivateKey_file(srv->ctx, srv->config.key, SSL_FILETYPE_PEM) <= 0) {
            ERR_print_errors_fp(stderr);
            return -1;
        }
        SSL_CTX_set_min_proto_version(srv->ctx, TLS1_2_VERSION);
    }
    return 0;
}

int accept_client(int http_fd, int https_fd, int *is_ssl) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    if (http_fd >= 0) FD_SET(http_fd, &read_fds);
    if (https_fd >= 0) FD_SET(https_fd, &read_fds);
    int max_fd = https_fd > http_fd ? https_fd : http_fd;
    if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
        perror("select failed");
        return -1;
    }
    if (http_fd >= 0 && FD_ISSET(http_fd, &read_fds)) {
        *is_ssl = 0;
        return accept(http_fd, NULL, NULL);
    }
    if (https_fd >= 0 && FD_ISSET(https_fd, &read_fds)) {
        *is_ssl = 1;
        return accept(https_fd, NULL, NULL);
    }
    return -1;
}

void server_run(server_t *srv) {
    int exit_int = 0;
    int is_ssl = 0;
    signal(SIGCHLD, SIG_IGN);
    while(1){
        exit_int = 0;
        int client_fd = accept_client(srv->http_fd, srv->https_fd, &is_ssl);
        if (client_fd < 0) continue;

        
        pid_t pid = fork();
        if (pid < 0) { perror("fork failed"); close(client_fd); continue; }
        if (pid == 0) {
            close(srv->http_fd);
            close(srv->https_fd);
            SSL *ssl = NULL;
            srv->curr_conn.ssl = ssl;
            srv->curr_conn.fd = client_fd;
            http_request_t req;
            http_request_init(&req);
            http_response_t res;
            http_response_init(&res);
            if (is_ssl) {
                srv->curr_conn.ssl = SSL_new(srv->ctx);
                if (!srv->curr_conn.ssl) goto ssl_err;
                SSL_set_fd(srv->curr_conn.ssl, client_fd); 
                if (!srv->curr_conn.ssl || (SSL_accept(srv->curr_conn.ssl) <= 0)) {
                    ssl_err:
                        fprintf(stderr, "Failed to create SSL object\n");
                        ERR_print_errors_fp(stderr);
                        check_error(srv ,&res,500);
                        exit_int = 1;
                        goto end;
                }
            }
            

            char buffer[BUFFER_SIZE] = {0};
            ssize_t bytes = conn_read(&srv->curr_conn, buffer, sizeof(buffer) - 1);
            if (bytes <= 0) { check_error(srv,&res, 400); exit_int = 1; goto end; }



            if (http_parse_request(buffer, bytes, &req) < 0) {check_error(srv,&res,400); exit_int = 1; goto end;
}

            // find matching route
            route_t *route = NULL;
            int path_exist = 0;
            int path_found = 0;
            route_params_t params = {0};
            for (int i = 0; i < srv->route_count; i++) {
                path_found = match_route(srv->routes[i].path, req.path, &params);
                path_exist = path_exist ? 1 : path_found;
                if (path_found && strcmp(srv->routes[i].method, req.method) == 0) {
                    route = &srv->routes[i];    
                    route->params = params;
                    break;
                }
            }

            if (route) chain_next(&req, &res, srv, route, 0);
            else if (path_exist) {check_error(srv, &res, 405); exit_int =1; goto end;}
            else {check_error(srv,&res,404); exit_int =1; goto end;}
            
            

            end:

                http_request_free(&req);
                conn_close(&srv->curr_conn);
                exit(exit_int);
        }   
        close(client_fd);
    }
}


void server_close(server_t *srv) {
    if (!srv) return;
    if (srv->http_fd >= 0) close(srv->http_fd);
    if (srv->https_fd >= 0) close(srv->https_fd);
    if (srv->ctx) SSL_CTX_free(srv->ctx);
    free(srv);
}





void server_use(server_t *srv, middleware_t m) {
    if (!srv || !m) return;
    if (srv->global_middleware_count >= MAX_MIDDLEWARE) {
        fprintf(stderr, "Max global middleware reached from 'server_use'\n");
        return;
    }
    srv->global_middleware[srv->global_middleware_count++] = m;
}

void route_use(server_t *srv, const char *method, const char *path, middleware_t m) {
    if (!srv || !method || !path || !m) return;
    for (int i = 0; i < srv->route_count; i++) {
        if (strcasecmp(srv->routes[i].method, method) == 0 &&
            strcmp(srv->routes[i].path, path) == 0) {
            if (srv->routes[i].middleware_count >= MAX_MIDDLEWARE) {
                fprintf(stderr, "Max route middleware reached from 'route_use'\n");
                return;
            }
            srv->routes[i].middleware[srv->routes[i].middleware_count++] = m;
            return;
        }
    }
    fprintf(stderr, "route %s %s not found in 'route_use'\n", method, path);
}

void chain_next(http_request_t *req, http_response_t *res,server_t *srv, route_t *route, int current) {

    //Yes this rebuilds every iteration, for now I will assume the overhead is negligible 
    middleware_t chain[MAX_MIDDLEWARE * 2];
    int count = 0;

    if (route->use_global && route->global_first) {
        for (int i = 0; i < srv->global_middleware_count; i++)
            chain[count++] = srv->global_middleware[i];
        for (int i = 0; i < route->middleware_count; i++)
            chain[count++] = route->middleware[i];
    } 
    else if (route->use_global && !route->global_first) {
        for (int i = 0; i < route->middleware_count; i++)
            chain[count++] = route->middleware[i];
        for (int i = 0; i < srv->global_middleware_count; i++)
            chain[count++] = srv->global_middleware[i];
    } 
    else {
        // no global
        for (int i = 0; i < route->middleware_count; i++)
            chain[count++] = route->middleware[i];
    }

    if (current < count)
        chain[current](req, res, srv, route, current + 1);
    else
        route->handler(&srv->curr_conn,req, res, &route->params);
}

void route_set_global(server_t *srv, const char *method, const char *path, int use_global) {
    for (int i = 0; i < srv->route_count; i++) {
        if (strcasecmp(srv->routes[i].method, method) == 0 &&
            strcmp(srv->routes[i].path, path) == 0) {
            srv->routes[i].use_global = use_global;
            return;
        }
    }
    fprintf(stderr, "Route not found in 'route_set_global'\n");
}

void route_set_global_order(server_t *srv, const char *method, const char *path, int global_first) {
    for (int i = 0; i < srv->route_count; i++){
        if (strcasecmp(srv->routes[i].method, method) == 0 &&
            strcmp(srv->routes[i].path, path) == 0) {
            srv->routes[i].global_first = global_first;
            return;
        }
    }
    fprintf(stderr, "Route not found in 'route_set_global_first'\n");
}