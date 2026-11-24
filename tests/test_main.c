#include "test.h"

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const test_case_t tests[] = {
        {"spec contract", test_spec_contract},
        {"config defaults and cleanup", test_config_defaults},
        {"config options", test_config_options},
        {"config invalid arguments", test_config_errors},
        {"config workers and queue", test_config_pool_options},
        {"pool full rejection and cleanup", test_pool_rejects_when_full_and_cleans_up},
        {"pool rejects bad sizes", test_pool_create_rejects_bad_sizes},
        {"read and absolute deadline", test_net_read_deadline},
        {"write all with small send buffer", test_net_write_all},
        {"write absolute deadline", test_net_write_deadline}};
    bool force_failure = false;
    if (argc == 2 && strcmp(argv[1], "--self-test-failure") == 0) {
        force_failure = true;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--self-test-failure]\n", argv[0]);
        return 2;
    }

    size_t failures = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        bool passed = tests[i].run();
        printf("%s: %s\n", passed ? "PASS" : "FAIL", tests[i].name);
        if (!passed) {
            failures++;
        }
    }
    if (force_failure) {
        fputs("FAIL: intentional runner failure\n", stderr);
        failures++;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
