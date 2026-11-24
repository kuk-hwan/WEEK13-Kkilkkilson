#include "mini_webserver.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* Written once by mw_http_set_server_name before any worker starts; read-only afterwards. */
static char server_name[MW_MAX_SERVER_NAME + 1U] = "mini-webserver";

void mw_http_set_server_name(const char *name) {
    (void)snprintf(server_name, sizeof(server_name), "%s", name);
}

const char *mw_http_status_reason(int status) {
    switch (status) {
    case 100:
        return "Continue";
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 417:
        return "Expectation Failed";
    case 431:
        return "Request Header Fields Too Large";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Internal Server Error";
    }
}

/* ---- request parsing (pure) ---- */

static bool is_tchar(unsigned char c) {
    return c != 0 && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static bool is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static bool is_control(unsigned char c) {
    return c < 0x20 || c == 0x7f;
}

static mw_slice_t slice_of(const char *text) {
    return (mw_slice_t){(const unsigned char *)text, strlen(text)};
}

static bool slice_ieq(mw_slice_t a, mw_slice_t b) {
    return a.length == b.length &&
           strncasecmp((const char *)a.data, (const char *)b.data, a.length) == 0;
}

static bool ieq(mw_slice_t s, const char *literal) {
    return slice_ieq(s, slice_of(literal));
}

static bool seq(mw_slice_t s, const char *literal) {
    return s.length == strlen(literal) && memcmp(s.data, literal, s.length) == 0;
}

static mw_slice_t trim(const unsigned char *p, size_t n) {
    while (n > 0 && (*p == ' ' || *p == '\t')) {
        p++;
        n--;
    }
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
        n--;
    }
    return (mw_slice_t){p, n};
}

/* Comma-separated token list (Connection), case-insensitive. */
static bool has_token(mw_slice_t list, mw_slice_t token) {
    size_t i = 0;
    while (i <= list.length) {
        const unsigned char *comma = memchr(list.data + i, ',', list.length - i);
        size_t end = comma != NULL ? (size_t)(comma - list.data) : list.length;
        if (slice_ieq(trim(list.data + i, end - i), token)) {
            return true;
        }
        i = end + 1U;
    }
    return false;
}

/* Content-Length value: digits only, without overflow. */
static bool parse_length(mw_slice_t value, uint64_t *length) {
    uint64_t parsed = 0;
    if (value.length == 0) {
        return false;
    }
    for (size_t i = 0; i < value.length; i++) {
        if (!is_digit(value.data[i]) ||
            parsed > (UINT64_MAX - (uint64_t)(value.data[i] - '0')) / 10U) {
            return false;
        }
        parsed = parsed * 10U + (uint64_t)(value.data[i] - '0');
    }
    *length = parsed;
    return true;
}

bool mw_http_is_hop_by_hop(const mw_headers_t *headers, mw_slice_t name) {
    static const char *const fixed[] = {"connection", "keep-alive",        "proxy-connection", "te",
                                        "trailer",    "transfer-encoding", "upgrade"};
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (ieq(name, fixed[i])) {
            return true;
        }
    }
    for (size_t i = 0; i < headers->count; i++) {
        if (ieq(headers->items[i].name, "connection") && has_token(headers->items[i].value, name)) {
            return true;
        }
    }
    return false;
}

static mw_parse_result_t fail(int status) {
    return (mw_parse_result_t){MW_PARSE_ERROR, 0, status};
}

