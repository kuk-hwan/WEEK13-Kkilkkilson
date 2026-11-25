#include "mini_webserver.h"
#include "test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

bool test_config_defaults(void) {
    mw_config_t config;
    char *args[] = {"mini-webserver"};
    CHECK(mw_config_parse(&config, 1, args) == 0);
    CHECK(strcmp(config.host, "127.0.0.1") == 0);
    CHECK(config.port == 8080);
    CHECK(strcmp(config.name, "server-8080") == 0);
    CHECK(config.root != NULL && config.root[0] == '/');
    CHECK(fcntl(config.root_fd, F_GETFD) >= 0);
    int owned_fd = config.root_fd;
    mw_config_destroy(&config);
    CHECK(config.root == NULL && config.root_fd == -1);
    CHECK(fcntl(owned_fd, F_GETFD) == -1 && errno == EBADF);
    mw_config_destroy(&config);
    return true;
}

bool test_config_options(void) {
    mw_config_t config;
    char *args[] = {"mini-webserver", "--host", "0.0.0.0", "--port", "65535", "--root", "public"};
    CHECK(mw_config_parse(&config, 7, args) == 0);
    CHECK(strcmp(config.host, "0.0.0.0") == 0);
    CHECK(config.port == 65535);
    CHECK(strcmp(config.name, "server-65535") == 0);
    mw_config_destroy(&config);
    return true;
}

bool test_config_name(void) {
    mw_config_t config;
    char *args[] = {"mini-webserver", "--name", "backend-a"};
    CHECK(mw_config_parse(&config, 3, args) == 0);
    CHECK(strcmp(config.name, "backend-a") == 0);
    mw_config_destroy(&config);
    char long_name[65];
    memset(long_name, 'n', 64);
    long_name[64] = '\0';
    char *bad[] = {"mini-webserver", "--name", long_name};
    CHECK(mw_config_parse(&config, 3, bad) == 2);
    long_name[63] = '\0';
    CHECK(mw_config_parse(&config, 3, bad) == 0);
    mw_config_destroy(&config);
    return true;
}

bool test_config_errors(void) {
    mw_config_t config;
    const char *ports[] = {"", "0", "65536", "-1", "+80", " 80", "80x", "9999999999999999999999"};
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        char *args[] = {"mini-webserver", "--port", (char *)ports[i]};
        CHECK(mw_config_parse(&config, 3, args) == 2);
        CHECK(config.root == NULL && config.root_fd == -1);
        mw_config_destroy(&config);
    }
    char *unknown[] = {"mini-webserver", "--unknown", "value"};
    CHECK(mw_config_parse(&config, 3, unknown) == 2);
    char *missing[] = {"mini-webserver", "--root"};
    CHECK(mw_config_parse(&config, 2, missing) == 2);
    char *host[] = {"mini-webserver", "--host", "not-an-ip"};
    CHECK(mw_config_parse(&config, 3, host) == 2);
    char *root_file[] = {"mini-webserver", "--root", "public/index.html"};
    CHECK(mw_config_parse(&config, 3, root_file) == 2);
    mw_config_destroy(&config);
    return true;
}

