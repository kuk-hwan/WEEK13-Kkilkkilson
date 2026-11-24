#include "mini_webserver.h"
#include "test.h"

#include <string.h>

static mw_slice_t view(const char *text) {
    return (mw_slice_t){(const unsigned char *)text, strlen(text)};
}

bool test_static_mime(void) {
    const struct {
        const char *path;
        const char *type;
    } cases[] = {{"a.html", "text/html; charset=utf-8"},
                 {"a.htm", "text/html; charset=utf-8"},
                 {"a.css", "text/css; charset=utf-8"},
                 {"a.js", "text/javascript; charset=utf-8"},
                 {"a.mjs", "text/javascript; charset=utf-8"},
                 {"a.json", "application/json; charset=utf-8"},
                 {"a.png", "image/png"},
                 {"a.jpg", "image/jpeg"},
                 {"a.jpeg", "image/jpeg"},
                 {"a.gif", "image/gif"},
                 {"a.svg", "image/svg+xml; charset=utf-8"},
                 {"a.ico", "image/x-icon"},
                 {"a.webp", "image/webp"},
                 {"a.txt", "text/plain; charset=utf-8"},
                 {"a.pdf", "application/pdf"},
                 {"a.wasm", "application/wasm"},
                 {"/a.CSS", "text/css; charset=utf-8"},
                 {"/a.JpEg", "image/jpeg"},
                 {"/a.bin", "application/octet-stream"},
                 {"/a", "application/octet-stream"},
                 {"/a.", "application/octet-stream"},
                 {"/a.css/file", "application/octet-stream"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(strcmp(mw_static_mime_type(view(cases[i].path)), cases[i].type) == 0);
    }
    /* A borrowed path view need not end with NUL. */
    CHECK(strcmp(mw_static_mime_type((mw_slice_t){(const unsigned char *)"x.png!", 5}),
                 "image/png") == 0);
    return true;
}

bool test_static_decode(void) {
    const struct {
        const char *input;
        const char *decoded;
    } cases[] = {{"/", ""},
                 {"/docs/a%20b.txt?x=%zz", "docs/a b.txt"},
                 {"/%ED%95%9C%EA%B8%80.txt", "한글.txt"},
                 {"/한글.txt", "한글.txt"},
                 {"/a%2fb//c/", "a/b/c/"},
                 {"////a", "a"},
                 {"/a%3fb%23c+z", "a?b#c+z"},
                 {"/a%252e", "a%2e"},
                 {"/.hidden/...", ".hidden/..."}};
    char decoded[128];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(mw_static_decode_path(view(cases[i].input), decoded, sizeof(decoded)) == 0);
        CHECK(strcmp(decoded, cases[i].decoded) == 0);
    }
    const char *bad_encoding[] = {"/%", "/%a", "/%zz", "/%0g", "/a%?q=xx"};
    for (size_t i = 0; i < sizeof(bad_encoding) / sizeof(bad_encoding[0]); i++) {
        CHECK(mw_static_decode_path(view(bad_encoding[i]), decoded, sizeof(decoded)) == 400);
    }
    const char *forbidden[] = {"/%00",      "/%01",        "/%1f",     "/%7f",    "/a\tb",
                               "/.",        "/..",         "/a/./b",   "/a/../b", "/%2e%2e",
                               "/%2E%2e/b", "/a%2f..%2fb", "/a/.?q=x", "/..//"};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        CHECK(mw_static_decode_path(view(forbidden[i]), decoded, sizeof(decoded)) == 403);
    }
    const unsigned char nul_path[] = {'/', 'a', 0, 'b'};
    CHECK(mw_static_decode_path((mw_slice_t){nul_path, sizeof(nul_path)}, decoded,
                                sizeof(decoded)) == 403);
    CHECK(mw_static_decode_path(view("/abc"), decoded, 4) == 0);
    CHECK(strcmp(decoded, "abc") == 0);
    CHECK(mw_static_decode_path(view("/abc"), decoded, 3) == 400);
    CHECK(mw_static_decode_path(view("/"), decoded, 1) == 0);
    CHECK(mw_static_decode_path(view("/"), decoded, 0) == 400);
    CHECK(mw_static_decode_path(view("abc"), decoded, sizeof(decoded)) == 400);
    CHECK(mw_static_decode_path(view(""), decoded, sizeof(decoded)) == 400);
    return true;
}
