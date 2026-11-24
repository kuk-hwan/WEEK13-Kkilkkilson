#include "mini_webserver.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static mw_parse_result_t parse(const char *text, mw_request_t *request) {
    return mw_http_parse_request_head((const unsigned char *)text, strlen(text), request);
}

static bool slice_is(mw_slice_t slice, const char *expected) {
    return slice.length == strlen(expected) && memcmp(slice.data, expected, slice.length) == 0;
}

/* Expects exactly this status (0 = parse must not fail) from a complete text. */
static bool status_is(const char *text, int status) {
    mw_request_t request;
    mw_parse_result_t result = parse(text, &request);
    if (status == 0) {
        return result.state == MW_PARSE_COMPLETE && result.status == 0;
    }
    return result.state == MW_PARSE_ERROR && result.status == status && result.consumed == 0;
}

bool test_http_valid_requests(void) {
    mw_request_t request;
    const char get[] =
        "GET /a/b?x=1&y=2 HTTP/1.1\r\nhOsT: example\r\nX-One:  padded \t\r\n\r\nBODY";
    mw_parse_result_t result = parse(get, &request);
    CHECK(result.state == MW_PARSE_COMPLETE && result.status == 0);
    CHECK(result.consumed == strlen(get) - 4U); /* the trailing bytes are not ours */
    CHECK(slice_is(request.method, "GET") && slice_is(request.target, "/a/b?x=1&y=2"));
    CHECK(slice_is(request.path, "/a/b") && slice_is(request.query, "x=1&y=2"));
    CHECK(request.version == MW_HTTP_1_1 && request.keep_alive && !request.has_content_length);
    CHECK(request.headers.count == 2 && slice_is(request.headers.items[1].value, "padded"));

    CHECK(parse("HEAD / HTTP/1.0\nHost: h\n\n", &request).state == MW_PARSE_COMPLETE);
    CHECK(slice_is(request.method, "HEAD") && request.version == MW_HTTP_1_0);
    CHECK(!request.keep_alive && request.query.length == 0);
    CHECK(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &request).state ==
          MW_PARSE_COMPLETE);
    CHECK(request.keep_alive);
    CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: TE, Close\r\n\r\n", &request).state ==
          MW_PARSE_COMPLETE);
    CHECK(!request.keep_alive);

    result = parse("POST /p HTTP/1.1\r\nHost: h\r\ncontent-LENGTH: 5\r\ncontent-length: 5\r\n\r\n",
                   &request);
    CHECK(result.state == MW_PARSE_COMPLETE);
    CHECK(request.has_content_length && request.content_length == 5);
    CHECK(parse("POST /p HTTP/1.1\r\nHost: h\r\nContent-Length: 1048576\r\n\r\n", &request).state ==
          MW_PARSE_COMPLETE);
    CHECK(parse("POST /p HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n", &request).state ==
          MW_PARSE_COMPLETE);
    CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nExpect: 100-Continue\r\n\r\n", &request).state ==
          MW_PARSE_COMPLETE);
    CHECK(request.expect_continue);
    return true;
}

bool test_http_need_more(void) {
    const char full[] = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\n";
    mw_request_t request;
    for (size_t cut = 0; cut < strlen(full); cut++) {
        mw_parse_result_t result =
            mw_http_parse_request_head((const unsigned char *)full, cut, &request);
        CHECK(result.state == MW_PARSE_NEED_MORE && result.consumed == 0 && result.status == 0);
    }
    CHECK(parse(full, &request).state == MW_PARSE_COMPLETE);
    CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\n\r", &request).state == MW_PARSE_NEED_MORE);
    return true;
}

