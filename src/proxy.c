#include "mini_webserver.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static bool ieq(mw_slice_t s, const char *literal) {
    size_t n = strlen(literal);
    return s.length == n && strncasecmp((const char *)s.data, literal, n) == 0;
}

mw_route_t *mw_proxy_match_route(mw_route_t *routes, size_t route_count, mw_slice_t raw_path) {
    mw_route_t *best = NULL;
    size_t best_length = 0;
    for (size_t i = 0; i < route_count; i++) {
        const char *prefix = routes[i].prefix;
        size_t n = strlen(prefix);
        if (n == 0 || n <= best_length || n > raw_path.length ||
            memcmp(raw_path.data, prefix, n) != 0) {
            continue;
        }
        /* "/service" takes "/service" and "/service/x" but not "/service-other". */
        if (prefix[n - 1] == '/' || raw_path.length == n || raw_path.data[n] == '/') {
            best = &routes[i];
            best_length = n;
        }
    }
    return best;
}

const mw_backend_t *mw_proxy_next_backend(mw_route_t *route) {
    (void)pthread_mutex_lock(&route->selection_lock);
    size_t index = route->next_backend % route->backend_count;
    route->next_backend = index + 1U;
    (void)pthread_mutex_unlock(&route->selection_lock);
    return &route->backends[index];
}

static void append(mw_buffer_t *out, const void *bytes, size_t length) {
    memcpy(out->data + out->length, bytes, length);
    out->length += length;
}

/* Absolute-form "http://host[:port]rest": rest is the origin-form target, "/" when it is empty or
 * starts with the query. */
static mw_slice_t origin_target(mw_slice_t target, bool *needs_slash) {
    size_t i = 7U; /* "http://", any case; the parser checked it */
    while (i < target.length && target.data[i] != '/' && target.data[i] != '?') {
        i++;
    }
    *needs_slash = i == target.length || target.data[i] == '?';
    return (mw_slice_t){target.data + i, target.length - i};
}

/* Reverse: method and target as received, the client's Host. Forward: origin-form target, Host
 * from the URI and no Proxy-Authorization. Both: HTTP/1.1, end-to-end headers, our framing. */
static int build_request(int client_fd, const mw_request_t *request, bool forward,
                         mw_buffer_t *out) {
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    char address[INET6_ADDRSTRLEN];
    if (getpeername(client_fd, (struct sockaddr *)&peer, &peer_length) == -1 ||
        getnameinfo((struct sockaddr *)&peer, peer_length, address, sizeof(address), NULL, 0,
                    NI_NUMERICHOST) != 0) {
        return -1;
    }
    char host[MW_MAX_BACKEND_HOST + 16U] = "";
    mw_slice_t target = request->target;
    bool needs_slash = false;
    if (forward) {
        target = origin_target(request->target, &needs_slash);
        (void)snprintf(
            host, sizeof(host), "Host: %.*s:%u\r\n", (int)request->destination_host.length,
            (const char *)request->destination_host.data, (unsigned int)request->destination_port);
    }
    char tail[128 + INET6_ADDRSTRLEN];
    int tail_length = snprintf(tail, sizeof(tail),
                               "Connection: close\r\nX-Forwarded-For: %s\r\n"
                               "Content-Length: %zu\r\n\r\n",
                               address, request->body.length);
    size_t capacity = request->method.length + target.length + 17U + strlen(host) +
                      (size_t)tail_length + request->body.length;
    for (size_t i = 0; i < request->headers.count; i++) {
        capacity +=
            request->headers.items[i].name.length + request->headers.items[i].value.length + 4U;
    }
    *out = (mw_buffer_t){malloc(capacity), 0, capacity};
    if (out->data == NULL) {
        return -1;
    }
    append(out, request->method.data, request->method.length);
    append(out, " ", 1);
    if (needs_slash) {
        append(out, "/", 1);
    }
    append(out, target.data, target.length);
    append(out, " HTTP/1.1\r\n", 11);
    append(out, host, strlen(host));
    for (size_t i = 0; i < request->headers.count; i++) {
        const mw_header_t *header = &request->headers.items[i];
        if (mw_http_is_hop_by_hop(&request->headers, header->name) || ieq(header->name, "expect") ||
            ieq(header->name, "x-forwarded-for") || ieq(header->name, "content-length") ||
            (forward && (ieq(header->name, "host") || ieq(header->name, "proxy-authorization")))) {
            continue;
        }
        append(out, header->name.data, header->name.length);
        append(out, ": ", 2);
        append(out, header->value.data, header->value.length);
        append(out, "\r\n", 2);
    }
    append(out, tail, (size_t)tail_length);
    append(out, request->body.data, request->body.length);
    return 0;
}

static int failure_status(void) {
    return errno == ETIMEDOUT ? 504 : 502;
}

/* One backend connection per request: send, then read until the backend closes. */
static int exchange(const mw_backend_t *backend, const mw_buffer_t *outgoing,
                    mw_buffer_t *incoming) {
    mw_deadline_t deadline = mw_net_now_ms() + MW_UPSTREAM_TIMEOUT_MS;
    int fd = mw_net_connect(backend, deadline);
    if (fd == -1) {
        return failure_status();
    }
    int status = 0;
    if (mw_net_write_all(fd, outgoing->data, outgoing->length, deadline) == -1) {
        status = failure_status();
    }
    while (status == 0) {
        if (incoming->length == incoming->capacity) {
            /* One byte over the limit tells "exactly 8 MiB" from "too much". */
            if (incoming->capacity > MW_MAX_UPSTREAM_BYTES) {
                status = 502;
                break;
            }
            size_t capacity = incoming->capacity == 0 ? 16384U : incoming->capacity * 2U;
            if (capacity > MW_MAX_UPSTREAM_BYTES) {
                capacity = MW_MAX_UPSTREAM_BYTES + 1U;
            }
            unsigned char *grown = realloc(incoming->data, capacity);
            if (grown == NULL) {
                status = 502;
                break;
            }
            incoming->data = grown;
            incoming->capacity = capacity;
        }
        ssize_t count = mw_net_read(fd, incoming->data + incoming->length,
                                    incoming->capacity - incoming->length, deadline);
        if (count == 0) {
            break;
        }
        if (count < 0) {
            status = failure_status();
        } else {
            incoming->length += (size_t)count;
        }
    }
    (void)close(fd);
    return status;
}

