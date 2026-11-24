#include "mini_webserver.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static bool fd_is_closed(int fd) {
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

bool test_pool_rejects_when_full_and_cleans_up(void) {
    mw_config_t config;
    char *args[] = {"mini-webserver"};
    CHECK(mw_config_parse(&config, 1, args) == 0);
    config.workers = 1;
    config.queue_capacity = 1;
    mw_runtime_t runtime = {.config = &config};
    atomic_init(&runtime.stopping, false);
    mw_pool_t *pool = NULL;
    CHECK(mw_pool_create(&runtime, &pool) == 0 && pool != NULL);

    /* One worker plus a queue of one can own at most two silent connections. */
    int server_side[4];
    int client_side[4];
    bool accepted[4];
    int accepted_count = 0;
    int rejected_count = 0;
    for (int i = 0; i < 4; i++) {
        int pair[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        server_side[i] = pair[0];
        client_side[i] = pair[1];
        accepted[i] = mw_pool_submit(pool, server_side[i]);
        if (accepted[i]) {
            accepted_count++;
        } else {
            rejected_count++;
        }
    }
    CHECK(accepted_count >= 1 && accepted_count <= 2);
    CHECK(rejected_count >= 2);
    /* A rejected FD stays with the caller. */
    for (int i = 0; i < 4; i++) {
        if (!accepted[i]) {
            CHECK(!fd_is_closed(server_side[i]));
            CHECK(close(server_side[i]) == 0);
        }
    }
    /* EOF lets the busy worker finish at once, so stop does not wait for a deadline. */
    for (int i = 0; i < 4; i++) {
        CHECK(close(client_side[i]) == 0);
    }
    mw_pool_stop(pool);
    CHECK(!mw_pool_submit(pool, -1));
    /* The pool closed every FD it owned: handled by a worker or dropped from the queue. */
    for (int i = 0; i < 4; i++) {
        if (accepted[i]) {
            CHECK(fd_is_closed(server_side[i]));
        }
    }
    mw_pool_destroy(pool);
    mw_config_destroy(&config);
    return true;
}

bool test_pool_create_rejects_bad_sizes(void) {
    mw_config_t config;
    char *args[] = {"mini-webserver"};
    CHECK(mw_config_parse(&config, 1, args) == 0);
    mw_runtime_t runtime = {.config = &config};
    atomic_init(&runtime.stopping, false);
    mw_pool_t *pool = NULL;
    config.workers = 0;
    CHECK(mw_pool_create(&runtime, &pool) == -1 && pool == NULL);
    config.workers = 1;
    config.queue_capacity = 0;
    CHECK(mw_pool_create(&runtime, &pool) == -1 && pool == NULL);
    mw_config_destroy(&config);
    return true;
}
