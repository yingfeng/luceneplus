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

export module lucene_core:merge_rate_limiter;

import :types;
import std;

import :error;
import :one_merge_progress;
import :rate_limiter;

namespace lucene {

/// A [`RateLimiter`] whose `0` MB/s means "stop this merge", and which reports
/// its pauses to an [`OneMergeProgress`].
///
/// Port of `MergeRateLimiter` (`index/merge_rate_limiter.rs`).
///
/// The writer gives one of these to every running merge, which is how a merge
/// scheduler throttles individual merges (and how `IndexWriter.set_merge_rate`
/// reaches them): unlike [`SimpleRateLimiter`], a rate of `0` is meaningful here
/// and parks the merge instead of disabling the limit.  Pauses shorter than 2 ms
/// are not worth the syscall and are skipped; a single pause never exceeds
/// 250 ms so a rate change is noticed quickly.
export class MergeRateLimiter final : public RateLimiter {
public:
    /// Starts unlimited; the progress object must outlive the limiter.
    explicit MergeRateLimiter(std::shared_ptr<OneMergeProgress> merge_progress);

    // ---- RateLimiter ----

    /// `0` stops the merge; negative values are rejected.
    [[nodiscard]] VoidResult set_mb_per_sec(double mb_per_sec) override;
    [[nodiscard]] double mb_per_sec() const override;
    [[nodiscard]] Result<int64_t> pause(int64_t bytes) override;
    [[nodiscard]] int64_t min_pause_check_bytes() const override;

    // ---- merge statistics ----

    /// Bytes reported to [`pause`] so far.
    [[nodiscard]] int64_t total_bytes_written() const noexcept {
        return total_bytes_written_.load(std::memory_order_seq_cst);
    }
    /// Nanoseconds the merge spent parked because the rate was `0`.
    [[nodiscard]] uint64_t total_stopped_ns() const noexcept {
        return merge_progress_->pause_time(PauseReason::kStopped);
    }
    /// Nanoseconds the merge spent paused because of the rate limit.
    [[nodiscard]] uint64_t total_paused_ns() const noexcept {
        return merge_progress_->pause_time(PauseReason::kPaused);
    }
    /// Whether the merge was aborted.
    [[nodiscard]] bool is_aborted() const noexcept { return merge_progress_->is_aborted(); }

    [[nodiscard]] const OneMergeProgress &merge_progress() const noexcept { return *merge_progress_; }

private:
    /// Pauses once, or returns `-1` when no pause is needed.
    [[nodiscard]] Result<int64_t> maybe_pause(int64_t bytes);

    mutable std::mutex mutex_;
    double mb_per_sec_{0.0};
    int64_t min_pause_check_bytes_{0};
    /// "Next allowed" instant, in nanoseconds since the limiter was created.
    int64_t last_ns_{0};
    std::chrono::steady_clock::time_point origin_;
    std::shared_ptr<OneMergeProgress> merge_progress_;
    std::atomic<int64_t> total_bytes_written_{0};
};

} // namespace lucene
