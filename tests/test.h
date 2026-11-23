#ifndef MINI_WEBSERVER_TEST_H
#define MINI_WEBSERVER_TEST_H

#include <stdbool.h>
#include <stdio.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

typedef bool (*test_function_t)(void);

typedef struct {
    const char *name;
    test_function_t run;
} test_case_t;

bool test_spec_contract(void);

#endif
