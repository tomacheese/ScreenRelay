#include <cstdio>
#include "common/types.hpp"
#include "test_utils.hpp"

/**
 * @brief Tests BackoffState.
 * @return 0 on success (a failing VERIFY exits with status 1)
 */
int run_backoff_tests() {
    printf("=== Backoff Tests ===\n");

    {
        // Default initial state
        BackoffState b;
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 1000);
        printf("[PASS] BackoffState default initial state is correct\n");
    }

    {
        // reset(500) sets delay_ms to 500
        BackoffState b;
        b.reset(500);
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 500);
        printf("[PASS] reset(500) sets delay_ms=500\n");
    }

    {
        // next_delay() grows exponentially (1000 -> 2000 -> 4000)
        BackoffState b;
        b.reset(1000);
        int d1 = b.next_delay(30000, 2.0f);
        VERIFY(d1 == 1000);
        VERIFY(b.attempt  == 1);
        VERIFY(b.delay_ms == 2000);

        int d2 = b.next_delay(30000, 2.0f);
        VERIFY(d2 == 2000);
        VERIFY(b.attempt  == 2);
        VERIFY(b.delay_ms == 4000);
        printf("[PASS] exponential back-off doubles correctly\n");
    }

    {
        // Delay is capped at max_ms
        BackoffState b;
        b.reset(16000);
        int d = b.next_delay(30000, 2.0f);
        VERIFY(d == 16000);
        VERIFY(b.delay_ms == 30000);

        int d2 = b.next_delay(30000, 2.0f);
        VERIFY(d2 == 30000);
        VERIFY(b.delay_ms == 30000); // stays at the cap
        printf("[PASS] delay is capped at max_ms=30000\n");
    }

    {
        // reset() restores the initial state
        BackoffState b;
        b.reset(1000);
        for (int i = 0; i < 6; ++i) b.next_delay(30000, 2.0f);
        b.reset(1000);
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 1000);
        printf("[PASS] reset() restores the initial value after reaching the cap\n");
    }

    return 0;
}
