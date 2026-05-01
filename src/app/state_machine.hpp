#pragma once
#include "common/types.hpp"
#include <atomic>
#include <functional>
#include <unordered_set>
#include <utility>

/**
 * @brief モニターパイプライン用ステートマシン
 *
 * PipelineState の有効な遷移テーブルを管理し、不正な遷移を防ぐ。
 * 遷移時にコールバックを呼び出す仕組みを持つ。
 */
class StateMachine {
public:
    /**
     * @brief ステート遷移コールバック型
     *
     * 遷移前のステートと遷移後のステートを引数として受け取る。
     */
    using TransitionCallback = std::function<void(PipelineState /*from*/,
                                                   PipelineState /*to*/)>;

    /** @brief コンストラクタ。初期ステートを INIT に設定し遷移テーブルを構築する。 */
    StateMachine();

    /**
     * @brief 現在のステートを返す
     * @return 現在の PipelineState
     */
    PipelineState current_state() const { return current_.load(); }

    /**
     * @brief 指定ステートへの遷移が有効か判定する
     * @param to 遷移先ステート
     * @return 有効な遷移であれば true
     */
    bool can_transition(PipelineState to) const;

    /**
     * @brief 指定ステートへ遷移する
     *
     * 有効な遷移でない場合は何もせず false を返す。
     * 成功した場合はコールバックが呼び出される。
     * @param to 遷移先ステート
     * @return 遷移成功時 true、無効な遷移の場合 false
     */
    bool transition_to(PipelineState to);

    /**
     * @brief ステート遷移時のコールバックを登録する
     * @param cb 遷移コールバック
     */
    void on_transition(TransitionCallback cb) { callback_ = std::move(cb); }

private:
    std::atomic<PipelineState> current_;          ///< 現在のステート
    TransitionCallback callback_;    ///< 遷移時コールバック

    /**
     * @brief ペアのハッシュ関数オブジェクト
     *
     * unordered_set のキーとして std::pair<PipelineState, PipelineState> を
     * 使用するためのカスタムハッシュ。
     */
    struct PairHash {
        size_t operator()(const std::pair<PipelineState, PipelineState>& p) const {
            auto h1 = std::hash<int>{}(static_cast<int>(p.first));
            auto h2 = std::hash<int>{}(static_cast<int>(p.second));
            return h1 ^ (h2 << 16);
        }
    };

    /** 有効な遷移ペアのセット */
    std::unordered_set<std::pair<PipelineState, PipelineState>, PairHash> valid_transitions_;

    /** @brief 有効な遷移テーブルを構築する */
    void build_transition_table();
};
