#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* Darwin hides O_NOFOLLOW in its strict POSIX namespace. */
#define _DARWIN_C_SOURCE
#endif
#include "mini_webserver.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Bootstrap response code is private; #5/#6 replace it with the HTTP/static modules. */
static int send_head(const mw_config_t *config, int fd, int status, uintmax_t length,
                     mw_deadline_t deadline) {
    time_t now = time(NULL);
    struct tm utc;
    char date[64];
    if (gmtime_r(&now, &utc) == NULL ||
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc) == 0) {
        return -1;
    }
    char head[512];
    int size =
        snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %ju\r\nConnection: close\r\nDate: %s\r\nServer: %s\r\n\r\n",
                 status, mw_http_status_reason(status), length, date, config->name);
    if (size < 0 || (size_t)size >= sizeof(head)) {
        return -1;
    }
    return mw_net_write_all(fd, head, (size_t)size, deadline);
}

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
    (void)mw_http_send_error(fd, 503, false, mw_net_now_ms() + OVERLOAD_WRITE_TIMEOUT_MS);
    /* Discard request bytes already received so close does not turn into a reset. */
    char discard[512];
    (void)recv(fd, discard, sizeof(discard), MSG_DONTWAIT);
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

static void serve_index(const mw_config_t *config, int client, bool head_only) {
    int fd = openat(config->root_fd, "index.html", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        int status = errno == ENOENT || errno == EACCES || errno == ELOOP ? 403 : 500;
        send_error(client, status, head_only);
        return;
    }
    struct stat info;
    if (fstat(fd, &info) == -1) {
        (void)close(fd);
        send_error(client, 500, head_only);
        return;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        (void)close(fd);
        send_error(client, 403, head_only);
        return;
    }
    mw_deadline_t deadline = mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS;
    if (send_head(config, client, 200, (uintmax_t)info.st_size, deadline) == 0 && !head_only) {
        unsigned char buffer[8192];
        off_t remaining = info.st_size;
        while (remaining > 0) {
            size_t capacity =
                remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            ssize_t count = read(fd, buffer, capacity);
            if (count == -1 && errno == EINTR) {
                continue;
            }
            if (count <= 0 || mw_net_write_all(client, buffer, (size_t)count, deadline) == -1) {
                break;
            }
            remaining -= count;
        }
    }
    (void)close(fd);
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

/*
 * Wait for the next request on a keep-alive connection, in short slices so a shutdown closes an
 * idle connection at once instead of after its idle timeout. Returns false to close it.
 */
#define IDLE_POLL_SLICE_MS 100

static bool wait_for_next_request(const mw_runtime_t *runtime, int client) {
    mw_deadline_t deadline = mw_net_now_ms() + MW_KEEP_ALIVE_IDLE_TIMEOUT_MS;
    while (!atomic_load(&runtime->stopping)) {
        struct pollfd entry = {.fd = client, .events = POLLIN};
        int result = poll(&entry, 1, IDLE_POLL_SLICE_MS);
        if (result > 0 || (result == -1 && errno != EINTR)) {
            return true; /* data, EOF or an error: the read below sorts it out */
        }
        if (mw_net_now_ms() >= deadline) {
            return false;
        }
    }
    return false;
}

static void handle_connection(mw_runtime_t *runtime, int client) {
    mw_config_t *config = runtime->config;
    mw_buffer_t input = {NULL, 0, 0};
    mw_request_t request;
    for (size_t served = 1;; served++) {
        if (served > 1 && input.length == 0 && !wait_for_next_request(runtime, client)) {
            break;
        }
        int result = mw_http_read_request(client, &input, &request,
                                          mw_net_now_ms() + MW_KEEP_ALIVE_IDLE_TIMEOUT_MS);
        mw_route_t *route =
            result == 0 ? mw_proxy_match_route(config->routes, config->route_count, request.path)
                        : NULL;
        if (route != NULL) {
            /* Only proxied responses keep the connection so far; #9 extends it to the rest. */
            bool keep = request.keep_alive && served < MW_MAX_REQUESTS_PER_CONNECTION &&
                        !atomic_load(&runtime->stopping);
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
        bool head_only = result == 0 && request.method.length == 4 &&
                         memcmp(request.method.data, "HEAD", 4) == 0;
        if (result > 0) {
            send_error(client, result, false);
        } else if (result == -1) {
            if (errno == ETIMEDOUT && input.length > 0) {
                send_error(client, 408, false);
            }
        } else if (serve_dynamic(config, client, &request)) {
            /* answered */
        } else if (request.method.length == 4 && memcmp(request.method.data, "POST", 4) == 0) {
            send_error(client, 405, false);
        } else if (request.path.length == 1 && request.query.length == 0) {
            serve_index(config, client, head_only);
        } else {
            send_error(client, 404, head_only);
        }
        break;
    }
    mw_http_buffer_destroy(&input);
}

/* Wrapper over the bootstrap handler above; #5 and #6 replace it with the real app module. */
void mw_app_handle_connection(mw_runtime_t *runtime, int client_fd) {
    handle_connection(runtime, client_fd);
}

/* The handler may only set the flag and write one byte; both are async-signal-safe. */
static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t wake_fd = -1;

static void on_stop_signal(int signal_number) {
    (void)signal_number;
    int saved = errno;
    stop_requested = 1;
    if (wake_fd != -1) {
        char byte = 1;
        ssize_t written = write(wake_fd, &byte, 1);
        (void)written; /* a full pipe already holds a wake-up byte */
    }
    errno = saved;
}

static int set_nonblocking_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    int fd_flags = fcntl(fd, F_GETFD);
    return fd_flags == -1 ? -1 : fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
}

static int open_wake_pipe(int pipe_fds[2]) {
    if (pipe(pipe_fds) == -1) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (set_nonblocking_cloexec(pipe_fds[i]) == -1) {
            int saved = errno;
            (void)close(pipe_fds[0]);
            (void)close(pipe_fds[1]);
            errno = saved;
            return -1;
        }
    }
    return 0;
}