bool test_http_bad_request(void) {
    static const char *const bad[] = {
        "GET /\r\n\r\n",                                   /* no version */
        "GET  / HTTP/1.1\r\nHost: h\r\n\r\n",              /* double space */
        "GET / HTTP/1.1 x\r\nHost: h\r\n\r\n",             /* extra token */
        "\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n",           /* empty request line */
        "GET / HTTP/1\r\nHost: h\r\n\r\n",                 /* version format */
        "GET / http/1.1\r\nHost: h\r\n\r\n",               /* version case */
        "GET / HTTP/1.10\r\nHost: h\r\n\r\n",              /* version format */
        "G(T / HTTP/1.1\r\nHost: h\r\n\r\n",               /* method is not a token */
        "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n",       /* absolute-form */
        "GET * HTTP/1.1\r\nHost: h\r\n\r\n",               /* asterisk-form */
        "GET index HTTP/1.1\r\nHost: h\r\n\r\n",           /* not origin-form */
        "GET /a\x01 HTTP/1.1\r\nHost: h\r\n\r\n",          /* control byte in target */
        "GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n",    /* header without colon */
        "GET / HTTP/1.1\r\nHost: h\r\n: empty\r\n\r\n",    /* empty name */
        "GET / HTTP/1.1\r\nHost : h\r\n\r\n",              /* space in name */
        "GET / HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n",    /* obs-fold */
        "GET / HTTP/1.1\r\nHost: h\r\n\tfolded\r\n\r\n",   /* obs-fold with tab */
        "GET / HTTP/1.1\r\nHost: h\rX: y\r\n\r\n",         /* bare CR */
        "GET / HTTP/1.1\r\nHost: h\r\nX: a\x01 b\r\n\r\n", /* control byte in value */
        "GET / HTTP/1.1\r\n\r\n",                          /* HTTP/1.1 without Host */
        "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",    /* duplicate Host */
        "GET / HTTP/1.0\r\nHost: a\r\nHost: a\r\n\r\n",    /* duplicate Host, 1.0 */
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: \r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: +5\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5x\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5, 5\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999999999999999999\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (!status_is(bad[i], 400)) {
            fprintf(stderr, "expected 400 for case %zu: %s\n", i, bad[i]);
            return false;
        }
    }
    /* A NUL byte is not a C string, so use an explicit length. */
    const char nul[] = "GET / HTTP/1.1\r\nHost: h\0x\r\n\r\n";
    mw_request_t request;
    mw_parse_result_t result =
        mw_http_parse_request_head((const unsigned char *)nul, sizeof(nul) - 1U, &request);
    CHECK(result.state == MW_PARSE_ERROR && result.status == 400);
    return true;
}

bool test_http_status_codes(void) {
    CHECK(status_is("GET / HTTP/2.0\r\nHost: h\r\n\r\n", 505));
    CHECK(status_is("GET / HTTP/0.9\r\nHost: h\r\n\r\n", 505));
    CHECK(status_is("GET / HTTP/1.2\r\nHost: h\r\n\r\n", 505));
    CHECK(status_is("PUT / HTTP/1.1\r\nHost: h\r\n\r\n", 501));
    CHECK(status_is("DELETE / HTTP/1.1\r\nHost: h\r\n\r\n", 501));
    CHECK(status_is("get / HTTP/1.1\r\nHost: h\r\n\r\n", 501));
    CHECK(status_is("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n", 501));
    CHECK(status_is("GET / HTTP/1.1\r\nHost: h\r\ntransfer-encoding: gzip\r\n\r\n", 501));
    CHECK(status_is("POST / HTTP/1.1\r\nHost: h\r\n\r\n", 411));
    CHECK(status_is("POST / HTTP/1.0\r\n\r\n", 411));
    CHECK(status_is("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1048577\r\n\r\n", 413));
    CHECK(status_is("GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 2000000\r\n\r\n", 413));
    CHECK(status_is("GET / HTTP/1.1\r\nHost: h\r\nExpect: 200-ok\r\n\r\n", 417));
    CHECK(status_is("GET / HTTP/1.0\r\nExpect: foo\r\n\r\n", 417));
    return true;
}

