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

module lucene_core:rate_limiter.impl;

import :rate_limiter;

import std;

import :error;
import :types;

namespace lucene {

namespace {

/// How often a caller should report its bytes: often enough that the pause is
/// short, rarely enough that the bookkeeping is negligible.
constexpr int64_t kMinPauseCheckMsec = 5;

} // namespace

SimpleRateLimiter::SimpleRateLimiter(double mb_per_sec) : origin_(std::chrono::steady_clock::now()) {
    // The constructor cannot fail for a valid rate; the reference ignores the
    // result for the same reason.
    (void)set_mb_per_sec(mb_per_sec);
}

VoidResult SimpleRateLimiter::set_mb_per_sec(double mb_per_sec) {
    if (!(mb_per_sec >= 0.0)) {
        return err(LuceneError::illegal_argument("mbPerSec must be a non-negative number (got " +
                                                 std::to_string(mb_per_sec) + ")"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    mb_per_sec_ = mb_per_sec;
    min_pause_check_bytes_ =
            static_cast<int64_t>((static_cast<double>(kMinPauseCheckMsec) / 1000.0) * mb_per_sec * 1024.0 * 1024.0);
    return {};
}

double SimpleRateLimiter::mb_per_sec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mb_per_sec_;
}

int64_t SimpleRateLimiter::min_pause_check_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_pause_check_bytes_;
}

int64_t SimpleRateLimiter::clock_ns() const {
    const auto elapsed = std::chrono::steady_clock::now() - origin_;
    return static_cast<int64_t>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) +
           1;
}

Result<int64_t> SimpleRateLimiter::pause(int64_t bytes) {
    const int64_t start_ns = clock_ns();

    const double rate = mb_per_sec();
    if (rate == 0.0) {
        // No limit: report the bytes as instantaneous.
        return 0;
    }

    const double seconds_to_pause = (static_cast<double>(bytes) / 1024.0 / 1024.0) / rate;
    int64_t target_ns = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The time we should sleep until: the last pause plus the time these
        // bytes take, which paces the instantaneous rate rather than the average
        // over all history.
        target_ns = last_ns_ + static_cast<int64_t>(1'000'000'000.0 * seconds_to_pause);
        if (start_ns >= target_ns) {
            // Already past the target: nothing to do, but keep the instant rate by
            // moving the marker to now rather than to the target.
            last_ns_ = start_ns;
            return 0;
        }
        last_ns_ = target_ns;
    }

    int64_t current_ns = start_ns;
    while (true) {
        const int64_t pause_ns = target_ns - current_ns;
        if (pause_ns <= 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(pause_ns));
        current_ns = clock_ns();
    }
    return current_ns - start_ns;
}

} // namespace lucene