/* Accept until a stop signal arrives. Returns false if the listener itself broke. */
static bool accept_loop(int listener, int wake_read, mw_pool_t *pool) {
    while (!stop_requested) {
        struct pollfd entries[2] = {{.fd = listener, .events = POLLIN},
                                    {.fd = wake_read, .events = POLLIN}};
        if (poll(entries, 2, -1) == -1) {
            if (errno != EINTR) {
                perror("poll");
                return false;
            }
            continue;
        }
        if (stop_requested || (entries[1].revents & POLLIN) != 0) {
            break;
        }
        if ((entries[0].revents & (POLLERR | POLLNVAL)) != 0) {
            fprintf(stderr, "accept: listener failed\n");
            return false;
        }
        if ((entries[0].revents & POLLIN) == 0) {
            continue;
        }
        int client = accept(listener, NULL, NULL);
        if (client == -1) {
            if (!accept_error_is_transient(errno)) {
                perror("accept");
                return false;
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
    return true;
}

int mw_server_run(mw_config_t *config) {
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    struct sigaction previous_pipe;
    (void)sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &previous_pipe) == -1) {
        perror("SIGPIPE");
        return MW_EXIT_RUNTIME_ERROR;
    }
    mw_http_set_server_name(config->name);
    int listener = mw_net_listen(config);
    if (listener == -1) {
        perror("listener (socket/bind/listen)");
        (void)sigaction(SIGPIPE, &previous_pipe, NULL);
        return MW_EXIT_RUNTIME_ERROR;
    }
    /* poll says readable, but the connection may be gone by accept: never block there. */
    int wake[2];
    if (set_nonblocking_cloexec(listener) == -1 || open_wake_pipe(wake) == -1) {
        perror("listener setup");
        (void)close(listener);
        (void)sigaction(SIGPIPE, &previous_pipe, NULL);
        return MW_EXIT_RUNTIME_ERROR;
    }
    stop_requested = 0;
    wake_fd = wake[1];
    struct sigaction stop = {.sa_handler = on_stop_signal};
    struct sigaction previous_int;
    struct sigaction previous_term;
    (void)sigemptyset(&stop.sa_mask);
    bool int_set = sigaction(SIGINT, &stop, &previous_int) == 0;
    bool term_set = int_set && sigaction(SIGTERM, &stop, &previous_term) == 0;
    mw_runtime_t runtime = {.config = config};
    atomic_init(&runtime.stopping, false);
    mw_pool_t *pool = NULL;
    int result = MW_EXIT_RUNTIME_ERROR;
    if (!term_set) {
        perror("SIGINT/SIGTERM");
    } else if (mw_pool_create(&runtime, &pool) == -1) {
        perror("thread pool");
    } else {
        fprintf(stderr, "Listening on http://%s:%u (root: %s)\n", config->host,
                (unsigned int)config->port, config->root);
        bool clean = accept_loop(listener, wake[0], pool);
        atomic_store(&runtime.stopping, true);
        /* Order: stop accepting, drop queued connections, let running requests finish
         * (mw_pool_destroy stops the pool and joins the workers), then free everything. */
        (void)close(listener);
        listener = -1;
        mw_pool_destroy(pool);
        if (clean) {
            result = MW_EXIT_SUCCESS;
        }
    }
    if (listener != -1) {
        (void)close(listener);
    }
    if (term_set) {
        (void)sigaction(SIGTERM, &previous_term, NULL);
    }
    if (int_set) {
        (void)sigaction(SIGINT, &previous_int, NULL);
    }
    wake_fd = -1;
    (void)close(wake[0]);
    (void)close(wake[1]);
    (void)sigaction(SIGPIPE, &previous_pipe, NULL);
    return result;
}
