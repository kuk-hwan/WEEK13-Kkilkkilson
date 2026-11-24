#define _XOPEN_SOURCE 700
#include "mini_webserver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int invalid_config(mw_config_t *config, const char *message) {
    fprintf(stderr,
            "%s\nusage: mini-webserver [--host IPv4] [--port 1..65535] [--root directory]"
            " [--workers 1..128] [--queue 1..4096] [--name name]\n",
            message);
    mw_config_destroy(config);
    return MW_EXIT_ARGUMENT_ERROR;
}

static bool parse_port(const char *text, uint16_t *port) {
    unsigned int value = 0;
    if (*text == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || value > (65535U - (*p - '0')) / 10U) {
            return false;
        }
        value = value * 10U + (*p - '0');
    }
    if (value == 0) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static bool parse_size(const char *text, size_t minimum, size_t maximum, size_t *result) {
    size_t value = 0;
    if (*text == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || value > (maximum - (size_t)(*p - '0')) / 10U) {
            return false;
        }
        value = value * 10U + (size_t)(*p - '0');
    }
    if (value < minimum || value > maximum) {
        return false;
    }
    *result = value;
    return true;
}

int mw_config_parse(mw_config_t *config, int argc, char *const argv[]) {
    memset(config, 0, sizeof(*config));
    config->root_fd = -1;
    config->port = MW_DEFAULT_PORT;
    config->workers = MW_DEFAULT_WORKERS;
    config->queue_capacity = MW_DEFAULT_QUEUE_CAPACITY;
    memcpy(config->host, MW_DEFAULT_HOST, sizeof(MW_DEFAULT_HOST));
    const char *root = MW_DEFAULT_ROOT;
    const char *name = NULL;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            return invalid_config(config, "missing option value");
        }
        const char *value = argv[i + 1];
        if (strcmp(argv[i], "--port") == 0) {
            if (!parse_port(value, &config->port)) {
                return invalid_config(config, "invalid port");
            }
        } else if (strcmp(argv[i], "--host") == 0) {
            struct in_addr address;
            if (strlen(value) >= sizeof(config->host) || inet_pton(AF_INET, value, &address) != 1) {
                return invalid_config(config, "invalid IPv4 host");
            }
            memcpy(config->host, value, strlen(value) + 1);
        } else if (strcmp(argv[i], "--workers") == 0) {
            if (!parse_size(value, MW_MIN_WORKERS, MW_MAX_WORKERS, &config->workers)) {
                return invalid_config(config, "invalid workers");
            }
        } else if (strcmp(argv[i], "--queue") == 0) {
            if (!parse_size(value, MW_MIN_QUEUE_CAPACITY, MW_MAX_QUEUE_CAPACITY,
                            &config->queue_capacity)) {
                return invalid_config(config, "invalid queue");
            }
        } else if (strcmp(argv[i], "--name") == 0) {
            if (*value == '\0' || strlen(value) > MW_MAX_SERVER_NAME) {
                return invalid_config(config, "invalid name (1..63 bytes)");
            }
            name = value;
        } else if (strcmp(argv[i], "--root") == 0) {
            root = value;
        } else {
            return invalid_config(config, "unknown option");
        }
    }
    config->root = realpath(root, NULL);
    if (config->root == NULL) {
        perror("root");
        return invalid_config(config, "root must be an existing directory");
    }
    config->root_fd = open(config->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (config->root_fd == -1) {
        perror("root");
        return invalid_config(config, "cannot open root directory");
    }
    if (name != NULL) {
        memcpy(config->name, name, strlen(name) + 1);
    } else {
        (void)snprintf(config->name, sizeof(config->name), "server-%u", (unsigned int)config->port);
    }
    return MW_EXIT_SUCCESS;
}

void mw_config_destroy(mw_config_t *config) {
    if (config->root_fd >= 0) {
        (void)close(config->root_fd);
        config->root_fd = -1;
    }
    free(config->root);
    config->root = NULL;
    for (size_t i = 0; i < config->route_count; i++) {
        (void)pthread_mutex_destroy(&config->routes[i].selection_lock);
    }
    config->route_count = 0;
}
