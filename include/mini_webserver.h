#ifndef MINI_WEBSERVER_H
#define MINI_WEBSERVER_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/* Public limits are from docs/spec.md. Sizes count bytes, not characters. */
#define MW_DEFAULT_HOST "127.0.0.1"
#define MW_DEFAULT_PORT 8080
#define MW_DEFAULT_ROOT "public"
#define MW_DEFAULT_WORKERS 8
#define MW_DEFAULT_QUEUE_CAPACITY 64
#define MW_MAX_REQUEST_LINE (8U * 1024U)
#define MW_MAX_HEADER_BYTES (16U * 1024U)
#define MW_MAX_HEADERS 64U
#define MW_MAX_BODY_BYTES (1024U * 1024U)
#define MW_MAX_UPSTREAM_BYTES (8U * 1024U * 1024U)
#define MW_MAX_ROUTES 16U
#define MW_MAX_BACKENDS 8U
#define MW_MIN_WORKERS 1U
#define MW_MAX_WORKERS 128U
#define MW_MIN_QUEUE_CAPACITY 1U
#define MW_MAX_QUEUE_CAPACITY 4096U
#define MW_MAX_SERVER_NAME 63U
#define MW_MAX_BACKEND_HOST 255U
#define MW_REQUEST_TIMEOUT_MS 5000
#define MW_KEEP_ALIVE_IDLE_TIMEOUT_MS 5000
#define MW_UPSTREAM_TIMEOUT_MS 3000
#define MW_MAX_REQUESTS_PER_CONNECTION 100U

typedef enum {
    MW_EXIT_SUCCESS = 0,
    MW_EXIT_RUNTIME_ERROR = 1,
    MW_EXIT_ARGUMENT_ERROR = 2
} mw_exit_code_t;

/* A borrowed, binary-safe view. It is not necessarily NUL-terminated. */
typedef struct {
    const unsigned char *data;
    size_t length;
} mw_slice_t;

/* Owned storage. The module that allocates it also releases it. */
typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} mw_buffer_t;

typedef struct {
    mw_slice_t name;
    mw_slice_t value;
} mw_header_t;

/* Keep duplicates and original order; Set-Cookie must not be merged. */
typedef struct {
    mw_header_t items[MW_MAX_HEADERS];
    size_t count;
} mw_headers_t;

typedef enum { MW_HTTP_1_0, MW_HTTP_1_1 } mw_http_version_t;

typedef struct {
    mw_slice_t method;
    mw_slice_t target;
    mw_slice_t path;
    mw_slice_t query;
    mw_http_version_t version;
    mw_headers_t headers;
    mw_slice_t body;
    size_t content_length;
    bool has_content_length;
    bool keep_alive;
    bool expect_continue;
} mw_request_t;

typedef enum { MW_PARSE_COMPLETE, MW_PARSE_NEED_MORE, MW_PARSE_ERROR } mw_parse_state_t;

typedef struct {
    mw_parse_state_t state;
    /* Header bytes consumed only on COMPLETE; zero on NEED_MORE or ERROR. */
    size_t consumed;
    /* HTTP error code only on ERROR; otherwise zero. */
    int status;
} mw_parse_result_t;

typedef struct {
    char host[MW_MAX_BACKEND_HOST + 1U];
    uint16_t port;
    struct sockaddr_storage address;
    socklen_t address_length;
} mw_backend_t;

typedef struct {
    char prefix[MW_MAX_REQUEST_LINE + 1U];
    mw_backend_t backends[MW_MAX_BACKENDS];
    size_t backend_count;
    size_t next_backend;
    /* Only protects next_backend, never network I/O. */
    pthread_mutex_t selection_lock;
} mw_route_t;

typedef struct {
    char host[INET_ADDRSTRLEN];
    uint16_t port;
    /* config owns the absolute path and root_fd until config_destroy. */
    char *root;
    int root_fd;
    size_t workers;
    size_t queue_capacity;
    char name[MW_MAX_SERVER_NAME + 1U];
    mw_route_t routes[MW_MAX_ROUTES];
    size_t route_count;
} mw_config_t;

