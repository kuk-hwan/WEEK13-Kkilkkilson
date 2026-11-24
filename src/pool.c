#include "mini_webserver.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

struct mw_pool {
    mw_runtime_t *runtime;
    pthread_t *threads;
    size_t thread_count;
    /* lock and not_empty guard only the fields below; never held during network I/O. */
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    int *queue;
    size_t capacity;
    size_t head;
    size_t count;
    bool stopping;
    bool joined;
};

static void *worker_main(void *argument) {
    mw_pool_t *pool = argument;
    for (;;) {
        (void)pthread_mutex_lock(&pool->lock);
        while (pool->count == 0 && !pool->stopping) {
            (void)pthread_cond_wait(&pool->not_empty, &pool->lock);
        }
        if (pool->stopping) {
            (void)pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        int fd = pool->queue[pool->head];
        pool->head = (pool->head + 1U) % pool->capacity;
        pool->count--;
        (void)pthread_mutex_unlock(&pool->lock);

        mw_app_handle_connection(pool->runtime, fd);
        (void)close(fd);
    }
}

int mw_pool_create(mw_runtime_t *runtime, mw_pool_t **out) {
    *out = NULL;
    size_t workers = runtime->config->workers;
    size_t capacity = runtime->config->queue_capacity;
    if (workers < MW_MIN_WORKERS || workers > MW_MAX_WORKERS || capacity < MW_MIN_QUEUE_CAPACITY ||
        capacity > MW_MAX_QUEUE_CAPACITY) {
        errno = EINVAL;
        return -1;
    }
    mw_pool_t *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return -1;
    }
    pool->runtime = runtime;
    pool->capacity = capacity;
    pool->queue = calloc(capacity, sizeof(*pool->queue));
    pool->threads = calloc(workers, sizeof(*pool->threads));
    if (pool->queue == NULL || pool->threads == NULL) {
        free(pool->queue);
        free(pool->threads);
        free(pool);
        return -1;
    }
    (void)pthread_mutex_init(&pool->lock, NULL);
    (void)pthread_cond_init(&pool->not_empty, NULL);

    /* Workers inherit this mask, so only the main thread receives SIGINT and SIGTERM. */
    sigset_t blocked;
    sigset_t previous;
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGINT);
    (void)sigaddset(&blocked, SIGTERM);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    int failure = 0;
    for (size_t i = 0; i < workers; i++) {
        failure = pthread_create(&pool->threads[i], NULL, worker_main, pool);
        if (failure != 0) {
            break;
        }
        pool->thread_count++;
    }
    (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (failure != 0) {
        mw_pool_destroy(pool);
        errno = failure;
        return -1;
    }
    *out = pool;
    return 0;
}

bool mw_pool_submit(mw_pool_t *pool, int fd) {
    (void)pthread_mutex_lock(&pool->lock);
    if (pool->stopping || pool->count == pool->capacity) {
        (void)pthread_mutex_unlock(&pool->lock);
        return false;
    }
    pool->queue[(pool->head + pool->count) % pool->capacity] = fd;
    pool->count++;
    (void)pthread_mutex_unlock(&pool->lock);
    (void)pthread_cond_signal(&pool->not_empty);
    return true;
}

void mw_pool_stop(mw_pool_t *pool) {
    (void)pthread_mutex_lock(&pool->lock);
    pool->stopping = true;
    while (pool->count > 0) {
        (void)close(pool->queue[pool->head]);
        pool->head = (pool->head + 1U) % pool->capacity;
        pool->count--;
    }
    bool join = !pool->joined;
    pool->joined = true;
    (void)pthread_mutex_unlock(&pool->lock);
    (void)pthread_cond_broadcast(&pool->not_empty);
    if (join) {
        for (size_t i = 0; i < pool->thread_count; i++) {
            (void)pthread_join(pool->threads[i], NULL);
        }
    }
}

void mw_pool_destroy(mw_pool_t *pool) {
    if (pool == NULL) {
        return;
    }
    mw_pool_stop(pool);
    (void)pthread_cond_destroy(&pool->not_empty);
    (void)pthread_mutex_destroy(&pool->lock);
    free(pool->queue);
    free(pool->threads);
    free(pool);
}
