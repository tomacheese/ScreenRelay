#include <cstdio>
#include "common/types.hpp"
#include "test_utils.hpp"

/**
 * @brief BackoffState のユニットテストを実行する
 * @return 成功時 0 (失敗時は VERIFY マクロにより exit(1))
 */
int run_backoff_tests() {
    printf("=== Backoff Tests ===\n");

    {
        // デフォルト初期状態の確認
        BackoffState b;
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 1000);
        printf("[PASS] BackoffState のデフォルト初期状態が正しい\n");
    }

    {
        // reset(500) で delay_ms=500 になること
        BackoffState b;
        b.reset(500);
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 500);
        printf("[PASS] reset(500) で delay_ms=500 になる\n");
    }

    {
        // next_delay() で指数増加すること (1000 → 2000 → 4000)
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
        printf("[PASS] 指数バックオフが正しく 2 倍になる\n");
    }

    {
        // max_ms でキャップされること
        BackoffState b;
        b.reset(16000);
        int d = b.next_delay(30000, 2.0f);
        VERIFY(d == 16000);
        VERIFY(b.delay_ms == 30000);

        int d2 = b.next_delay(30000, 2.0f);
        VERIFY(d2 == 30000);
        VERIFY(b.delay_ms == 30000); // 上限でキャップされていること
        printf("[PASS] max_ms=30000 でキャップされる\n");
    }

    {
        // reset() でリセットされること
        BackoffState b;
        b.reset(1000);
        for (int i = 0; i < 6; ++i) b.next_delay(30000, 2.0f);
        b.reset(1000);
        VERIFY(b.attempt  == 0);
        VERIFY(b.delay_ms == 1000);
        printf("[PASS] 上限到達後に reset() で初期値に戻る\n");
    }

    return 0;
}
