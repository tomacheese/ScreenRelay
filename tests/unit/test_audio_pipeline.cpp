#include <cstdio>
#include "audio/audio_pipeline.hpp"
#include "test_utils.hpp"

/**
 * @brief AudioSubscriberQueue のユニットテストを実行する
 *
 * WASAPI 実デバイスに依存しない範囲（購読キューの push/pop、AudioPipeline の
 * 購読・購読解除の管理）のみを対象とする。実デバイスが必要な
 * pipeline_thread_func() 内のブロードキャスト処理自体は対象外。
 *
 * @return 成功時 0 (失敗時は VERIFY マクロにより exit(1))
 */
int run_audio_pipeline_tests() {
    printf("=== Audio Pipeline Tests ===\n");

    {
        // push したパケットを try_pop で取得できること (FIFO 順)
        AudioSubscriberQueue q;
        EncodedPacket p1; p1.pts = 1;
        EncodedPacket p2; p2.pts = 2;
        q.push(p1);
        q.push(p2);

        EncodedPacket out;
        VERIFY(q.try_pop(out));
        VERIFY(out.pts == 1);
        VERIFY(q.try_pop(out));
        VERIFY(out.pts == 2);
        VERIFY(!q.try_pop(out));
        printf("[PASS] push したパケットが FIFO 順で取得できる\n");
    }

    {
        // 空のキューで try_pop が false を返すこと
        AudioSubscriberQueue q;
        EncodedPacket out;
        VERIFY(!q.try_pop(out));
        printf("[PASS] 空のキューでは try_pop が false を返す\n");
    }

    {
        // wait_pop がタイムアウトで false を返すこと
        AudioSubscriberQueue q;
        EncodedPacket out;
        VERIFY(!q.wait_pop(out, 50));
        printf("[PASS] wait_pop がタイムアウトで false を返す\n");
    }

    {
        // 最大サイズを超えた場合に最古パケットが破棄されること
        AudioSubscriberQueue q;
        for (int i = 0; i < 100; ++i) {
            EncodedPacket p; p.pts = i;
            q.push(p);
        }
        EncodedPacket out;
        VERIFY(q.try_pop(out));
        VERIFY_MSG(out.pts > 0, "Oldest packets should have been dropped");
        printf("[PASS] キュー上限超過時に最古パケットが破棄される\n");
    }

    {
        // AudioPipeline の購読・購読解除が例外なく動作すること
        // (実デバイスへの依存を避けるため start() は呼ばない)
        AudioPipeline pipeline;
        auto sub1 = pipeline.subscribe();
        auto sub2 = pipeline.subscribe();
        VERIFY(sub1 != nullptr);
        VERIFY(sub2 != nullptr);
        VERIFY(sub1 != sub2);
        pipeline.unsubscribe(sub1);
        pipeline.unsubscribe(sub2);
        VERIFY(!pipeline.is_running());
        printf("[PASS] AudioPipeline の購読・購読解除が動作する\n");
    }

    return 0;
}
