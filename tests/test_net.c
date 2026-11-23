#include "mini_webserver.h"
#include "test.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool test_net_read_deadline(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(write(pair[0], "abc", 3) == 3);
    char buffer[16];
    CHECK(mw_net_read(pair[1], buffer, sizeof(buffer), mw_net_now_ms() + 1000) == 3);
    CHECK(memcmp(buffer, "abc", 3) == 0);
    mw_deadline_t start = mw_net_now_ms();
    CHECK(mw_net_read(pair[1], buffer, sizeof(buffer), start + 50) == -1);
    CHECK(errno == ETIMEDOUT);
    CHECK(mw_net_now_ms() - start >= 50);
    (void)close(pair[0]);
    CHECK(mw_net_read(pair[1], buffer, sizeof(buffer), mw_net_now_ms() + 1000) == 0);
    (void)close(pair[1]);
    return true;
}

typedef struct {
    int fd;
    size_t count;
    bool valid;
} reader_t;

static void *consume(void *argument) {
    reader_t *reader = argument;
    unsigned char buffer[113];
    reader->valid = true;
    for (;;) {
        ssize_t count = read(reader->fd, buffer, sizeof(buffer));
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            reader->valid = reader->valid && count == 0;
            return NULL;
        }
        for (ssize_t i = 0; i < count; i++) {
            reader->valid = reader->valid && buffer[i] == 0x5a;
        }
        reader->count += (size_t)count;
    }
}

bool test_net_write_all(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int small = 4096;
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    size_t size = 256U * 1024U;
    unsigned char *bytes = malloc(size);
    CHECK(bytes != NULL);
    memset(bytes, 0x5a, size);
    reader_t reader = {.fd = pair[1]};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, consume, &reader) == 0);
    int result = mw_net_write_all(pair[0], bytes, size, mw_net_now_ms() + 3000);
    (void)shutdown(pair[0], SHUT_WR);
    CHECK(pthread_join(thread, NULL) == 0);
    free(bytes);
    (void)close(pair[0]);
    (void)close(pair[1]);
    CHECK(result == 0 && reader.valid && reader.count == size);
    return true;
}

bool test_net_write_deadline(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int small = 4096;
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    unsigned char *bytes = calloc(256U * 1024U, 1);
    CHECK(bytes != NULL);
    int result = mw_net_write_all(pair[0], bytes, 256U * 1024U, mw_net_now_ms() + 50);
    int error = errno;
    free(bytes);
    (void)close(pair[0]);
    (void)close(pair[1]);
    CHECK(result == -1 && error == ETIMEDOUT);
    return true;
}
