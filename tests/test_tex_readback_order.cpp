#include "tex_readback_policy.hpp"

#include <cassert>
#include <cstdio>

using namespace wwmi_tex_readback;

int main()
{
    // 1. Confirmed post-execute submission signals immediately with 0 ms delay.
    assert(ShouldSignalConfirmed(false, true));
    assert(!ShouldSignalConfirmed(true, true));
    assert(!ShouldSignalConfirmed(false, false));

    // 2. Legacy 1000 ms fallback & pre-execute reset heuristic fire ONLY when paired host
    //    post-execute hook is NOT active. When paired host post-execute IS active, delaying
    //    real ExecuteCommandLists > 1000 ms (e.g. F10 RtWaitQueueIdle in FG FrameEndStep)
    //    MUST NOT signal a fence or allow Map before post-execute confirmation.
    assert(!ShouldSignalLateFallback(false, true, 100, 2000, false, 1000));
    assert(!ShouldSignalLateFallback(false, false, 100, 1099, false, 1000));
    assert(ShouldSignalLateFallback(false, false, 100, 1100, false, 1000));
    assert(!ShouldSignalLateFallback(false, false, 100, 5000, true, 1000));
    assert(!ShouldSignalLateFallback(true, false, 100, 5000, false, 1000));
    assert(!ShouldSignalLateFallback(false, false, 0, 5000, false, 1000));

    assert(ShouldSignalPreExecuteReset(false, false, true, true));
    assert(!ShouldSignalPreExecuteReset(true, false, true, true));
    assert(!ShouldSignalPreExecuteReset(false, true, true, true));
    assert(!ShouldSignalPreExecuteReset(false, false, false, true));
    assert(!ShouldSignalPreExecuteReset(false, false, true, false));

    // 3. Fenced entry: waits until fence completes, then becomes ready without dropping.
    //    UINT64_MAX from GetCompletedValue() signals device removal and MUST drop, never Map.
    {
        assert(FenceValueIsRemoved(UINT64_MAX));
        assert(!FenceValueIsRemoved(0));
        assert(!FenceValueIsRemoved(42));

        const auto wait = EvaluateDrainEntry(true, false, true, true, true, false, true, false);
        assert(!wait.drop && !wait.ready && !wait.release_unsignalled && !wait.release_once_retry);

        const auto done = EvaluateDrainEntry(true, true, true, true, true, false, true, false);
        assert(!done.drop && done.ready && !done.release_unsignalled && !done.release_once_retry);

        const auto removed_fenced = EvaluateDrainEntry(true, false, true, true, true, false, true, false, true);
        assert(removed_fenced.drop && !removed_fenced.ready && !removed_fenced.release_unsignalled);
    }

    // 4. Failed CreateFence (has_queue_fence == false after submit/execute) NEVER maps un-fenced
    //    GPU memory AND NEVER recycles the in-flight buffer prematurely while device is healthy;
    //    once device removal is confirmed, it terminates cleanly and releases unsignalled_.
    {
        const auto live_once = EvaluateDrainEntry(false, false, false, true, true, false, true, false);
        assert(!live_once.drop && !live_once.ready && !live_once.release_unsignalled && !live_once.release_once_retry);

        const auto pre_exec_no_fence = EvaluateDrainEntry(false, false, false, true, false, true, true, false);
        assert(!pre_exec_no_fence.drop && !pre_exec_no_fence.ready && !pre_exec_no_fence.release_unsignalled && !pre_exec_no_fence.release_once_retry);

        const auto removed_no_fence = EvaluateDrainEntry(false, false, false, true, true, false, true, false, true);
        assert(removed_no_fence.drop && !removed_no_fence.ready && removed_no_fence.release_unsignalled && !removed_no_fence.release_once_retry);
    }

    // 5. Abandoned pre-execute command list (reset before ExecuteCommandLists): drops,
    //    decrements unsignalled_, and releases one-shot tried_ so a later copy can retry.
    {
        const auto dropped_once = EvaluateDrainEntry(false, false, false, false, false, true, true, false);
        assert(dropped_once.drop && !dropped_once.ready && dropped_once.release_unsignalled && dropped_once.release_once_retry);

        const auto dropped_upload = EvaluateDrainEntry(false, false, false, false, false, true, false, false);
        assert(dropped_upload.drop && !dropped_upload.ready && dropped_upload.release_unsignalled && !dropped_upload.release_once_retry);
    }

    // 6. Submitted entry awaiting transient Signal retry stays pending even if list generation advances.
    {
        const auto pending_retry = EvaluateDrainEntry(false, false, true, true, true, true, true, false);
        assert(!pending_retry.drop && !pending_retry.ready && !pending_retry.release_unsignalled && !pending_retry.release_once_retry);
    }

    std::puts("test_tex_readback_order: PASS");
    return 0;
}
