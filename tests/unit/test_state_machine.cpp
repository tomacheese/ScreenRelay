#include <cstdio>
#include "app/state_machine.hpp"
#include "test_utils.hpp"

/**
 * @brief StateMachine のユニットテストを実行する
 *
 * 有効遷移・無効遷移・終端ステート・コールバック発火を検証する。
 * @return 成功時 0 (失敗時は VERIFY マクロにより exit(1))
 */
int run_state_machine_tests() {
    printf("=== State Machine Tests ===\n");

    {
        // INIT → CAPTURING (有効遷移)
        StateMachine sm;
        VERIFY(sm.current_state() == PipelineState::INIT);
        bool ok = sm.transition_to(PipelineState::CAPTURING);
        VERIFY_MSG(ok, "INIT → CAPTURING should succeed");
        VERIFY(sm.current_state() == PipelineState::CAPTURING);
        printf("[PASS] INIT → CAPTURING\n");
    }

    {
        // INIT → FATAL (有効遷移)
        StateMachine sm;
        bool ok = sm.transition_to(PipelineState::FATAL);
        VERIFY_MSG(ok, "INIT → FATAL should succeed");
        VERIFY(sm.current_state() == PipelineState::FATAL);
        printf("[PASS] INIT → FATAL\n");
    }

    {
        // CAPTURING → CONNECTING (有効遷移)
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        bool ok = sm.transition_to(PipelineState::CONNECTING);
        VERIFY_MSG(ok, "CAPTURING → CONNECTING should succeed");
        printf("[PASS] CAPTURING → CONNECTING\n");
    }

    {
        // CONNECTING → STREAMING (有効遷移)
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        bool ok = sm.transition_to(PipelineState::STREAMING);
        VERIFY_MSG(ok, "CONNECTING → STREAMING should succeed");
        printf("[PASS] CONNECTING → STREAMING\n");
    }

    {
        // STREAMING → RECONNECTING (有効遷移)
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        sm.transition_to(PipelineState::STREAMING);
        bool ok = sm.transition_to(PipelineState::RECONNECTING);
        VERIFY_MSG(ok, "STREAMING → RECONNECTING should succeed");
        printf("[PASS] STREAMING → RECONNECTING\n");
    }

    {
        // STREAMING → RECONFIGURING (有効遷移)
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        sm.transition_to(PipelineState::STREAMING);
        bool ok = sm.transition_to(PipelineState::RECONFIGURING);
        VERIFY_MSG(ok, "STREAMING → RECONFIGURING should succeed");
        printf("[PASS] STREAMING → RECONFIGURING\n");
    }

    {
        // RECONNECTING → STREAMING (有効遷移)
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        sm.transition_to(PipelineState::STREAMING);
        sm.transition_to(PipelineState::RECONNECTING);
        bool ok = sm.transition_to(PipelineState::STREAMING);
        VERIFY_MSG(ok, "RECONNECTING → STREAMING should succeed");
        printf("[PASS] RECONNECTING → STREAMING\n");
    }

    {
        // RECONFIGURING → CAPTURING (有効遷移)
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        sm.transition_to(PipelineState::STREAMING);
        sm.transition_to(PipelineState::RECONFIGURING);
        bool ok = sm.transition_to(PipelineState::CAPTURING);
        VERIFY_MSG(ok, "RECONFIGURING → CAPTURING should succeed");
        printf("[PASS] RECONFIGURING → CAPTURING\n");
    }

    {
        // 無効遷移: INIT → STREAMING は拒否される
        StateMachine sm;
        bool ok = sm.transition_to(PipelineState::STREAMING);
        VERIFY_MSG(!ok, "INIT → STREAMING must be rejected");
        VERIFY(sm.current_state() == PipelineState::INIT);
        printf("[PASS] 無効遷移 INIT → STREAMING が拒否される\n");
    }

    {
        // 無効遷移: INIT → RECONNECTING は拒否される
        StateMachine sm;
        bool ok = sm.transition_to(PipelineState::RECONNECTING);
        VERIFY_MSG(!ok, "INIT → RECONNECTING must be rejected");
        VERIFY(sm.current_state() == PipelineState::INIT);
        printf("[PASS] 無効遷移 INIT → RECONNECTING が拒否される\n");
    }

    {
        // STOPPING に到達したらそれ以上遷移できないこと
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::STOPPING);
        VERIFY(sm.current_state() == PipelineState::STOPPING);
        bool ok = sm.transition_to(PipelineState::CAPTURING);
        VERIFY_MSG(!ok, "STOPPING → CAPTURING must be rejected");
        VERIFY(sm.current_state() == PipelineState::STOPPING);
        printf("[PASS] STOPPING は終端ステートでそれ以上遷移できない\n");
    }

    {
        // FATAL に到達したらそれ以上遷移できないこと
        StateMachine sm;
        sm.transition_to(PipelineState::FATAL);
        VERIFY(sm.current_state() == PipelineState::FATAL);
        bool ok = sm.transition_to(PipelineState::CAPTURING);
        VERIFY_MSG(!ok, "FATAL → CAPTURING must be rejected");
        VERIFY(sm.current_state() == PipelineState::FATAL);
        printf("[PASS] FATAL は終端ステートでそれ以上遷移できない\n");
    }

    {
        // 遷移コールバックが正しく発火すること
        StateMachine sm;
        PipelineState cb_from = PipelineState::INIT;
        PipelineState cb_to   = PipelineState::INIT;
        sm.on_transition([&](PipelineState f, PipelineState t) {
            cb_from = f;
            cb_to   = t;
        });
        sm.transition_to(PipelineState::CAPTURING);
        VERIFY(cb_from == PipelineState::INIT);
        VERIFY(cb_to   == PipelineState::CAPTURING);
        printf("[PASS] 遷移コールバックが正しく発火する\n");
    }

    return 0;
}
