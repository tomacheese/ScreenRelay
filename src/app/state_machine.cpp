#include "app/state_machine.hpp"

StateMachine::StateMachine() : current_(PipelineState::INIT) {
    build_transition_table();
}

void StateMachine::build_transition_table() {
    using S = PipelineState;

    valid_transitions_ = {
        {S::INIT,          S::CAPTURING},
        {S::INIT,          S::FATAL},

        {S::CAPTURING,     S::CONNECTING},
        {S::CAPTURING,     S::STOPPING},
        {S::CAPTURING,     S::FATAL},

        {S::CONNECTING,    S::STREAMING},
        {S::CONNECTING,    S::CAPTURING},
        {S::CONNECTING,    S::STOPPING},
        {S::CONNECTING,    S::FATAL},

        {S::STREAMING,     S::RECONNECTING},
        {S::STREAMING,     S::RECONFIGURING},
        {S::STREAMING,     S::STOPPING},
        {S::STREAMING,     S::FATAL},

        {S::RECONNECTING,  S::STREAMING},
        {S::RECONNECTING,  S::CAPTURING},
        {S::RECONNECTING,  S::STOPPING},

        {S::RECONFIGURING, S::CAPTURING},
        {S::RECONFIGURING, S::STOPPING},
        {S::RECONFIGURING, S::FATAL},

        // STOPPING and FATAL are terminal: no outgoing transitions
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