/* Returns 0 or the HTTP error status. Method 501 is last so format errors win. */
static int parse_request_line(const unsigned char *line, size_t length, mw_request_t *request) {
    const unsigned char *end = line + length;
    const unsigned char *sp1 = memchr(line, ' ', length);
    const unsigned char *sp2 = sp1 != NULL ? memchr(sp1 + 1, ' ', (size_t)(end - sp1) - 1U) : NULL;
    if (sp2 == NULL) {
        return 400;
    }
    mw_slice_t method = {line, (size_t)(sp1 - line)};
    mw_slice_t target = {sp1 + 1, (size_t)(sp2 - sp1) - 1U};
    mw_slice_t version = {sp2 + 1, (size_t)(end - sp2) - 1U};
    if (method.length == 0 || target.length == 0 || target.data[0] != '/') {
        return 400;
    }
    for (size_t i = 0; i < method.length; i++) {
        if (!is_tchar(method.data[i])) {
            return 400;
        }
    }
    for (size_t i = 0; i < target.length; i++) {
        if (is_control(target.data[i])) {
            return 400;
        }
    }
    if (version.length != 8 || memcmp(version.data, "HTTP/", 5) != 0 ||
        !is_digit(version.data[5]) || version.data[6] != '.' || !is_digit(version.data[7])) {
        return 400;
    }
    if (version.data[5] != '1' || (version.data[7] != '0' && version.data[7] != '1')) {
        return 505;
    }
    /* Method names are case-sensitive: "get" is not GET. */
    if (!seq(method, "GET") && !seq(method, "HEAD") && !seq(method, "POST")) {
        return 501;
    }
    const unsigned char *query = memchr(target.data, '?', target.length);
    size_t path_length = query != NULL ? (size_t)(query - target.data) : target.length;
    request->method = method;
    request->target = target;
    request->path = (mw_slice_t){target.data, path_length};
    request->query = query != NULL ? (mw_slice_t){query + 1, target.length - path_length - 1U}
                                   : (mw_slice_t){target.data + target.length, 0};
    request->version = version.data[7] == '1' ? MW_HTTP_1_1 : MW_HTTP_1_0;
    return 0;
}

mw_parse_result_t mw_http_parse_request_head(const unsigned char *bytes, size_t length,
                                             mw_request_t *request) {
    mw_request_t req;
    memset(&req, 0, sizeof(req));
    size_t pos = 0;
    bool first = true;
    unsigned host_count = 0;
    bool transfer_encoding = false;
    bool conn_close = false;
    bool conn_keep_alive = false;
    bool has_expect = false;
    mw_slice_t expect = {NULL, 0};
    uint64_t content_length = 0;

    for (;;) {
        const unsigned char *newline =
            length > pos ? memchr(bytes + pos, '\n', length - pos) : NULL;
        if (newline == NULL) {
            if (first && length - pos > MW_MAX_REQUEST_LINE + 1U) {
                return fail(414);
            }
            return length >= MW_MAX_HEADER_BYTES ? fail(431)
                                                 : (mw_parse_result_t){MW_PARSE_NEED_MORE, 0, 0};
        }
        size_t line_end = (size_t)(newline - bytes);
        size_t end = line_end > pos && bytes[line_end - 1] == '\r' ? line_end - 1U : line_end;
        const unsigned char *line = bytes + pos;
        size_t line_length = end - pos;
        if (first && line_length > MW_MAX_REQUEST_LINE) {
            return fail(414);
        }
        if (line_end + 1U > MW_MAX_HEADER_BYTES) {
            return fail(431);
        }
        if (memchr(line, '\r', line_length) != NULL || memchr(line, '\0', line_length) != NULL) {
            return fail(400);
        }
        pos = line_end + 1U;
        if (first) {
            int status = parse_request_line(line, line_length, &req);
            if (status != 0) {
                return fail(status);
            }
            first = false;
            continue;
        }
        if (line_length == 0) {
            break;
        }
        if (line[0] == ' ' || line[0] == '\t') { /* obs-fold */
            return fail(400);
        }
        const unsigned char *colon = memchr(line, ':', line_length);
        if (colon == NULL || colon == line) {
            return fail(400);
        }
        for (const unsigned char *c = line; c < colon; c++) {
            if (!is_tchar(*c)) {
                return fail(400);
            }
        }
        if (req.headers.count == MW_MAX_HEADERS) {
            return fail(431);
        }
        mw_slice_t name = {line, (size_t)(colon - line)};
        mw_slice_t value = trim(colon + 1, line_length - name.length - 1U);
        for (size_t i = 0; i < value.length; i++) {
            if (is_control(value.data[i]) && value.data[i] != '\t') {
                return fail(400);
            }
        }
        req.headers.items[req.headers.count++] = (mw_header_t){name, value};

        if (ieq(name, "content-length")) {
            uint64_t parsed = 0;
            if (!parse_length(value, &parsed) ||
                (req.has_content_length && parsed != content_length)) {
                return fail(400);
            }
            req.has_content_length = true;
            content_length = parsed;
        } else if (ieq(name, "transfer-encoding")) {
            transfer_encoding = true;
        } else if (ieq(name, "host")) {
            host_count++;
        } else if (ieq(name, "expect")) {
            has_expect = true;
            expect = value;
        } else if (ieq(name, "connection")) {
            conn_close = conn_close || has_token(value, slice_of("close"));
            conn_keep_alive = conn_keep_alive || has_token(value, slice_of("keep-alive"));
        }
    }

    bool http11 = req.version == MW_HTTP_1_1;
    if (host_count > 1U || (http11 && host_count == 0U)) {
        return fail(400);
    }
    if (transfer_encoding) {
        return fail(501);
    }
    if (req.has_content_length) {
        if (content_length > MW_MAX_BODY_BYTES) {
            return fail(413);
        }
        req.content_length = (size_t)content_length;
    } else if (seq(req.method, "POST")) {
        return fail(411);
    }
    if (has_expect) {
        if (!ieq(expect, "100-continue")) {
            return fail(417);
        }
        req.expect_continue = http11;
    }
    req.keep_alive = !conn_close && (http11 || conn_keep_alive);
    *request = req;
    return (mw_parse_result_t){MW_PARSE_COMPLETE, pos, 0};
}

