#include <cstdio>
#include "audio/audio_pipeline.hpp"
#include "test_utils.hpp"

/**
 * @brief Tests AudioSubscriberQueue and AudioPipeline subscribe/unsubscribe.
 *
 * Covers only what needs no real WASAPI device; the broadcast loop in
 * pipeline_thread_func() is not tested.
 *
 * @return 0 on success (a failing VERIFY exits with status 1)
 */
int run_audio_pipeline_tests() {
    printf("=== Audio Pipeline Tests ===\n");

    {
        // Pushed packets come back from try_pop in FIFO order
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
        printf("[PASS] pushed packets are popped in FIFO order\n");
    }

    {
        // try_pop on an empty queue returns false
        AudioSubscriberQueue q;
        EncodedPacket out;
        VERIFY(!q.try_pop(out));
        printf("[PASS] try_pop returns false on an empty queue\n");
    }

    {
        // wait_pop returns false on timeout
        AudioSubscriberQueue q;
        EncodedPacket out;
        VERIFY(!q.wait_pop(out, 50));
        printf("[PASS] wait_pop returns false on timeout\n");
    }

    {
        // Overflowing the queue drops the oldest packets
        AudioSubscriberQueue q;
        for (int i = 0; i < 100; ++i) {
            EncodedPacket p; p.pts = i;
            q.push(p);
        }
        EncodedPacket out;
        VERIFY(q.try_pop(out));
        VERIFY_MSG(out.pts > 0, "Oldest packets should have been dropped");
        printf("[PASS] the oldest packet is dropped when the queue limit is exceeded\n");
    }

    {
        // subscribe/unsubscribe work without throwing
        // (start() is not called to avoid needing a real device)
        AudioPipeline pipeline;
        auto sub1 = pipeline.subscribe();
        auto sub2 = pipeline.subscribe();
        VERIFY(sub1 != nullptr);
        VERIFY(sub2 != nullptr);
        VERIFY(sub1 != sub2);
        pipeline.unsubscribe(sub1);
        pipeline.unsubscribe(sub2);
        VERIFY(!pipeline.is_running());
        printf("[PASS] AudioPipeline subscribe / unsubscribe works\n");
    }

    return 0;
}