bool test_config_pool_options(void) {
    mw_config_t config;
    char *defaults[] = {"mini-webserver"};
    CHECK(mw_config_parse(&config, 1, defaults) == 0);
    CHECK(config.workers == 8 && config.queue_capacity == 64);
    mw_config_destroy(&config);

    char *lower[] = {"mini-webserver", "--workers", "1", "--queue", "1"};
    CHECK(mw_config_parse(&config, 5, lower) == 0);
    CHECK(config.workers == 1 && config.queue_capacity == 1);
    mw_config_destroy(&config);

    char *upper[] = {"mini-webserver", "--workers", "128", "--queue", "4096"};
    CHECK(mw_config_parse(&config, 5, upper) == 0);
    CHECK(config.workers == 128 && config.queue_capacity == 4096);
    mw_config_destroy(&config);

    const char *bad[] = {"", "0", "-1", "+1", " 1", "1x", "x", "99999999999999999999"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char *workers[] = {"mini-webserver", "--workers", (char *)bad[i]};
        CHECK(mw_config_parse(&config, 3, workers) == 2);
        CHECK(config.root == NULL && config.root_fd == -1);
        mw_config_destroy(&config);
        char *queue[] = {"mini-webserver", "--queue", (char *)bad[i]};
        CHECK(mw_config_parse(&config, 3, queue) == 2);
        mw_config_destroy(&config);
    }
    char *too_many_workers[] = {"mini-webserver", "--workers", "129"};
    CHECK(mw_config_parse(&config, 3, too_many_workers) == 2);
    mw_config_destroy(&config);
    char *too_big_queue[] = {"mini-webserver", "--queue", "4097"};
    CHECK(mw_config_parse(&config, 3, too_big_queue) == 2);
    mw_config_destroy(&config);
    return true;
}

bool test_config_route(void) {
    mw_config_t config;
    char *args[] = {"mini-webserver", "--route", "/api/=127.0.0.1:8001"};
    CHECK(mw_config_parse(&config, 3, args) == 0);
    CHECK(config.route_count == 1 && strcmp(config.routes[0].prefix, "/api/") == 0);
    const mw_backend_t *backend = &config.routes[0].backends[0];
    CHECK(config.routes[0].backend_count == 1 && strcmp(backend->host, "127.0.0.1") == 0);
    CHECK(backend->port == 8001 && backend->address.ss_family == AF_INET);
    CHECK(backend->address_length == sizeof(struct sockaddr_in));
    CHECK(ntohs(((const struct sockaddr_in *)&backend->address)->sin_port) == 8001);
    mw_config_destroy(&config);
    CHECK(config.route_count == 0);

    const char *bad[] = {"api/=127.0.0.1:8001",
                         "/api/",
                         "/api/=127.0.0.1",
                         "/api/=127.0.0.1:",
                         "/api/=127.0.0.1:0",
                         "/api/=:8001",
                         "=127.0.0.1:8001",
                         "/a:1=127.0.0.1",
                         "/api/=127.0.0.1:8001,",
                         "/api/=,127.0.0.1:8001",
                         "/api/=127.0.0.1:8001,,127.0.0.1:8002",
                         "/api/=127.0.0.1:8001,127.0.0.1:0",
                         "/api/=127.0.0.1:8001,127.0.0.1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char *wrong[] = {"mini-webserver", "--route", (char *)bad[i]};
        CHECK(mw_config_parse(&config, 3, wrong) == 2);
        CHECK(config.route_count == 0);
    }
    return true;
}

bool test_config_route_lists(void) {
    mw_config_t config;
    char *args[] = {"mini-webserver", "--route", "/api/=127.0.0.1:8001,127.0.0.1:8002", "--route",
                    "/api/admin/=localhost:8003"};
    CHECK(mw_config_parse(&config, 5, args) == 0);
    CHECK(config.route_count == 2 && config.routes[0].backend_count == 2);
    CHECK(config.routes[0].backends[0].port == 8001 && config.routes[0].backends[1].port == 8002);
    CHECK(strcmp(config.routes[0].backends[1].host, "127.0.0.1") == 0);
    CHECK(ntohs(((const struct sockaddr_in *)&config.routes[0].backends[1].address)->sin_port) ==
          8002);
    CHECK(config.routes[1].backend_count == 1 && config.routes[1].backends[0].port == 8003);
    mw_config_destroy(&config);

    /* 8 backends per route and 16 routes are allowed; one more of either is an argument error. */
    char eight[256] = "/a/=";
    char nine[512];
    for (int i = 0; i < 8; i++) {
        char entry[32];
        (void)snprintf(entry, sizeof(entry), "%s127.0.0.1:%d", i == 0 ? "" : ",", 9001 + i);
        strcat(eight, entry);
    }
    (void)snprintf(nine, sizeof(nine), "%s,127.0.0.1:9009", eight);
    char *ok_args[] = {"mini-webserver", "--route", eight};
    CHECK(mw_config_parse(&config, 3, ok_args) == 0 && config.routes[0].backend_count == 8);
    mw_config_destroy(&config);
    char *nine_args[] = {"mini-webserver", "--route", nine};
    CHECK(mw_config_parse(&config, 3, nine_args) == 2);
    CHECK(config.route_count == 0);

    char texts[17][32];
    char *many[2 + 2 * 17] = {"mini-webserver"};
    int argc = 1;
    for (int i = 0; i < 17; i++) {
        (void)snprintf(texts[i], sizeof(texts[i]), "/r%d/=127.0.0.1:8001", i);
        many[argc++] = "--route";
        many[argc++] = texts[i];
    }
    CHECK(mw_config_parse(&config, 1 + 2 * 16, many) == 0 && config.route_count == 16);
    mw_config_destroy(&config);
    CHECK(mw_config_parse(&config, argc, many) == 2);
    CHECK(config.route_count == 0);
    return true;
}

