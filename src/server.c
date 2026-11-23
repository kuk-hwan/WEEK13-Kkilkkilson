#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* Darwin hides O_NOFOLLOW in its strict POSIX namespace. */
#define _DARWIN_C_SOURCE
#endif
#include "mini_webserver.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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

static void serve_index(const mw_config_t *config, int client) {
    int fd = openat(config->root_fd, "index.html", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        int status = errno == ENOENT || errno == EACCES || errno == ELOOP ? 403 : 500;
        send_error(client, status, false);
        return;
    }
    struct stat info;
    if (fstat(fd, &info) == -1) {
        (void)close(fd);
        send_error(client, 500, false);
        return;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        (void)close(fd);
        send_error(client, 403, false);
        return;
    }
    mw_deadline_t deadline = mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS;
    if (send_head(config, client, 200, (uintmax_t)info.st_size, deadline) == 0) {
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

static void handle_connection(const mw_config_t *config, int client) {
    mw_buffer_t input = {NULL, 0, 0};
    mw_request_t request;
    int result = mw_http_read_request(client, &input, &request,
                                      mw_net_now_ms() + MW_KEEP_ALIVE_IDLE_TIMEOUT_MS);
    if (result > 0) {
        send_error(client, result, false);
    } else if (result == -1) {
        if (errno == ETIMEDOUT && input.length > 0) {
            send_error(client, 408, false);
        }
    } else if (request.method.length == 4 && memcmp(request.method.data, "POST", 4) == 0) {
        send_error(client, 405, false); /* no POST route yet (#10) */
    } else if (request.path.length == 1 && request.query.length == 0) {
        serve_index(config, client);
    } else {
        send_error(client, 404, request.method.length == 4 && request.method.data[0] == 'H');
    }
    mw_http_buffer_destroy(&input);
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
    fprintf(stderr, "Listening on http://%s:%u (root: %s)\n", config->host,
            (unsigned int)config->port, config->root);
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client == -1) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            break;
        }
        handle_connection(config, client);
        (void)close(client);
    }
    (void)close(listener);
    (void)sigaction(SIGPIPE, &previous, NULL);
    return MW_EXIT_RUNTIME_ERROR;
}
