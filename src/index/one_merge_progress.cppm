/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

export module lucene_core:one_merge_progress;

import :types;
import std;

namespace lucene {

/// Why a merge thread is paused.
export enum class PauseReason : uint8_t {
    /// Stopped, because the throughput rate is `0`.
    kStopped = 0,
    /// Temporarily paused because the write rate was exceeded.
    kPaused = 1,
    /// Any other reason.
    kOther = 2,
};

/// Progress and state of one running merge: it can be paused and resumed, and it
/// can be aborted.
///
/// Port of `OneMergeProgress` (`index/merge_policy.rs`).
///
/// Only the thread that called [`set_merge_thread`] may pause; that invariant is
/// checked in debug builds, like the reference implementation.  The paused time is
/// accumulated per [`PauseReason`] so the merge scheduler can report how much of
/// a merge was spent waiting on I/O.
export class OneMergeProgress {
public:
    OneMergeProgress() = default;

    /// Aborts the merge at the next possible moment and wakes a paused thread.
    void abort();

    [[nodiscard]] bool is_aborted() const noexcept { return aborted_.load(std::memory_order_seq_cst); }

    /// Pauses the calling thread for at least `pause_nanos` nanoseconds, unless
    /// the merge is aborted or `condition` returns `false` (which lets another
    /// thread end the pause early, e.g. when the rate changes).
    void pause_nanos(uint64_t pause_nanos, PauseReason reason, const std::function<bool()> &condition);

    /// Wakes any thread stalled in [`pause_nanos`].
    void wakeup();

    /// Nanoseconds spent paused for `reason`.
    [[nodiscard]] uint64_t pause_time(PauseReason reason) const noexcept;

    /// Records the calling thread as the one that may pause the merge.
    void set_merge_thread();

    /// Forgets the merge thread recorded by [`set_merge_thread`].
    void clear_merge_thread();

private:
    void add_pause_time(PauseReason reason, uint64_t nanos);

    std::mutex pause_mutex_;
    std::condition_variable pausing_;
    std::atomic<uint64_t> stopped_ns_{0};
    std::atomic<uint64_t> paused_ns_{0};
    std::atomic<uint64_t> other_ns_{0};
    std::atomic<bool> aborted_{false};
    std::mutex owner_mutex_;
    std::optional<std::thread::id> owner_{};
};

} // namespace lucene
