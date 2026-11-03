#pragma once
#include <cstdio>
#include <cstdlib>

// Test assertion that stays active under NDEBUG, unlike assert().
#define VERIFY(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "[FAIL] VERIFY(%s) at %s:%d\n", \
                     #cond, __FILE__, __LINE__); \
        std::exit(1); \
    } \
} while(0)

#define VERIFY_MSG(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "[FAIL] %s  (%s at %s:%d)\n", \
                     msg, #cond, __FILE__, __LINE__); \
        std::exit(1); \
    } \
} while(0)