typedef struct {
    /* Validated settings stay fixed; route selection counters remain mutable. */
    mw_config_t *config;
    atomic_bool stopping;
} mw_runtime_t;

typedef struct mw_pool mw_pool_t;

/* Absolute CLOCK_MONOTONIC milliseconds; never a per-read timeout. */
typedef int64_t mw_deadline_t;

typedef struct {
    int status;
    mw_headers_t headers;
    mw_slice_t body;
} mw_response_t;

typedef struct {
    int status;
    /* Header views borrow the input bytes until forwarding is complete. */
    mw_headers_t headers;
    /* HTTP parser owns this decoded/copied body until upstream_destroy. */
    mw_buffer_t body;
    size_t content_length;
    bool has_content_length;
} mw_upstream_response_t;

/* config: initialize/destroy on the same module, including route mutexes. */
int mw_config_parse(mw_config_t *config, int argc, char *const argv[]);
void mw_config_destroy(mw_config_t *config);

/* server: owns listener, signal pipe and pool; borrows mutable route counters. */
int mw_server_run(mw_config_t *config);

/* net: returned FDs belong to caller; reads may be short, EOF is zero. */
mw_deadline_t mw_net_now_ms(void);
int mw_net_listen(const mw_config_t *config);
int mw_net_connect(const mw_backend_t *backend, mw_deadline_t deadline);
ssize_t mw_net_read(int fd, void *buffer, size_t capacity, mw_deadline_t deadline);
int mw_net_write_all(int fd, const void *buffer, size_t length, mw_deadline_t deadline);

/* pool: submit takes ownership of fd only when true. Workers close owned FDs. */
int mw_pool_create(mw_runtime_t *runtime, mw_pool_t **pool);
bool mw_pool_submit(mw_pool_t *pool, int fd);
void mw_pool_stop(mw_pool_t *pool);
void mw_pool_destroy(mw_pool_t *pool);

/* http: pure parsing does no I/O. Request views borrow immutable input. */
mw_parse_result_t mw_http_parse_request_head(const unsigned char *bytes, size_t length,
                                             mw_request_t *request);
int mw_http_read_request(int fd, mw_buffer_t *input, mw_request_t *request, mw_deadline_t deadline);
void mw_http_buffer_destroy(mw_buffer_t *buffer);
int mw_http_send_response(int fd, const mw_response_t *response, bool head_only, bool keep_alive,
                          mw_deadline_t deadline);
int mw_http_send_error(int fd, int status, bool head_only, mw_deadline_t deadline);
const char *mw_http_status_reason(int status);
/* Server header value. Call once before workers start; read-only afterwards. */
void mw_http_set_server_name(const char *name);
/* Complete upstream bytes only: returns 0, or the HTTP error to send (502). */
int mw_http_parse_upstream_response(const unsigned char *bytes, size_t length, bool head_only,
                                    mw_upstream_response_t *response);
void mw_http_upstream_destroy(mw_upstream_response_t *response);
/* True for hop-by-hop names: the fixed list plus names listed in any Connection header. */
bool mw_http_is_hop_by_hop(const mw_headers_t *headers, mw_slice_t name);

/* static: pure helpers return 0 on success, otherwise 400/403. */
const char *mw_static_mime_type(mw_slice_t path);
int mw_static_decode_path(mw_slice_t path, char *relative_path, size_t capacity);
int mw_static_serve(const mw_config_t *config, int client_fd, const mw_request_t *request,
                    bool keep_alive, mw_deadline_t deadline);

/* app: borrows client_fd; pool closes it after this function returns. */
void mw_app_handle_connection(mw_runtime_t *runtime, int client_fd);

/* proxy: borrowed results stay valid while config/routes stay alive. */
mw_route_t *mw_proxy_match_route(mw_route_t *routes, size_t route_count, mw_slice_t raw_path);
const mw_backend_t *mw_proxy_next_backend(mw_route_t *route);
int mw_proxy_serve(const mw_config_t *config, mw_route_t *route, int client_fd,
                   const mw_request_t *request, bool keep_alive, mw_deadline_t deadline);

#endif
