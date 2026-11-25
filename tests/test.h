#ifndef MINI_WEBSERVER_TEST_H
#define MINI_WEBSERVER_TEST_H

#include <stdbool.h>
#include <stdio.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

typedef bool (*test_function_t)(void);

typedef struct {
    const char *name;
    test_function_t run;
} test_case_t;

bool test_spec_contract(void);
bool test_static_mime(void);
bool test_static_decode(void);
bool test_config_defaults(void);
bool test_config_options(void);
bool test_config_errors(void);
bool test_config_name(void);
bool test_config_pool_options(void);
bool test_config_route(void);
bool test_config_route_lists(void);
bool test_config_forward_allow(void);
bool test_pool_rejects_when_full_and_cleans_up(void);
bool test_pool_create_rejects_bad_sizes(void);
bool test_net_read_deadline(void);
bool test_net_write_all(void);
bool test_net_write_deadline(void);
bool test_net_bytes_written(void);
bool test_http_valid_requests(void);
bool test_http_need_more(void);
bool test_http_bad_request(void);
bool test_http_forward_targets(void);
bool test_http_status_codes(void);
bool test_http_limits(void);
bool test_http_response_headers(void);
bool test_http_read_request(void);
bool test_http_upstream_response(void);
bool test_http_upstream_chunked(void);
bool test_http_hop_by_hop(void);
bool test_http_last_response(void);
bool test_proxy_match_route(void);
bool test_proxy_round_robin(void);

#endif
