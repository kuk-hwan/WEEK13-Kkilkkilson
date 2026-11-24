#include "mini_webserver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static _Thread_local uint64_t bytes_written;
static _Thread_local int counted_fd = -1;

void mw_net_count_writes_to(int fd) {
    counted_fd = fd;
}

uint64_t mw_net_bytes_written(void) {
    return bytes_written;
}

static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

mw_deadline_t mw_net_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_ready(int fd, short events, mw_deadline_t deadline) {
    for (;;) {
        mw_deadline_t now = mw_net_now_ms();
        if (now == -1) {
            return -1;
        }
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        int64_t remaining = deadline - now;
        int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd entry = {.fd = fd, .events = events};
        int result = poll(&entry, 1, timeout);
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            continue;
        }
        if (result < 0) {
            return -1;
        }
        if ((entry.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return -1;
        }
        /* recv/send resolves EOF, POLLHUP and socket errors. */
        return 0;
    }
}

int mw_net_listen(const mw_config_t *config) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    int reuse = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(config->port)};
    if (inet_pton(AF_INET, config->host, &address.sin_addr) != 1) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(fd, SOMAXCONN) == -1) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int mw_net_connect(const mw_backend_t *backend, mw_deadline_t deadline) {
    int fd = socket(backend->address.ss_family, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (nonblocking(fd) == -1) {
        error = errno;
    } else if (connect(fd, (const struct sockaddr *)&backend->address, backend->address_length) ==
               -1) {
        error = errno;
        if (error == EINPROGRESS || error == EINTR) {
            error = wait_ready(fd, POLLOUT, deadline) == -1 ? errno : 0;
            if (error == 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1) {
                error = errno;
            }
        }
    }
    if (error != 0) {
        (void)close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

ssize_t mw_net_read(int fd, void *buffer, size_t capacity, mw_deadline_t deadline) {
    if (capacity == 0) {
        return 0;
    }
    if (nonblocking(fd) == -1) {
        return -1;
    }
    for (;;) {
        if (wait_ready(fd, POLLIN, deadline) == -1) {
            return -1;
        }
        ssize_t count = recv(fd, buffer, capacity, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return count;
    }
}

int mw_net_write_all(int fd, const void *buffer, size_t length, mw_deadline_t deadline) {
    if (nonblocking(fd) == -1) {
        return -1;
    }
    const unsigned char *bytes = buffer;
    size_t sent = 0;
    while (sent < length) {
        if (wait_ready(fd, POLLOUT, deadline) == -1) {
            return -1;
        }
        size_t chunk = length - sent;
        if (chunk > 65536U) {
            chunk = 65536U;
        }
        ssize_t count = send(fd, bytes + sent, chunk, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EPIPE;
            }
            return -1;
        }
        sent += (size_t)count;
        if (fd == counted_fd) {
            bytes_written += (uint64_t)count;
        }
    }
    return 0;
}
