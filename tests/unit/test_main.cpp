#include <cstdio>

// 各テストファイルで定義されるテスト実行関数の前方宣言
extern int run_config_loader_tests();
extern int run_state_machine_tests();
extern int run_backoff_tests();
extern int run_metrics_tests();
extern int run_monitor_detector_tests();
extern int run_audio_pipeline_tests();

/**
 * @brief ユニットテストのエントリーポイント
 *
 * 全テストモジュールを順に呼び出す。
 * いずれかのテストが失敗すると VERIFY マクロにより exit(1) で終了する。
 */
int main() {
    printf("=== screen-relay Unit Tests ===\n");
    run_config_loader_tests();
    run_state_machine_tests();
    run_backoff_tests();
    run_metrics_tests();
    run_monitor_detector_tests();
    run_audio_pipeline_tests();
    printf("\nAll tests passed.\n");
    return 0;
}