static bool allowed(const mw_config_t *config, const char *address) {
    struct in_addr client;
    return inet_pton(AF_INET, address, &client) == 1 &&
           mw_config_forward_allowed(config->forward_allow, config->forward_allow_count, client);
}

bool test_config_forward_allow(void) {
    mw_config_t config;
    char *off[] = {"mini-webserver"};
    CHECK(mw_config_parse(&config, 1, off) == 0 && config.forward_allow_count == 0);
    CHECK(!allowed(&config, "127.0.0.1")); /* empty list: nobody */
    mw_config_destroy(&config);

    char *several[] = {"mini-webserver", "--forward-allow", "10.1.2.3/8",   "--forward-allow",
                       "192.168.1.7/32", "--forward-allow", "172.16.0.0/12"};
    CHECK(mw_config_parse(&config, 7, several) == 0 && config.forward_allow_count == 3);
    CHECK(allowed(&config, "10.0.0.0") &&
          allowed(&config, "10.255.255.255")); /* host bits dropped */
    CHECK(!allowed(&config, "9.255.255.255") && !allowed(&config, "11.0.0.0"));
    CHECK(allowed(&config, "192.168.1.7"));
    CHECK(!allowed(&config, "192.168.1.6") && !allowed(&config, "192.168.1.8"));
    CHECK(allowed(&config, "172.16.0.0") && allowed(&config, "172.31.255.255"));
    CHECK(!allowed(&config, "172.15.255.255") && !allowed(&config, "172.32.0.0"));
    CHECK(!allowed(&config, "127.0.0.1"));
    mw_config_destroy(&config);

    char *everyone[] = {"mini-webserver", "--forward-allow", "1.2.3.4/0"};
    CHECK(mw_config_parse(&config, 3, everyone) == 0);
    CHECK(allowed(&config, "0.0.0.0") && allowed(&config, "255.255.255.255"));
    mw_config_destroy(&config);

    const char *bad[] = {"127.0.0.1", "127.0.0.1/",    "127.0.0.1/33", "127.0.0.1/-1",
                         "127.0.0/8", "127.0.0.256/8", "/8",           "127.0.0.1/8x",
                         "::1/128",   "h.example/8",   "127.0.0.1/8/8"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char *args[] = {"mini-webserver", "--forward-allow", (char *)bad[i]};
        CHECK(mw_config_parse(&config, 3, args) == 2);
    }
    char *many[35] = {"mini-webserver"};
    for (int i = 0; i < 17; i++) {
        many[1 + 2 * i] = "--forward-allow";
        many[2 + 2 * i] = "127.0.0.1/32";
    }
    CHECK(mw_config_parse(&config, 33, many) == 0 && config.forward_allow_count == 16);
    mw_config_destroy(&config);
    CHECK(mw_config_parse(&config, 35, many) == 2);
    return true;
}
