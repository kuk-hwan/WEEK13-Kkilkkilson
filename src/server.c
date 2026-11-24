#include "mini_webserver.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/*
 * Send an error page, then drain what the client still sends (bounded) so close() does not turn
 * unread bytes into a RST that destroys the response before the client reads it.
 */
static void send_error(int fd, int status, bool head_only) {
    mw_deadline_t deadline = mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS;
    if (mw_http_send_error(fd, status, head_only, deadline) == -1 || shutdown(fd, SHUT_WR) == -1) {
        return;
    }
    unsigned char sink[4096];
    mw_deadline_t linger = mw_net_now_ms() + 200;
    for (size_t drained = 0; drained < 1024U * 1024U;) {
        ssize_t count = mw_net_read(fd, sink, sizeof(sink), linger);
        if (count <= 0) {
            break;
        }
        drained += (size_t)count;
    }
}

/* The accepting thread writes this, so it gets a short deadline instead of the request one. */
#define OVERLOAD_WRITE_TIMEOUT_MS 100

static void reject_overloaded(int fd) {
    if (mw_http_send_error(fd, 503, false, mw_net_now_ms() + OVERLOAD_WRITE_TIMEOUT_MS) == -1) {
        return;
    }
    /* A successful write leaves fd nonblocking; discard only bytes already received. */
    char discard[512];
    (void)recv(fd, discard, sizeof(discard), 0);
}

/* A failed accept is retried unless the listener itself is unusable. */
static bool accept_error_is_transient(int error) {
    switch (error) {
    case EINTR:
    case ECONNABORTED:
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EPROTO:
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
        return true;
    default:
        return false;
    }
}

static bool slice_is(mw_slice_t s, const char *literal) {
    return s.length == strlen(literal) && memcmp(s.data, literal, s.length) == 0;
}

static mw_slice_t header_value(const mw_request_t *request, const char *name) {
    for (size_t i = 0; i < request->headers.count; i++) {
        const mw_slice_t key = request->headers.items[i].name;
        if (key.length == strlen(name) &&
            strncasecmp((const char *)key.data, name, key.length) == 0) {
            return request->headers.items[i].value;
        }
    }
    return (mw_slice_t){NULL, 0};
}

static void send_405(int fd, const char *allow, bool head_only) {
    static const char page[] = "<!doctype html><h1>405 Method Not Allowed</h1>\n";
    mw_response_t response = {.status = 405,
                              .body = {(const unsigned char *)page, sizeof(page) - 1}};
    response.headers.items[0] =
        (mw_header_t){{(const unsigned char *)"Content-Type", 12},
                      {(const unsigned char *)"text/html; charset=utf-8", 24}};
    response.headers.items[1] = (mw_header_t){{(const unsigned char *)"Allow", 5},
                                              {(const unsigned char *)allow, strlen(allow)}};
    response.headers.count = 2;
    (void)mw_http_send_response(fd, &response, head_only, false,
                                mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS);
}

/* Dynamic endpoints (#10). Returns false when the path is not one of them. */
static bool serve_dynamic(const mw_config_t *config, int fd, const mw_request_t *request) {
    bool get = slice_is(request->method, "GET");
    bool head = slice_is(request->method, "HEAD");
    mw_response_t response = {.status = 200};
    if (slice_is(request->path, "/api/whoami")) {
        if (!get && !head) {
            send_405(fd, "GET, HEAD", false);
            return true;
        }
        char json[512];
        size_t n = (size_t)snprintf(json, sizeof(json), "{\"name\":\"");
        for (const char *p = config->name; *p != '\0'; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                json[n++] = '\\';
                json[n++] = (char)c;
            } else if (c < 0x20) {
                n += (size_t)snprintf(json + n, 7, "\\u%04x", c);
            } else {
                json[n++] = (char)c;
            }
        }
        n += (size_t)snprintf(json + n, sizeof(json) - n, "\",\"port\":%u}\n",
                              (unsigned int)config->port);
        response.body = (mw_slice_t){(const unsigned char *)json, n};
        response.headers.items[0] = (mw_header_t){{(const unsigned char *)"Content-Type", 12},
                                                  {(const unsigned char *)"application/json", 16}};
        response.headers.count = 1;
        (void)mw_http_send_response(fd, &response, head, false,
                                    mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS);
        return true;
    }
    if (slice_is(request->path, "/api/echo")) {
        if (!slice_is(request->method, "POST")) {
            send_405(fd, "POST", head);
            return true;
        }
        mw_slice_t type = header_value(request, "Content-Type");
        if (type.length == 0) {
            type = (mw_slice_t){(const unsigned char *)"application/octet-stream", 24};
        }
        response.body = request->body;
        response.headers.items[0] =
            (mw_header_t){{(const unsigned char *)"Content-Type", 12}, type};
        response.headers.count = 1;
        (void)mw_http_send_response(fd, &response, false, false,
                                    mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS);
        return true;
    }
    return false;
}

