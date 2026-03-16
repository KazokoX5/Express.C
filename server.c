//TESTING SERVER

#include <stdio.h>
#include <string.h>
#include <openssl/err.h>

#include "reqparser.h"

/* -- Global server pointer -- */
server_t *g_srv = NULL;

/* ---------------------------Middleware--------------------------- */

void logger(http_request_t *req, http_response_t *res, server_t *srv, route_t *route, int current){
    (void)srv; (void)route; (void)current;
    printf("[LOG] %s %s\n", req->method, req->path);
    chain_next(req, res, srv, route, current);
    printf("[LOG] response status: %d\n", res->status_code);
}

void auth(http_request_t *req, http_response_t *res, server_t *srv, route_t *route, int current){
    const char *token = http_request_get_header_value(req, "Authorization");
    if (!token) {
        http_response_set_status(res, 401, "Unauthorized");
        http_response_add_header(res, "Content-Type", "text/html");
        http_response_set_body(res, "<h1>401 Unauthorized</h1>", strlen("<h1>401 Unauthorized</h1>"));
        http_response_send(res, &srv->curr_conn);
        http_response_free(res);
        return; // stop the middleware chain
    }
    chain_next(req, res, srv, route, current);
}

/* ---------------------------Handlers--------------------------- */

void on_hello(connection_t *conn, http_request_t *req, http_response_t *res, route_params_t *params){
    (void)req; (void)params;
    const char *body = "Hello, world!";
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_set_body(res, body, strlen(body));
    http_response_send(res, conn);
    http_response_free(res);
}

void on_user(connection_t *conn,http_request_t *req, http_response_t *res, route_params_t *params){
    const char *id = route_params_get(params, "id");
    char body[128];
    int len = snprintf(body, sizeof(body), "<h1>User: %s</h1>", id ? id : "unknown");
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/html");
    http_response_set_body(res, body, (size_t)len);
    http_response_send(res, conn);
    http_response_free(res);
}

void on_search(connection_t *conn, http_request_t *req, http_response_t *res, route_params_t *params){
    (void)params;
    const char *q = NULL;
    const char *page = NULL;
    int count = 0;
    path_queries_t *queries = &req->queries;
    for (int i = 0; i < queries->query_count; i++) {
        printf("%s %s\n",queries->queries[i].name,queries->queries[i].value);
        if (strcmp(queries->queries[i].name, "q") == 0)            q = queries->queries[i].value;
        else if (strcmp(queries->queries[i].name, "pages") == 0)            page = queries->queries[i].value;
        else count++;
    }
    char body[256];
    int len = snprintf(body, sizeof(body),
                       "<h1>Search</h1><p>q=%s page=%s others count=%d</p>",
                       q ? q : "(none)", page ? page : "(none)",count);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/html");
    http_response_set_body(res, body, (size_t)len);
    http_response_send(res, conn);
    http_response_free(res);
}

void on_files(connection_t *conn, http_request_t *req, http_response_t *res, route_params_t *params){
    (void)params;
    if (g_srv) {
        const char *path = route_params_get(params, "*");
        char start[PATH_MAX] = "/";
        strcat(start,path);
        serve_static(g_srv, start, res);
    }
    else {
        http_response_set_status(res, 500, "Internal Server Error");
        http_response_add_header(res, "Content-Type", "text/html");
        http_response_set_body(res, "<h1>500</h1>", strlen("<h1>500</h1>"));
        http_response_send(res, conn);
        http_response_free(res);
    }
}

void on_secret(connection_t *conn, http_request_t *req, http_response_t *res, route_params_t *params){
    (void)params;
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_set_body(res, "secret data", strlen("secret data"));
    http_response_send(res, conn);
    http_response_free(res);
}


/* ---------------------------Err Handlers--------------------------- */

void my_404(connection_t *conn, http_response_t *res, int status_code)
{
    (void)status_code;
    http_response_set_status(res, 404, "Not Found");
    http_response_add_header(res, "Content-Type", "text/html");
    http_response_set_body(res, "<h1>Custom 404</h1>", strlen("<h1>Custom 404</h1>"));
    http_response_send(res, conn);
    http_response_free(res);
}

/* ---------------------------Main--------------------------- */

int main(void)
{
    server_config_t config = {
        .http_port   = 8080,
        .https_port  = 8443,                
        .cert        = "localhost.pem",
        .key         = "localhost-key.pem",
        .static_dir  = "./static",
        .max_clients = 10
    };

    g_srv = server_create(&config);
    if (!g_srv) {
        fprintf(stderr, "server_create failed\n");
        return 1;
    }


    server_use(g_srv, logger);


    server_get(g_srv,  "/hello",    on_hello);
    server_get(g_srv,  "/user/:id", on_user);
    server_get(g_srv,  "/search",   on_search);
    server_get(g_srv,  "/files/*",  on_files);
    server_get(g_srv,  "/secret",   on_secret);


    route_use(g_srv, "GET", "/secret", auth);
    route_set_global_order(g_srv, "GET", "/secret", 0); 

    server_set_error_handler(g_srv, 404, my_404);

    if (server_prepare(g_srv) < 0) {
        fprintf(stderr, "server_prepare failed\n");
        server_close(g_srv);
        return 1;
    }

    printf("Starting server on port %d(https) and %d(https) (static dir: %s)\n", config.http_port, config.http_port ,config.static_dir);
    server_run(g_srv);

    server_close(g_srv);
    return 0;
}