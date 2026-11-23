#include "mini_webserver.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    mw_config_t *config = malloc(sizeof(*config));
    if (config == NULL) {
        perror("config allocation");
        return MW_EXIT_RUNTIME_ERROR;
    }
    int result = mw_config_parse(config, argc, argv);
    if (result == MW_EXIT_SUCCESS) {
        result = mw_server_run(config);
    }
    mw_config_destroy(config);
    free(config);
    return result;
}
