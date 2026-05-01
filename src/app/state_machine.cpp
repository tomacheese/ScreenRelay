#include "app/state_machine.hpp"

StateMachine::StateMachine() : current_(PipelineState::INIT) {
    build_transition_table();
}

void StateMachine::build_transition_table() {
    using S = PipelineState;

    // 各ステートから遷移可能なステートを定義する
    // 仕様:
    //   INIT         → CAPTURING, FATAL
    //   CAPTURING    → CONNECTING, STOPPING, FATAL
    //   CONNECTING   → STREAMING, CAPTURING, STOPPING
    //   STREAMING    → RECONNECTING, RECONFIGURING, STOPPING, FATAL
    //   RECONNECTING → STREAMING, CAPTURING, STOPPING
    //   RECONFIGURING→ CAPTURING, STOPPING
    //   STOPPING     → (なし、terminal)
    //   FATAL        → (なし、terminal)
    valid_transitions_ = {
        // INIT からの遷移
        {S::INIT,          S::CAPTURING},
        {S::INIT,          S::FATAL},

        // CAPTURING からの遷移
        {S::CAPTURING,     S::CONNECTING},
        {S::CAPTURING,     S::STOPPING},
        {S::CAPTURING,     S::FATAL},

        // CONNECTING からの遷移
        {S::CONNECTING,    S::STREAMING},
        {S::CONNECTING,    S::CAPTURING},
        {S::CONNECTING,    S::STOPPING},

        // STREAMING からの遷移
        {S::STREAMING,     S::RECONNECTING},
        {S::STREAMING,     S::RECONFIGURING},
        {S::STREAMING,     S::STOPPING},
        {S::STREAMING,     S::FATAL},

        // RECONNECTING からの遷移
        {S::RECONNECTING,  S::STREAMING},
        {S::RECONNECTING,  S::CAPTURING},
        {S::RECONNECTING,  S::STOPPING},

        // RECONFIGURING からの遷移
        {S::RECONFIGURING, S::CAPTURING},
        {S::RECONFIGURING, S::STOPPING},

        // STOPPING と FATAL は terminal ステートのため遷移なし
    };
}

bool StateMachine::can_transition(PipelineState to) const {
    return valid_transitions_.count({current_.load(), to}) > 0;
}

bool StateMachine::transition_to(PipelineState to) {
    PipelineState from = current_.load();
    if (!valid_transitions_.count({from, to})) return false;
    current_.store(to);
    if (callback_) callback_(from, to);
    return true;
}
