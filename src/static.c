#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "mini_webserver.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *mw_static_mime_type(mw_slice_t path) {
    static const struct {
        const char *extension;
        const char *type;
    } types[] = {{"html", "text/html; charset=utf-8"},
                 {"htm", "text/html; charset=utf-8"},
                 {"css", "text/css; charset=utf-8"},
                 {"js", "text/javascript; charset=utf-8"},
                 {"mjs", "text/javascript; charset=utf-8"},
                 {"json", "application/json; charset=utf-8"},
                 {"png", "image/png"},
                 {"jpg", "image/jpeg"},
                 {"jpeg", "image/jpeg"},
                 {"gif", "image/gif"},
                 {"svg", "image/svg+xml; charset=utf-8"},
                 {"ico", "image/x-icon"},
                 {"webp", "image/webp"},
                 {"txt", "text/plain; charset=utf-8"},
                 {"pdf", "application/pdf"},
                 {"wasm", "application/wasm"}};
    size_t extension = path.length;
    for (size_t i = path.length; i > 0; i--) {
        if (path.data[i - 1] == '/') {
            break;
        }
        if (path.data[i - 1] == '.') {
            extension = i;
            break;
        }
    }
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t length = strlen(types[i].extension);
        if (path.length - extension == length &&
            strncasecmp((const char *)path.data + extension, types[i].extension, length) == 0) {
            return types[i].type;
        }
    }
    return "application/octet-stream";
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int mw_static_decode_path(mw_slice_t path, char *relative_path, size_t capacity) {
    if (path.length == 0 || path.data[0] != '/' || capacity == 0) {
        return 400;
    }
    size_t used = 0;
    for (size_t i = 0; i < path.length && path.data[i] != '?'; i++) {
        unsigned char c = path.data[i];
        if (c == '%') {
            if (i + 2U >= path.length || hex_digit(path.data[i + 1]) < 0 ||
                hex_digit(path.data[i + 2]) < 0) {
                return 400;
            }
            c = (unsigned char)(hex_digit(path.data[i + 1]) * 16 + hex_digit(path.data[i + 2]));
            i += 2U;
        }
        if (c < 0x20 || c == 0x7f) {
            return 403;
        }
        /* Collapse separators, and never produce an absolute openat path. */
        if (c == '/' && (used == 0 || relative_path[used - 1] == '/')) {
            continue;
        }
        if (used + 1U >= capacity) {
            return 400;
        }
        relative_path[used++] = (char)c;
    }
    relative_path[used] = '\0';
    const char *component = relative_path;
    for (size_t i = 0; i <= used; i++) {
        if (relative_path[i] == '/' || relative_path[i] == '\0') {
            size_t length = (size_t)(relative_path + i - component);
            if ((length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.')) {
                return 403;
            }
            component = relative_path + i + 1U;
        }
    }
    return 0;
}

static int open_status(int error) {
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        return 404;
    case EACCES:
    case ELOOP:
        return 403;
    default:
        return 500;
    }
}

/* Reject sockets/devices/FIFOs before open; the opened FD is still checked afterwards. */
static int open_component(int directory, const char *name, int *status) {
    struct stat info;
    if (fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) == -1) {
        *status = open_status(errno);
        return -1;
    }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
        *status = 403;
        return -1;
    }
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        *status = open_status(errno);
    }
    return fd;
}

/* Each descriptor anchors the next lookup; no checked pathname is reopened. */
static int open_file(int root_fd, char *path, struct stat *info, bool *index, int *status) {
    int current = openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current == -1) {
        *status = open_status(errno);
        return -1;
    }
    char *component = path;
    while (*component != '\0') {
        char *slash = strchr(component, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
        int next = open_component(current, component, status);
        (void)close(current);
        if (next == -1) {
            return -1;
        }
        current = next;
        if (fstat(current, info) == -1) {
            *status = 500;
            (void)close(current);
            return -1;
        }
        if (slash != NULL && !S_ISDIR(info->st_mode)) {
            *status = 404;
            (void)close(current);
            return -1;
        }
        component = slash != NULL ? slash + 1 : component + strlen(component);
    }
    if (fstat(current, info) == -1) {
        *status = 500;
        (void)close(current);
        return -1;
    }
    if (S_ISDIR(info->st_mode)) {
        int next = open_component(current, "index.html", status);
        (void)close(current);
        if (next == -1) {
            if (*status == 404) {
                *status = 403;
            }
            return -1;
        }
        current = next;
        *index = true;
        if (fstat(current, info) == -1) {
            *status = 500;
            (void)close(current);
            return -1;
        }
    }
    if (!S_ISREG(info->st_mode) || info->st_size < 0) {
        *status = 403;
        (void)close(current);
        return -1;
    }
    return current;
}

int mw_static_serve(const mw_config_t *config, int client_fd, const mw_request_t *request,
                    bool keep_alive, mw_deadline_t deadline) {
    bool head = request->method.length == 4 && memcmp(request->method.data, "HEAD", 4) == 0;
    bool get = request->method.length == 3 && memcmp(request->method.data, "GET", 3) == 0;
    if (!head && !get) {
        return mw_http_send_error(client_fd, 405, false, deadline);
    }
    char path[MW_MAX_REQUEST_LINE + 1U];
    int status = mw_static_decode_path(request->target, path, sizeof(path));
    if (status != 0) {
        return mw_http_send_error(client_fd, status, head, deadline);
    }
    const char *type = mw_static_mime_type((mw_slice_t){(const unsigned char *)path, strlen(path)});
    bool index = false;
    struct stat info;
    int file = open_file(config->root_fd, path, &info, &index, &status);
    if (file == -1) {
        return mw_http_send_error(client_fd, status, head, deadline);
    }
    if ((uintmax_t)info.st_size > SIZE_MAX) {
        (void)close(file);
        return mw_http_send_error(client_fd, 500, head, deadline);
    }
    if (index) {
        type = "text/html; charset=utf-8";
    }
    mw_response_t response = {.status = 200, .body = {NULL, (size_t)info.st_size}};
    response.headers.items[0] = (mw_header_t){{(const unsigned char *)"Content-Type", 12},
                                              {(const unsigned char *)type, strlen(type)}};
    response.headers.count = 1;
    /* Serialize only the head, advertising the file length without loading its body. */
    int result = mw_http_send_response(client_fd, &response, true, keep_alive, deadline);
    off_t remaining = head ? 0 : info.st_size;
    unsigned char buffer[8192];
    while (result == 0 && remaining > 0) {
        size_t capacity = remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t count = read(file, buffer, capacity);
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EIO;
            }
            result = -1;
        } else {
            result = mw_net_write_all(client_fd, buffer, (size_t)count, deadline);
            remaining -= count;
        }
    }
    (void)close(file);
    return result;
}
