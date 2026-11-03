#pragma once
#include "common/types.hpp"
#include <atomic>
#include <functional>
#include <unordered_set>
#include <utility>

/**
 * @brief State machine for a monitor pipeline: rejects transitions not in the table.
 *
 * Only the owning pipeline thread calls transition_to(); it is a non-atomic
 * check-then-store and is not safe for concurrent callers. current_state() may
 * be read from any thread.
 */
class StateMachine {
public:
    /** Called after a successful transition with (from, to). */
    using TransitionCallback = std::function<void(PipelineState /*from*/,
                                                   PipelineState /*to*/)>;

    StateMachine();

    PipelineState current_state() const { return current_.load(); }

    /** @brief True if the current state may move to @p to. */
    bool can_transition(PipelineState to) const;

    /**
     * @brief Move to @p to, invoking the callback on success.
     * @return false (no change, no callback) if the transition is not allowed
     */
    bool transition_to(PipelineState to);

    /** @brief Register the transition callback (replaces any previous one); set before start. */
    void on_transition(TransitionCallback cb) { callback_ = std::move(cb); }

private:
    std::atomic<PipelineState> current_;
    TransitionCallback callback_;

    /** Hash for the (from, to) pair used as the unordered_set key. */
    struct PairHash {
        size_t operator()(const std::pair<PipelineState, PipelineState>& p) const {
            auto h1 = std::hash<int>{}(static_cast<int>(p.first));
            auto h2 = std::hash<int>{}(static_cast<int>(p.second));
            return h1 ^ (h2 << 16);
        }
    };

    /** Allowed (from, to) pairs. */
    std::unordered_set<std::pair<PipelineState, PipelineState>, PairHash> valid_transitions_;

    void build_transition_table();
};