bool test_http_limits(void) {
    mw_request_t request;
    /* Request line: exactly 8 KiB is fine, one more byte is 414. */
    size_t prefix = strlen("GET / HTTP/1.1");
    char *line = malloc(MW_MAX_HEADER_BYTES + 64U);
    CHECK(line != NULL);
    memcpy(line, "GET /", 5);
    memset(line + 5, 'a', MW_MAX_REQUEST_LINE - prefix);
    strcpy(line + 5 + MW_MAX_REQUEST_LINE - prefix, " HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK(parse(line, &request).state == MW_PARSE_COMPLETE);
    memmove(line + 6, line + 5, strlen(line + 5) + 1U);
    line[5] = 'a';
    mw_parse_result_t result = parse(line, &request);
    CHECK(result.state == MW_PARSE_ERROR && result.status == 414);
    /* Without a newline an over-long request line is already 414. */
    memset(line, 'G', MW_MAX_REQUEST_LINE + 2U);
    result =
        mw_http_parse_request_head((const unsigned char *)line, MW_MAX_REQUEST_LINE + 2U, &request);
    CHECK(result.state == MW_PARSE_ERROR && result.status == 414);
    result = mw_http_parse_request_head((const unsigned char *)line, MW_MAX_REQUEST_LINE, &request);
    CHECK(result.state == MW_PARSE_NEED_MORE);

    /* Header count: 64 pass, 65 is 431. */
    for (unsigned count = 63; count <= 65; count++) {
        char *p = line;
        p += sprintf(p, "GET / HTTP/1.1\r\nHost: h\r\n");
        for (unsigned i = 1; i < count; i++) {
            p += sprintf(p, "X%u: v\r\n", i);
        }
        strcpy(p, "\r\n");
        CHECK(status_is(line, count > MW_MAX_HEADERS ? 431 : 0));
    }

    /* Head size: exactly 16 KiB passes, one more byte is 431, also while still incomplete. */
    for (size_t extra = 0; extra <= 1; extra++) {
        const char start[] = "GET / HTTP/1.1\r\nHost: h\r\nX: ";
        size_t value = MW_MAX_HEADER_BYTES - strlen(start) - 4U + extra;
        memcpy(line, start, strlen(start));
        memset(line + strlen(start), 'v', value);
        strcpy(line + strlen(start) + value, "\r\n\r\n");
        CHECK(status_is(line, extra == 0 ? 0 : 431));
    }
    memcpy(line, "GET / HTTP/1.1\r\nHost: h\r\nX: ", 27);
    memset(line + 27, 'v', MW_MAX_HEADER_BYTES);
    result = mw_http_parse_request_head((const unsigned char *)line, MW_MAX_HEADER_BYTES, &request);
    CHECK(result.state == MW_PARSE_ERROR && result.status == 431);
    free(line);
    return true;
}

bool test_http_response_headers(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    mw_http_set_server_name("unit");
    mw_response_t response = {.status = 405, .body = {(const unsigned char *)"hello", 5}};
    response.headers.items[0] =
        (mw_header_t){{(const unsigned char *)"Allow", 5}, {(const unsigned char *)"GET", 3}};
    /* Generated headers supplied by the caller must not be duplicated. */
    response.headers.items[1] = (mw_header_t){{(const unsigned char *)"content-length", 14},
                                              {(const unsigned char *)"99", 2}};
    response.headers.count = 2;
    mw_deadline_t deadline = mw_net_now_ms() + 1000;
    CHECK(mw_http_send_response(pair[0], &response, false, true, deadline) == 0);
    CHECK(mw_http_send_response(pair[0], &response, true, false, deadline) == 0);
    CHECK(mw_http_send_error(pair[0], 417, false, deadline) == 0);
    char out[2048];
    size_t used = 0;
    for (int i = 0; i < 3; i++) { /* three responses, small enough to arrive together */
        ssize_t count = mw_net_read(pair[1], out + used, sizeof(out) - 1U - used, deadline);
        CHECK(count > 0);
        used += (size_t)count;
        out[used] = '\0';
        if (strstr(out, "417 Expectation Failed") != NULL && strstr(out, "</h1>") != NULL) {
            break;
        }
    }
    CHECK(strstr(out, "HTTP/1.1 405 Method Not Allowed\r\n") == out);
    CHECK(strstr(out, "Server: unit\r\n") != NULL && strstr(out, "Date: ") != NULL);
    CHECK(strstr(out, "Content-Length: 5\r\nConnection: keep-alive\r\n") != NULL);
    CHECK(strstr(out, "Content-Length: 99") == NULL && strstr(out, "Allow: GET\r\n") != NULL);
    CHECK(strstr(out, "\r\n\r\nhello") != NULL);
    /* HEAD: same headers (length of the real body), no body bytes. */
    char *head = strstr(strstr(out, "hello") + 5, "HTTP/1.1 405");
    CHECK(head != NULL && strstr(head, "Connection: close\r\n") != NULL);
    CHECK(strstr(head, "Content-Length: 5\r\n") != NULL);
    CHECK(strstr(head, "\r\n\r\nHTTP/1.1 417") != NULL);
    CHECK(strstr(out, "Content-Type: text/html; charset=utf-8\r\n") != NULL);
    (void)close(pair[0]);
    (void)close(pair[1]);
    return true;
}

bool test_http_read_request(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char text[] = "POST /p HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\nabcdGET /next";
    CHECK(write(pair[0], text, strlen(text)) == (ssize_t)strlen(text));
    mw_buffer_t input = {NULL, 0, 0};
    mw_request_t request;
    CHECK(mw_http_read_request(pair[1], &input, &request, mw_net_now_ms() + 1000) == 0);
    CHECK(slice_is(request.body, "abcd") && slice_is(request.path, "/p"));
    CHECK(input.length == strlen(text)); /* pipelined bytes stay buffered */
    mw_http_buffer_destroy(&input);

    /* Errors come back as the HTTP status, before the body is read. */
    CHECK(write(pair[0], "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 9999999\r\n\r\n", 54) ==
          54);
    CHECK(mw_http_read_request(pair[1], &input, &request, mw_net_now_ms() + 1000) == 413);
    mw_http_buffer_destroy(&input);

    /* EOF before a complete head is -1/ECONNRESET. */
    CHECK(write(pair[0], "GET / HT", 8) == 8);
    (void)close(pair[0]);
    CHECK(mw_http_read_request(pair[1], &input, &request, mw_net_now_ms() + 1000) == -1);
    CHECK(input.length == 8);
    mw_http_buffer_destroy(&input);
    CHECK(input.data == NULL && input.length == 0);
    (void)close(pair[1]);
    return true;
}

static int parse_upstream(const char *text, bool head_only, mw_upstream_response_t *response) {
    return mw_http_parse_upstream_response((const unsigned char *)text, strlen(text), head_only,
                                           response);
}

bool test_http_upstream_response(void) {
    mw_upstream_response_t response;
    CHECK(parse_upstream("HTTP/1.1 201 Created\r\nX-A: 1\r\nContent-Length: 3\r\n"
                         "Set-Cookie: a\r\nSet-Cookie: b\r\n\r\nabcEXTRA",
                         false, &response) == 0);
    CHECK(response.status == 201 && response.headers.count == 4);
    CHECK(slice_is(response.headers.items[3].value, "b"));
    CHECK(response.has_content_length && response.content_length == 3);
    CHECK(response.body.length == 3 && memcmp(response.body.data, "abc", 3) == 0);
    mw_http_upstream_destroy(&response);
    CHECK(response.body.data == NULL && response.body.length == 0);

    /* Without Content-Length the body runs to the close. LF-only lines are accepted. */
    CHECK(parse_upstream("HTTP/1.0 200 OK\nContent-Type: text/plain\n\nuntil\r\nclose", false,
                         &response) == 0);
    CHECK(!response.has_content_length && response.body.length == 12);
    CHECK(memcmp(response.body.data, "until\r\nclose", 12) == 0);
    mw_http_upstream_destroy(&response);
    CHECK(parse_upstream("HTTP/1.1 200\r\n\r\n", false, &response) == 0);
    CHECK(response.status == 200 && response.body.data == NULL);
    mw_http_upstream_destroy(&response);

    /* HEAD carries no body but keeps the length for the client. */
    CHECK(parse_upstream("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", true, &response) == 0);
    CHECK(response.content_length == 10 && response.body.length == 0);
    mw_http_upstream_destroy(&response);

    const char *broken[] = {"",
                            "HTTP/1.1 200 OK\r\nX: 1\r\n",
                            "HTTP/1.1 OK\r\n\r\n",
                            "HTTP/2 200 OK\r\n\r\n",
                            "HTTP/1.1 20 OK\r\n\r\n",
                            "HTTP/1.1 2000 OK\r\n\r\n",
                            "HTTP/1.1 099 Low\r\n\r\n",
                            "ICY 200 OK\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nNo-Colon\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nX: 1\r\n folded\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nX: a\rInjected: 1\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
                            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc"};
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
        CHECK(parse_upstream(broken[i], false, &response) == 502);
        mw_http_upstream_destroy(&response); /* safe after an error too */
    }
    return true;
}

bool test_http_upstream_chunked(void) {
    mw_upstream_response_t response;
    /* Several chunks, hex sizes in either case, a chunk extension, and a trailer that is dropped. */
    CHECK(parse_upstream("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\nSet-Cookie: a\r\n"
                         "Set-Cookie: b\r\n\r\n3\r\nabc\r\nA;ext=1\r\n0123456789\r\n"
                         "0\r\nX-Trailer: t\r\n\r\n",
                         false, &response) == 0);
    CHECK(response.body.length == 13 && memcmp(response.body.data, "abc0123456789", 13) == 0);
    CHECK(response.headers.count == 3); /* the trailer is not a header */
    CHECK(slice_is(response.headers.items[1].value, "a") &&
          slice_is(response.headers.items[2].value, "b"));
    mw_http_upstream_destroy(&response);

    CHECK(parse_upstream("HTTP/1.1 200 OK\nTransfer-Encoding: chunked\n\n2\nhi\n0\n\n", false,
                         &response) == 0);
    CHECK(response.body.length == 2 && memcmp(response.body.data, "hi", 2) == 0);
    mw_http_upstream_destroy(&response);
    CHECK(parse_upstream("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", false,
                         &response) == 0);
    CHECK(response.body.length == 0 && response.body.data == NULL);
    mw_http_upstream_destroy(&response);

    /* HEAD, 204 and 304 never have a body, whatever the framing headers say. */
    const char *bodyless[] = {"HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\nzz",
                              "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n\r\nabcde"};
    for (size_t i = 0; i < 2; i++) {
        CHECK(parse_upstream(bodyless[i], false, &response) == 0 && response.body.length == 0);
        mw_http_upstream_destroy(&response);
    }
    CHECK(parse_upstream("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz", true,
                         &response) == 0);
    CHECK(response.body.length == 0);
    mw_http_upstream_destroy(&response);

    const char *head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const char *broken[] = {"",                          /* no chunk at all */
                            "3\r\nabc\r\n",              /* no last chunk */
                            "3\r\nabc\r\n0\r\n",         /* trailer part never ends */
                            "3\r\nabc\r\n0\r\nX: 1\r\n", /* ... even with a trailer line */
                            "zz\r\nabc\r\n0\r\n\r\n",   /* size is not hex */
                            "\r\nabc\r\n0\r\n\r\n",     /* empty size */
                            "5\r\nabc\r\n0\r\n\r\n",    /* closed in the middle of a chunk */
                            "3\r\nabcde\r\n0\r\n\r\n",  /* data longer than the size */
                            "FFFFFFFFFF\r\nabc\r\n0\r\n\r\n"};
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
        char text[128];
        (void)snprintf(text, sizeof(text), "%s%s", head, broken[i]);
        CHECK(parse_upstream(text, false, &response) == 502);
        CHECK(response.body.data == NULL);
        mw_http_upstream_destroy(&response);
    }
    /* A coding other than a lone "chunked" cannot be forwarded as plain bytes. */
    CHECK(parse_upstream("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
                         false, &response) == 502);
    mw_http_upstream_destroy(&response);
    return true;
}

bool test_http_hop_by_hop(void) {
    mw_request_t request;
    CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: keep-alive, X-Hop\r\n\r\n", &request)
              .state == MW_PARSE_COMPLETE);
    const char *hop[] = {"Connection", "KEEP-ALIVE",        "proxy-connection", "TE",
                         "Trailer",    "Transfer-Encoding", "Upgrade",          "x-hop"};
    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++) {
        mw_slice_t name = {(const unsigned char *)hop[i], strlen(hop[i])};
        CHECK(mw_http_is_hop_by_hop(&request.headers, name));
    }
    const char *end_to_end[] = {"Host", "X-Hopper", "Set-Cookie", "Content-Type"};
    for (size_t i = 0; i < sizeof(end_to_end) / sizeof(end_to_end[0]); i++) {
        mw_slice_t name = {(const unsigned char *)end_to_end[i], strlen(end_to_end[i])};
        CHECK(!mw_http_is_hop_by_hop(&request.headers, name));
    }
    return true;
}
