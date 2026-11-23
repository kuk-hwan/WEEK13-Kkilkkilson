#include "test.h"

#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const test_case_t tests[] = {{"spec contract", test_spec_contract}};
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
