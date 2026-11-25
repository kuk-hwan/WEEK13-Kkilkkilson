#define _XOPEN_SOURCE 700
#include "mini_webserver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int invalid_config(mw_config_t *config, const char *message) {
    fprintf(stderr,
            "%s\nusage: mini-webserver [--host IPv4] [--port 1..65535] [--root directory]"
            " [--workers 1..128] [--queue 1..4096] [--name name]"
            " [--route /prefix=host:port[,host:port...]]... [--forward-allow IPv4/length]...\n",
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

/* "host:port" in text[0..length): resolved once, the first getaddrinfo result is fixed for the
 * whole run. */
static bool parse_backend(mw_backend_t *backend, const char *text, size_t length) {
    const char *colon = NULL;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == ':') {
            colon = text + i;
        }
    }
    if (colon == NULL) {
        return false;
    }
    size_t host_length = (size_t)(colon - text);
    size_t port_length = length - host_length - 1U;
    char port_text[8];
    if (host_length == 0 || host_length > MW_MAX_BACKEND_HOST || port_length >= sizeof(port_text)) {
        return false;
    }
    memcpy(port_text, colon + 1, port_length);
    port_text[port_length] = '\0';
    if (!parse_port(port_text, &backend->port)) {
        return false;
    }
    memcpy(backend->host, text, host_length);
    backend->host[host_length] = '\0';
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV};
    struct addrinfo *found = NULL;
    int error = getaddrinfo(backend->host, port_text, &hints, &found);
    if (error != 0) {
        fprintf(stderr, "route backend %s: %s\n", backend->host, gai_strerror(error));
        return false;
    }
    memcpy(&backend->address, found->ai_addr, found->ai_addrlen);
    backend->address_length = found->ai_addrlen;
    freeaddrinfo(found);
    return true;
}

/* "/prefix=host:port[,host:port...]": up to MW_MAX_BACKENDS backends, tried in this order by the
 * round robin. The route is only counted once everything in it is valid. */
static bool parse_route(mw_config_t *config, const char *text) {
    const char *equals = strchr(text, '=');
    if (config->route_count == MW_MAX_ROUTES || text[0] != '/' || equals == NULL) {
        return false;
    }
    size_t prefix_length = (size_t)(equals - text);
    mw_route_t *route = &config->routes[config->route_count];
    if (prefix_length > MW_MAX_REQUEST_LINE) {
        return false;
    }
    size_t count = 0;
    for (const char *entry = equals + 1;;) {
        const char *comma = strchr(entry, ',');
        size_t length = comma != NULL ? (size_t)(comma - entry) : strlen(entry);
        if (count == MW_MAX_BACKENDS || !parse_backend(&route->backends[count], entry, length)) {
            return false;
        }
        count++;
        if (comma == NULL) {
            break;
        }
        entry = comma + 1;
    }
    memcpy(route->prefix, text, prefix_length);
    route->prefix[prefix_length] = '\0';
    if (pthread_mutex_init(&route->selection_lock, NULL) != 0) {
        return false;
    }
    route->backend_count = count;
    route->next_backend = 0;
    config->route_count++;
    return true;
}

/* "a.b.c.d/0..32". Host bits past the length are dropped, so 10.1.2.3/8 means 10.0.0.0/8. */
static bool parse_forward_allow(mw_config_t *config, const char *text) {
    const char *slash = strchr(text, '/');
    char address_text[INET_ADDRSTRLEN];
    struct in_addr address;
    size_t length = 0;
    if (config->forward_allow_count == MW_MAX_FORWARD_ALLOW || slash == NULL ||
        (size_t)(slash - text) >= sizeof(address_text)) {
        return false;
    }
    memcpy(address_text, text, (size_t)(slash - text));
    address_text[slash - text] = '\0';
    if (inet_pton(AF_INET, address_text, &address) != 1 || !parse_size(slash + 1, 0, 32, &length)) {
        return false;
    }
    uint32_t mask = length == 0 ? 0U : UINT32_MAX << (32U - length);
    config->forward_allow[config->forward_allow_count++] =
        (mw_cidr_t){ntohl(address.s_addr) & mask, mask};
    return true;
}

bool mw_config_forward_allowed(const mw_cidr_t *allow, size_t count, struct in_addr client) {
    uint32_t address = ntohl(client.s_addr);
    for (size_t i = 0; i < count; i++) {
        if ((address & allow[i].mask) == allow[i].network) {
            return true;
        }
    }
    return false;
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
        } else if (strcmp(argv[i], "--route") == 0) {
            if (!parse_route(config, value)) {
                return invalid_config(
                    config,
                    "invalid route (/prefix=host:port[,host:port...], max 16 routes, 8 backends)");
            }
        } else if (strcmp(argv[i], "--forward-allow") == 0) {
            if (!parse_forward_allow(config, value)) {
                return invalid_config(config, "invalid forward allow (IPv4/0..32, max 16)");
            }
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