static int send_upstream(int fd, const mw_upstream_response_t *upstream, bool head_only,
                         bool keep_alive, mw_deadline_t deadline) {
    mw_response_t response = {.status = upstream->status,
                              .body = {upstream->body.data, upstream->body.length}};
    for (size_t i = 0; i < upstream->headers.count; i++) {
        if (!mw_http_is_hop_by_hop(&upstream->headers, upstream->headers.items[i].name)) {
            response.headers.items[response.headers.count++] = upstream->headers.items[i];
        }
    }
    if (head_only) {
        /* No body bytes go out, but Content-Length must still be the backend's. */
        response.body = (mw_slice_t){NULL, upstream->content_length};
    }
    return mw_http_send_response(fd, &response, head_only, keep_alive, deadline);
}

/* Like mw_http_send_error, but a refusal (403) or backend failure keeps the client's connection. */
static int send_gateway_error(int fd, int status, bool head_only, bool keep_alive,
                              mw_deadline_t deadline) {
    char body[64];
    int size = snprintf(body, sizeof(body), "<!doctype html><h1>%d %s</h1>\n", status,
                        mw_http_status_reason(status));
    mw_response_t response = {.status = status,
                              .body = {(const unsigned char *)body, (size_t)size}};
    response.headers.items[0] =
        (mw_header_t){{(const unsigned char *)"Content-Type", 12},
                      {(const unsigned char *)"text/html; charset=utf-8", 24}};
    response.headers.count = 1;
    return mw_http_send_response(fd, &response, head_only, keep_alive, deadline);
}

/* Rewrite, exchange, relay: the steps both proxies share. A failure is a 502/504 page. */
static int relay(int client_fd, const mw_request_t *request, const mw_backend_t *backend,
                 bool forward, bool keep_alive, mw_deadline_t deadline) {
    bool head_only = request->method.length == 4 && memcmp(request->method.data, "HEAD", 4) == 0;
    mw_buffer_t outgoing = {NULL, 0, 0};
    mw_buffer_t incoming = {NULL, 0, 0};
    int status = backend == NULL ? 502
                 : build_request(client_fd, request, forward, &outgoing) == 0
                     ? exchange(backend, &outgoing, &incoming)
                     : 502;
    int result = -1;
    if (status == 0) {
        mw_upstream_response_t upstream;
        status =
            mw_http_parse_upstream_response(incoming.data, incoming.length, head_only, &upstream);
        if (status == 0) {
            result = send_upstream(client_fd, &upstream, head_only, keep_alive, deadline);
        }
        mw_http_upstream_destroy(&upstream);
    }
    if (status != 0) {
        result = send_gateway_error(client_fd, status, head_only, keep_alive, deadline);
    }
    free(outgoing.data);
    free(incoming.data);
    return result;
}

int mw_proxy_serve(const mw_config_t *config, mw_route_t *route, int client_fd,
                   const mw_request_t *request, bool keep_alive, mw_deadline_t deadline) {
    (void)config;
    return relay(client_fd, request, mw_proxy_next_backend(route), false, keep_alive, deadline);
}

/* The destination's first IPv4 address. Blocking: DNS time is outside the upstream deadline. */
static bool resolve_destination(const mw_request_t *request, mw_backend_t *destination) {
    size_t length = request->destination_host.length;
    if (length > MW_MAX_BACKEND_HOST) {
        return false;
    }
    memcpy(destination->host, request->destination_host.data, length);
    destination->host[length] = '\0';
    destination->port = request->destination_port;
    char port[8];
    (void)snprintf(port, sizeof(port), "%u", (unsigned int)destination->port);
    struct addrinfo hints = {
        .ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV};
    struct addrinfo *found = NULL;
    if (getaddrinfo(destination->host, port, &hints, &found) != 0) {
        return false;
    }
    memcpy(&destination->address, found->ai_addr, found->ai_addrlen);
    destination->address_length = found->ai_addrlen;
    freeaddrinfo(found);
    return true;
}

int mw_proxy_forward(const mw_config_t *config, int client_fd, const mw_request_t *request,
                     bool keep_alive, mw_deadline_t deadline) {
    bool head_only = request->method.length == 4 && memcmp(request->method.data, "HEAD", 4) == 0;
    struct sockaddr_in peer;
    socklen_t peer_length = sizeof(peer);
    if (getpeername(client_fd, (struct sockaddr *)&peer, &peer_length) == -1 ||
        peer.sin_family != AF_INET ||
        !mw_config_forward_allowed(config->forward_allow, config->forward_allow_count,
                                   peer.sin_addr)) {
        return send_gateway_error(client_fd, 403, head_only, keep_alive, deadline);
    }
    mw_backend_t destination;
    return relay(client_fd, request,
                 resolve_destination(request, &destination) ? &destination : NULL, true, keep_alive,
                 deadline);
}