static void handle_connection(mw_config_t *config, int client) {
    mw_buffer_t input = {NULL, 0, 0};
    mw_request_t request;
    for (size_t served = 1;; served++) {
        int result = mw_http_read_request(client, &input, &request,
                                          mw_net_now_ms() + MW_KEEP_ALIVE_IDLE_TIMEOUT_MS);
        mw_route_t *route =
            result == 0 ? mw_proxy_match_route(config->routes, config->route_count, request.path)
                        : NULL;
        if (route != NULL) {
            /* Only proxied responses keep the connection so far; #9 extends it to the rest. */
            bool keep = request.keep_alive && served < MW_MAX_REQUESTS_PER_CONNECTION;
            if (mw_proxy_serve(config, route, client, &request, keep,
                               mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS) != 0 ||
                !keep) {
                break;
            }
            /* Drop this request, keep any pipelined bytes behind it. */
            size_t used = (size_t)(request.body.data - input.data) + request.body.length;
            memmove(input.data, input.data + used, input.length - used);
            input.length -= used;
            continue;
        }
        if (result > 0) {
            send_error(client, result, false);
        } else if (result == -1) {
            if (errno == ETIMEDOUT && input.length > 0) {
                send_error(client, 408, false);
            }
        } else if (serve_dynamic(config, client, &request)) {
            /* answered */
        } else {
            (void)mw_static_serve(config, client, &request, false,
                                  mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS);
        }
        break;
    }
    mw_http_buffer_destroy(&input);
}

/* The worker owns the connection; handlers borrow it until the response is complete. */
void mw_app_handle_connection(mw_runtime_t *runtime, int client_fd) {
    handle_connection(runtime->config, client_fd);
}

int mw_server_run(mw_config_t *config) {
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    struct sigaction previous;
    (void)sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &previous) == -1) {
        perror("SIGPIPE");
        return MW_EXIT_RUNTIME_ERROR;
    }
    mw_http_set_server_name(config->name);
    int listener = mw_net_listen(config);
    if (listener == -1) {
        perror("listener (socket/bind/listen)");
        (void)sigaction(SIGPIPE, &previous, NULL);
        return MW_EXIT_RUNTIME_ERROR;
    }
    mw_runtime_t runtime = {.config = config};
    atomic_init(&runtime.stopping, false);
    mw_pool_t *pool = NULL;
    if (mw_pool_create(&runtime, &pool) == -1) {
        perror("thread pool");
        (void)close(listener);
        (void)sigaction(SIGPIPE, &previous, NULL);
        return MW_EXIT_RUNTIME_ERROR;
    }
    fprintf(stderr, "Listening on http://%s:%u (root: %s)\n", config->host,
            (unsigned int)config->port, config->root);
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client == -1) {
            if (!accept_error_is_transient(errno)) {
                perror("accept");
                break;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                /* Out of descriptors or memory: back off briefly instead of spinning. */
                perror("accept");
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 10L * 1000L * 1000L};
                (void)nanosleep(&pause, NULL);
            }
            continue;
        }
        if (!mw_pool_submit(pool, client)) {
            reject_overloaded(client);
            (void)close(client);
        }
    }
    mw_pool_destroy(pool);
    (void)close(listener);
    (void)sigaction(SIGPIPE, &previous, NULL);
    return MW_EXIT_RUNTIME_ERROR;
}
