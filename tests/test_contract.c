#include "mini_webserver.h"
#include "test.h"

/* Expected values are independent literals copied from docs/spec.md. */
bool test_spec_contract(void) {
    CHECK(MW_DEFAULT_PORT == 8080);
    CHECK(MW_DEFAULT_WORKERS == 8);
    CHECK(MW_DEFAULT_QUEUE_CAPACITY == 64);
    CHECK(MW_MAX_REQUEST_LINE == 8192U);
    CHECK(MW_MAX_HEADER_BYTES == 16384U);
    CHECK(MW_MAX_HEADERS == 64U);
    CHECK(MW_MAX_BODY_BYTES == 1048576U);
    CHECK(MW_MAX_UPSTREAM_BYTES == 8388608U);
    CHECK(MW_MAX_ROUTES == 16U);
    CHECK(MW_MAX_BACKENDS == 8U);
    CHECK(MW_MIN_WORKERS == 1U && MW_MAX_WORKERS == 128U);
    CHECK(MW_MIN_QUEUE_CAPACITY == 1U && MW_MAX_QUEUE_CAPACITY == 4096U);
    CHECK(MW_MAX_SERVER_NAME == 63U);
    CHECK(MW_REQUEST_TIMEOUT_MS == 5000);
    CHECK(MW_KEEP_ALIVE_IDLE_TIMEOUT_MS == 5000);
    CHECK(MW_UPSTREAM_TIMEOUT_MS == 3000);
    CHECK(MW_MAX_REQUESTS_PER_CONNECTION == 100U);
    CHECK(MW_EXIT_SUCCESS == 0);
    CHECK(MW_EXIT_RUNTIME_ERROR == 1);
    CHECK(MW_EXIT_ARGUMENT_ERROR == 2);
    return true;
}

/* Compile-only checks do not link unimplemented server functions. */
typedef mw_parse_result_t (*request_parser_t)(const unsigned char *, size_t, mw_request_t *);
typedef const char *(*mime_mapper_t)(mw_slice_t);
typedef int (*path_decoder_t)(mw_slice_t, char *, size_t);
typedef mw_route_t *(*route_matcher_t)(mw_route_t *, size_t, mw_slice_t);
typedef const mw_backend_t *(*backend_selector_t)(mw_route_t *);
typedef int (*upstream_parser_t)(const unsigned char *, size_t, bool, mw_upstream_response_t *);

#define CHECK_SIGNATURE(function, signature)                                                       \
    _Static_assert(_Generic(&(function), signature: 1, default: 0), #function " signature "        \
                                                                              "changed")

CHECK_SIGNATURE(mw_http_parse_request_head, request_parser_t);
CHECK_SIGNATURE(mw_static_mime_type, mime_mapper_t);
CHECK_SIGNATURE(mw_static_decode_path, path_decoder_t);
CHECK_SIGNATURE(mw_proxy_match_route, route_matcher_t);
CHECK_SIGNATURE(mw_proxy_next_backend, backend_selector_t);
CHECK_SIGNATURE(mw_http_parse_upstream_response, upstream_parser_t);
