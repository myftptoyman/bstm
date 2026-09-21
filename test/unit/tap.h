#ifndef TAP_H
#define TAP_H
#include <stdio.h>
#include <stdlib.h>
static int tap_fail = 0, tap_n = 0;
#define CHECK(c, ...) do { tap_n++; if (!(c)) { tap_fail++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n    ", __FILE__, __LINE__, #c); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define TAP_DONE(name) do { \
    printf("%-22s %3d checks, %d fail\n", name, tap_n, tap_fail); \
    return tap_fail ? 1 : 0; } while (0)
#endif
