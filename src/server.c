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

static const char *reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 408:
        return "Request Timeout";
    case 431:
        return "Request Header Fields Too Large";
    default:
        return "Internal Server Error";
    }
}

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
                 status, reason(status), length, date, config->name);
    if (size < 0 || (size_t)size >= sizeof(head)) {
        return -1;
    }
    return mw_net_write_all(fd, head, (size_t)size, deadline);
}

static void send_error(const mw_config_t *config, int fd, int status) {
    char body[160];
    int size =
        snprintf(body, sizeof(body), "<!doctype html><h1>%d %s</h1>\n", status, reason(status));
    mw_deadline_t deadline = mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS;
    if (size >= 0 && (size_t)size < sizeof(body) &&
        send_head(config, fd, status, (uintmax_t)size, deadline) == 0) {
        (void)mw_net_write_all(fd, body, (size_t)size, deadline);
    }
}

static void serve_index(const mw_config_t *config, int client) {
    int fd = openat(config->root_fd, "index.html", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        int status = errno == ENOENT || errno == EACCES || errno == ELOOP ? 403 : 500;
        send_error(config, client, status);
        return;
    }
    struct stat info;
    if (fstat(fd, &info) == -1) {
        (void)close(fd);
        send_error(config, client, 500);
        return;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        (void)close(fd);
        send_error(config, client, 403);
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

static bool head_complete(const unsigned char *bytes, size_t size, size_t from) {
    for (size_t i = from; i < size; i++) {
        if (i >= 1 && bytes[i - 1] == '\n' && bytes[i] == '\n') {
            return true;
        }
        if (i >= 3 && memcmp(bytes + i - 3, "\r\n\r\n", 4) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_root_get(const unsigned char *bytes, size_t size) {
    const unsigned char *end = memchr(bytes, '\n', size);
    if (end == NULL) {
        return false;
    }
    size_t length = (size_t)(end - bytes);
    if (length > 0 && bytes[length - 1] == '\r') {
        length--;
    }
    const char http10[] = "GET / HTTP/1.0";
    const char http11[] = "GET / HTTP/1.1";
    return length == sizeof(http11) - 1 &&
           (memcmp(bytes, http10, length) == 0 || memcmp(bytes, http11, length) == 0);
}

static void handle_connection(const mw_config_t *config, int client) {
    unsigned char head[MW_MAX_HEADER_BYTES];
    size_t used = 0;
    mw_deadline_t deadline = mw_net_now_ms() + MW_KEEP_ALIVE_IDLE_TIMEOUT_MS;
    while (used < sizeof(head)) {
        ssize_t count = mw_net_read(client, head + used, sizeof(head) - used, deadline);
        if (count <= 0) {
            if (count == -1 && errno == ETIMEDOUT && used > 0) {
                send_error(config, client, 408);
            }
            return;
        }
        if (used == 0) {
            deadline = mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS;
        }
        size_t from = used > 3 ? used - 3 : 0;
        used += (size_t)count;
        if (head_complete(head, used, from)) {
            if (is_root_get(head, used)) {
                serve_index(config, client);
            } else {
                send_error(config, client, 404);
            }
            return;
        }
    }
    send_error(config, client, 431);
}

int mw_server_run(mw_config_t *config) {
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    struct sigaction previous;
    (void)sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &previous) == -1) {
        perror("SIGPIPE");
        return MW_EXIT_RUNTIME_ERROR;
    }
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