/* ---- request reading ---- */

static int reserve(mw_buffer_t *buffer, size_t need) {
    if (need <= buffer->capacity) {
        return 0;
    }
    size_t capacity = buffer->capacity != 0 ? buffer->capacity : 4096U;
    while (capacity < need) {
        capacity *= 2U;
    }
    unsigned char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
        return -1;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return 0;
}

/*
 * Returns 0 (request complete, head+body in `input`), an HTTP error status, or -1 with errno
 * (ETIMEDOUT, ECONNRESET for EOF, ENOMEM, ...). Bytes already in `input` (pipelined) stay there:
 * the caller drops consumed + content_length bytes after answering. Views in `request` borrow
 * `input->data`. `deadline` is the idle deadline while `input` is empty; the first byte replaces
 * it with the 5 s request deadline. With bytes already buffered it is the request deadline.
 */
int mw_http_read_request(int fd, mw_buffer_t *input, mw_request_t *request,
                         mw_deadline_t deadline) {
    bool idle = input->length == 0;
    bool continue_sent = false;
    size_t total = 0; /* head + body bytes, known once the head is complete */
    for (;;) {
        if (input->length > 0) {
            mw_parse_result_t result =
                mw_http_parse_request_head(input->data, input->length, request);
            if (result.state == MW_PARSE_ERROR) {
                return result.status;
            }
            if (result.state == MW_PARSE_COMPLETE) {
                total = result.consumed + request->content_length;
                if (input->length >= total) {
                    request->body =
                        (mw_slice_t){input->data + result.consumed, request->content_length};
                    return 0;
                }
                if (request->expect_continue && !continue_sent) {
                    static const char line[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    if (mw_net_write_all(fd, line, sizeof(line) - 1U, deadline) == -1) {
                        return -1;
                    }
                    continue_sent = true;
                }
            }
        }
        if (reserve(input, total != 0 ? total : input->length + 4096U) == -1) {
            errno = ENOMEM;
            return -1;
        }
        ssize_t count =
            mw_net_read(fd, input->data + input->length, input->capacity - input->length, deadline);
        if (count <= 0) {
            if (count == 0) {
                errno = ECONNRESET;
            }
            return -1;
        }
        input->length += (size_t)count;
        if (idle) {
            idle = false;
            deadline = mw_net_now_ms() + MW_REQUEST_TIMEOUT_MS;
        }
    }
}

void mw_http_buffer_destroy(mw_buffer_t *buffer) {
    free(buffer->data);
    *buffer = (mw_buffer_t){NULL, 0, 0};
}

/* ---- upstream responses (pure) ---- */

/* "HTTP/1.x SP 3DIGIT [SP reason]". Returns the status, or 0 when the line is malformed. */
static int parse_status_line(const unsigned char *line, size_t length) {
    if (length < 12 || memcmp(line, "HTTP/1.", 7) != 0 || !is_digit(line[7]) || line[8] != ' ' ||
        !is_digit(line[9]) || !is_digit(line[10]) || !is_digit(line[11]) ||
        (length > 12 && line[12] != ' ') || line[9] == '0') {
        return 0;
    }
    return (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
}

/* End of the line at `pos` (CRLF or LF), or SIZE_MAX when the data ends first. */
static size_t line_end_at(const unsigned char *bytes, size_t length, size_t pos, size_t *next) {
    const unsigned char *newline = pos < length ? memchr(bytes + pos, '\n', length - pos) : NULL;
    if (newline == NULL) {
        return SIZE_MAX;
    }
    size_t line_end = (size_t)(newline - bytes);
    *next = line_end + 1U;
    return line_end > pos && bytes[line_end - 1] == '\r' ? line_end - 1U : line_end;
}

/* Decodes "size[;ext] CRLF data CRLF ... 0 CRLF trailer* CRLF" into `body`; trailers go. */
static int decode_chunked(const unsigned char *bytes, size_t length, mw_buffer_t *body) {
    size_t pos = 0;
    for (;;) {
        size_t next = 0;
        size_t end = line_end_at(bytes, length, pos, &next);
        if (end == SIZE_MAX) {
            return 502; /* closed before the last chunk */
        }
        size_t size = 0;
        size_t digits = 0;
        for (; pos + digits < end && isxdigit(bytes[pos + digits]); digits++) {
            unsigned char c = bytes[pos + digits];
            size = size * 16U + (size_t)(is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10);
            if (digits >= 8) {
                return 502; /* absurd chunk size */
            }
        }
        if (digits == 0 || (pos + digits < end && bytes[pos + digits] != ';')) {
            return 502;
        }
        pos = next;
        if (size == 0) {
            break;
        }
        if (size > length - pos) {
            return 502;
        }
        unsigned char *grown = realloc(body->data, body->length + size);
        if (grown == NULL) {
            return 502;
        }
        body->data = grown;
        memcpy(body->data + body->length, bytes + pos, size);
        body->length += size;
        body->capacity = body->length;
        pos += size;
        end = line_end_at(bytes, length, pos, &next);
        if (end != pos) {
            return 502; /* data not followed by a line end */
        }
        pos = next;
    }
    for (;;) { /* trailer lines up to the empty line */
        size_t next = 0;
        size_t end = line_end_at(bytes, length, pos, &next);
        if (end == SIZE_MAX) {
            return 502;
        }
        if (end == pos) {
            return 0;
        }
        pos = next;
    }
}

/*
 * `bytes` is everything the backend sent before closing. Header views borrow `bytes`; the body is
 * copied. The body is Content-Length bytes when given, otherwise everything up to the close.
 */
int mw_http_parse_upstream_response(const unsigned char *bytes, size_t length, bool head_only,
                                    mw_upstream_response_t *response) {
    memset(response, 0, sizeof(*response));
    size_t pos = 0;
    bool chunked = false;
    for (;;) {
        const unsigned char *newline =
            length > pos ? memchr(bytes + pos, '\n', length - pos) : NULL;
        if (newline == NULL) {
            return 502; /* closed before the end of the head */
        }
        size_t line_end = (size_t)(newline - bytes);
        size_t end = line_end > pos && bytes[line_end - 1] == '\r' ? line_end - 1U : line_end;
        const unsigned char *line = bytes + pos;
        size_t line_length = end - pos;
        pos = line_end + 1U;
        /* A bare CR could split a header line on the client side. */
        if (memchr(line, '\r', line_length) != NULL || memchr(line, '\0', line_length) != NULL) {
            return 502;
        }
        if (response->status == 0) {
            response->status = parse_status_line(line, line_length);
            if (response->status == 0) {
                return 502;
            }
            continue;
        }
        if (line_length == 0) {
            break;
        }
        const unsigned char *colon = memchr(line, ':', line_length);
        if (colon == NULL || colon == line || response->headers.count == MW_MAX_HEADERS) {
            return 502;
        }
        for (const unsigned char *c = line; c < colon; c++) {
            if (!is_tchar(*c)) { /* also rejects obs-fold */
                return 502;
            }
        }
        mw_slice_t name = {line, (size_t)(colon - line)};
        mw_slice_t value = trim(colon + 1, line_length - name.length - 1U);
        response->headers.items[response->headers.count++] = (mw_header_t){name, value};
        if (ieq(name, "content-length")) {
            uint64_t parsed = 0;
            if (!parse_length(value, &parsed) ||
                (response->has_content_length && parsed != response->content_length)) {
                return 502;
            }
            response->has_content_length = true;
            response->content_length = (size_t)parsed;
        } else if (ieq(name, "transfer-encoding")) {
            /* Only a lone "chunked" is understood; gzip etc. would reach the client mislabeled. */
            if (chunked || !ieq(value, "chunked")) {
                return 502;
            }
            chunked = true;
        }
    }
    if (head_only || response->status == 204 || response->status == 304) {
        return 0;
    }
    if (chunked) {
        int status = decode_chunked(bytes + pos, length - pos, &response->body);
        if (status != 0) {
            mw_http_upstream_destroy(response);
        }
        return status;
    }
    size_t body_length = length - pos;
    if (response->has_content_length) {
        if (response->content_length > body_length) {
            return 502; /* closed in the middle of the body */
        }
        body_length = response->content_length;
    }
    if (body_length > 0) {
        response->body.data = malloc(body_length);
        if (response->body.data == NULL) {
            return 502;
        }
        memcpy(response->body.data, bytes + pos, body_length);
        response->body.length = body_length;
        response->body.capacity = body_length;
    }
    return 0;
}

void mw_http_upstream_destroy(mw_upstream_response_t *response) {
    free(response->body.data);
    response->body = (mw_buffer_t){NULL, 0, 0};
}

/* ---- responses ---- */

static bool is_generated_header(mw_slice_t name) {
    return ieq(name, "content-length") || ieq(name, "connection") || ieq(name, "date") ||
           ieq(name, "server");
}

int mw_http_send_response(int fd, const mw_response_t *response, bool head_only, bool keep_alive,
                          mw_deadline_t deadline) {
    time_t now = time(NULL);
    struct tm utc;
    char date[64];
    if (gmtime_r(&now, &utc) == NULL ||
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc) == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t capacity = 256U + strlen(server_name);
    for (size_t i = 0; i < response->headers.count; i++) {
        capacity +=
            response->headers.items[i].name.length + response->headers.items[i].value.length + 4U;
    }
    /* Small bodies share the head's write so Nagle never splits them. */
    bool inline_body = !head_only && response->body.length <= 8192U;
    if (inline_body) {
        capacity += response->body.length;
    }
    char *out = malloc(capacity);
    if (out == NULL) {
        errno = ENOMEM;
        return -1;
    }
    int size = snprintf(out, capacity,
                        "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: %s\r\nContent-Length: %zu\r\n"
                        "Connection: %s\r\n",
                        response->status, mw_http_status_reason(response->status), date,
                        server_name, response->body.length, keep_alive ? "keep-alive" : "close");
    size_t length = (size_t)size;
    for (size_t i = 0; i < response->headers.count; i++) {
        const mw_header_t *header = &response->headers.items[i];
        if (!is_generated_header(header->name)) {
            memcpy(out + length, header->name.data, header->name.length);
            length += header->name.length;
            out[length++] = ':';
            out[length++] = ' ';
            memcpy(out + length, header->value.data, header->value.length);
            length += header->value.length;
            out[length++] = '\r';
            out[length++] = '\n';
        }
    }
    out[length++] = '\r';
    out[length++] = '\n';
    if (inline_body && response->body.length > 0) {
        memcpy(out + length, response->body.data, response->body.length);
        length += response->body.length;
    }
    int result = mw_net_write_all(fd, out, length, deadline);
    free(out);
    if (result == 0 && !head_only && !inline_body) {
        result = mw_net_write_all(fd, response->body.data, response->body.length, deadline);
    }
    return result;
}

/* Error pages always close the connection (spec "오류 뒤 연결"). */
int mw_http_send_error(int fd, int status, bool head_only, mw_deadline_t deadline) {
    char body[160];
    int size = snprintf(body, sizeof(body), "<!doctype html><h1>%d %s</h1>\n", status,
                        mw_http_status_reason(status));
    mw_response_t response = {.status = status,
                              .body = {(const unsigned char *)body, (size_t)size}};
    response.headers.items[0] =
        (mw_header_t){{(const unsigned char *)"Content-Type", 12},
                      {(const unsigned char *)"text/html; charset=utf-8", 24}};
    response.headers.count = 1;
    if (status == 405) {
        response.headers.items[1] = (mw_header_t){{(const unsigned char *)"Allow", 5},
                                                  {(const unsigned char *)"GET, HEAD", 9}};
        response.headers.count = 2;
    }
    return mw_http_send_response(fd, &response, head_only, false, deadline);
}
