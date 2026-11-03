#include <cstdio>
#include "app/state_machine.hpp"
#include "test_utils.hpp"

/**
 * @brief Tests StateMachine: valid and invalid transitions, terminal states and the
 *        transition callback.
 * @return 0 on success (a failing VERIFY exits with status 1)
 */
int run_state_machine_tests() {
    printf("=== State Machine Tests ===\n");

    {
        // INIT → CAPTURING
        StateMachine sm;
        VERIFY(sm.current_state() == PipelineState::INIT);
        bool ok = sm.transition_to(PipelineState::CAPTURING);
        VERIFY_MSG(ok, "INIT → CAPTURING should succeed");
        VERIFY(sm.current_state() == PipelineState::CAPTURING);
        printf("[PASS] INIT → CAPTURING\n");
    }

    {
        // INIT → FATAL
        StateMachine sm;
        bool ok = sm.transition_to(PipelineState::FATAL);
        VERIFY_MSG(ok, "INIT → FATAL should succeed");
        VERIFY(sm.current_state() == PipelineState::FATAL);
        printf("[PASS] INIT → FATAL\n");
    }

    {
        // CAPTURING → CONNECTING
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        bool ok = sm.transition_to(PipelineState::CONNECTING);
        VERIFY_MSG(ok, "CAPTURING → CONNECTING should succeed");
        printf("[PASS] CAPTURING → CONNECTING\n");
    }

    {
        // CONNECTING → STREAMING
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        bool ok = sm.transition_to(PipelineState::STREAMING);
        VERIFY_MSG(ok, "CONNECTING → STREAMING should succeed");
        printf("[PASS] CONNECTING → STREAMING\n");
    }

    {
        // STREAMING → RECONNECTING
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        sm.transition_to(PipelineState::STREAMING);
        bool ok = sm.transition_to(PipelineState::RECONNECTING);
        VERIFY_MSG(ok, "STREAMING → RECONNECTING should succeed");
        printf("[PASS] STREAMING → RECONNECTING\n");
    }

    {
        // STREAMING → RECONFIGURING
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        sm.transition_to(PipelineState::STREAMING);
        bool ok = sm.transition_to(PipelineState::RECONFIGURING);
        VERIFY_MSG(ok, "STREAMING → RECONFIGURING should succeed");
        printf("[PASS] STREAMING → RECONFIGURING\n");
    }

    {
        // RECONNECTING → STREAMING
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
        // RECONFIGURING → CAPTURING
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
        // Invalid: INIT → STREAMING is rejected
        StateMachine sm;
        bool ok = sm.transition_to(PipelineState::STREAMING);
        VERIFY_MSG(!ok, "INIT → STREAMING must be rejected");
        VERIFY(sm.current_state() == PipelineState::INIT);
        printf("[PASS] invalid transition INIT -> STREAMING is rejected\n");
    }

    {
        // Invalid: INIT → RECONNECTING is rejected
        StateMachine sm;
        bool ok = sm.transition_to(PipelineState::RECONNECTING);
        VERIFY_MSG(!ok, "INIT → RECONNECTING must be rejected");
        VERIFY(sm.current_state() == PipelineState::INIT);
        printf("[PASS] invalid transition INIT -> RECONNECTING is rejected\n");
    }

    {
        // STOPPING is terminal
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::STOPPING);
        VERIFY(sm.current_state() == PipelineState::STOPPING);
        bool ok = sm.transition_to(PipelineState::CAPTURING);
        VERIFY_MSG(!ok, "STOPPING → CAPTURING must be rejected");
        VERIFY(sm.current_state() == PipelineState::STOPPING);
        printf("[PASS] STOPPING is terminal and allows no further transitions\n");
    }

    {
        // FATAL is terminal
        StateMachine sm;
        sm.transition_to(PipelineState::FATAL);
        VERIFY(sm.current_state() == PipelineState::FATAL);
        bool ok = sm.transition_to(PipelineState::CAPTURING);
        VERIFY_MSG(!ok, "FATAL → CAPTURING must be rejected");
        VERIFY(sm.current_state() == PipelineState::FATAL);
        printf("[PASS] FATAL is terminal and allows no further transitions\n");
    }

    {
        // CONNECTING → FATAL (valid; used when encoder init fails)
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        bool ok = sm.transition_to(PipelineState::FATAL);
        VERIFY_MSG(ok, "CONNECTING → FATAL should succeed");
        VERIFY(sm.current_state() == PipelineState::FATAL);
        printf("[PASS] CONNECTING → FATAL\n");
    }

    {
        // RECONFIGURING → FATAL (valid; used when DXGI re-init fails)
        StateMachine sm;
        sm.transition_to(PipelineState::CAPTURING);
        sm.transition_to(PipelineState::CONNECTING);
        sm.transition_to(PipelineState::STREAMING);
        sm.transition_to(PipelineState::RECONFIGURING);
        bool ok = sm.transition_to(PipelineState::FATAL);
        VERIFY_MSG(ok, "RECONFIGURING → FATAL should succeed");
        VERIFY(sm.current_state() == PipelineState::FATAL);
        printf("[PASS] RECONFIGURING → FATAL\n");
    }

    {
        // The transition callback fires with (from, to)
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
        printf("[PASS] transition callback fires correctly\n");
    }

    return 0;
}
