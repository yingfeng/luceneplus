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

module lucene_core:merge_rate_limiter.impl;

import :merge_rate_limiter;

import std;

import :error;
import :one_merge_progress;
import :rate_limiter;
import :types;

namespace lucene {

namespace {

/// How often a merge should report its bytes to the limiter.
constexpr int64_t MIN_PAUSE_CHECK_MSEC = 25;
/// Pauses shorter than this are not worth the wakeup.
constexpr int64_t MIN_PAUSE_NS = 2'000'000;
/// A single pause never lasts longer, so a rate change is picked up quickly.
constexpr int64_t MAX_PAUSE_NS = 250'000'000;
constexpr int64_t ONE_MB = 1024 * 1024;

} // namespace

MergeRateLimiter::MergeRateLimiter(std::shared_ptr<OneMergeProgress> merge_progress)
    : origin_(std::chrono::steady_clock::now()), merge_progress_(std::move(merge_progress)) {
    // Unlimited to start with; the setter also initialises
    // `min_pause_check_bytes_`.
    (void)set_mb_per_sec(std::numeric_limits<double>::infinity());
}

VoidResult MergeRateLimiter::set_mb_per_sec(double mb_per_sec) {
    if (!(mb_per_sec >= 0.0)) {
        return err(LuceneError::illegal_argument("mbPerSec must be positive; got: " + std::to_string(mb_per_sec)));
    }
    int64_t min_check = ONE_MB;
    if (std::isfinite(mb_per_sec)) {
        const double raw = (static_cast<double>(MIN_PAUSE_CHECK_MSEC) / 1000.0) * mb_per_sec * ONE_MB;
        min_check = raw >= static_cast<double>(ONE_MB) ? ONE_MB : static_cast<int64_t>(raw);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mb_per_sec_ = mb_per_sec;
        min_pause_check_bytes_ = min_check;
    }
    // Wakes a merge parked at rate 0 so it re-reads the rate.
    merge_progress_->wakeup();
    return {};
}

double MergeRateLimiter::mb_per_sec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mb_per_sec_;
}

int64_t MergeRateLimiter::min_pause_check_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_pause_check_bytes_;
}

Result<int64_t> MergeRateLimiter::maybe_pause(int64_t bytes) {
    // A good moment to notice that somebody aborted the merge.
    if (merge_progress_->is_aborted()) {
        return err(LuceneError::merge_aborted("Merge aborted."));
    }

    const double rate = mb_per_sec();
    const double seconds_to_pause = (static_cast<double>(bytes) / static_cast<double>(ONE_MB)) / rate;

    int64_t cur_pause_ns = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto elapsed = std::chrono::steady_clock::now() - origin_;
        const auto cur_ns = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        // The instant we should sleep until: the last pause plus the time these
        // bytes take.  Pacing the instantaneous rate (rather than the average
        // over all history) is what keeps a long merge from banking credit.
        int64_t target_ns = last_ns_;
        if (std::isfinite(seconds_to_pause)) {
            target_ns += static_cast<int64_t>(1'000'000'000.0 * seconds_to_pause);
        } else {
            target_ns = std::numeric_limits<int64_t>::max() / 2;
        }
        const int64_t pause = target_ns - cur_ns;
        if (pause <= MIN_PAUSE_NS) {
            // Set to "now", not to the target, to enforce the instant rate.
            last_ns_ = cur_ns;
            cur_pause_ns = 0;
        } else {
            cur_pause_ns = pause;
        }
    }

    if (cur_pause_ns == 0) {
        return int64_t{-1};
    }
    if (cur_pause_ns > MAX_PAUSE_NS) {
        cur_pause_ns = MAX_PAUSE_NS;
    }

    const auto start = std::chrono::steady_clock::now();
    merge_progress_->pause_nanos(static_cast<uint64_t>(cur_pause_ns),
                                 rate == 0.0 ? PauseReason::kStopped : PauseReason::kPaused,
                                 [this, rate] { return rate == mb_per_sec(); });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

Result<int64_t> MergeRateLimiter::pause(int64_t bytes) {
    total_bytes_written_.fetch_add(bytes, std::memory_order_seq_cst);

    // Loop because the rate may change while this thread is parking.
    int64_t paused = 0;
    while (true) {
        auto delta = maybe_pause(bytes);
        if (!delta.has_value()) {
            return std::unexpected(std::move(delta.error()));
        }
        if (delta.value() < 0) {
            return paused;
        }
        paused += delta.value();
    }
}

} // namespace lucene
