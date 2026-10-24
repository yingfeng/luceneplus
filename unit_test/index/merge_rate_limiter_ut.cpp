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

module;

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <thread>

module lucene_core:ut.merge_rate_limiter;

import lucene_core;

using namespace lucene;

namespace {

constexpr int64_t ONE_MB = 1024 * 1024;

std::shared_ptr<OneMergeProgress> make_progress() { return std::make_shared<OneMergeProgress>(); }

} // namespace

TEST(OneMergeProgressTest, AbortIsStickyAndRecorded) {
    OneMergeProgress progress;
    EXPECT_FALSE(progress.is_aborted());
    progress.abort();
    EXPECT_TRUE(progress.is_aborted());
    EXPECT_EQ(progress.pause_time(PauseReason::kStopped), 0U);
}

TEST(OneMergeProgressTest, PauseNanosWaitsAtLeastTheRequestedTime) {
    OneMergeProgress progress;
    progress.set_merge_thread();

    const auto start = std::chrono::steady_clock::now();
    progress.pause_nanos(5'000'000, PauseReason::kPaused, [] { return true; });
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 4);
    EXPECT_GT(progress.pause_time(PauseReason::kPaused), 0U);
    EXPECT_EQ(progress.pause_time(PauseReason::kOther), 0U);
}

TEST(OneMergeProgressTest, AFalseConditionEndsThePauseImmediately) {
    OneMergeProgress progress;
    const auto start = std::chrono::steady_clock::now();
    progress.pause_nanos(200'000'000, PauseReason::kOther, [] { return false; });
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
    EXPECT_GT(progress.pause_time(PauseReason::kOther), 0U);
}

TEST(OneMergeProgressTest, AbortWakesAPausedThread) {
    OneMergeProgress progress;
    std::thread aborter([&progress] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        progress.abort();
    });

    const auto start = std::chrono::steady_clock::now();
    // Would sleep 200ms if the abort did not wake it.
    progress.pause_nanos(200'000'000, PauseReason::kPaused, [] { return true; });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    aborter.join();

    EXPECT_TRUE(progress.is_aborted());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);
    EXPECT_GT(progress.pause_time(PauseReason::kPaused), 0U);
}

TEST(MergeRateLimiterTest, StartsUnlimited) {
    auto progress = make_progress();
    MergeRateLimiter limiter(progress);

    EXPECT_FALSE(std::isfinite(limiter.mb_per_sec()));
    EXPECT_EQ(limiter.min_pause_check_bytes(), ONE_MB);
    EXPECT_EQ(limiter.total_bytes_written(), 0);
    EXPECT_FALSE(limiter.is_aborted());

    // Unlimited: no pause at all.
    const auto start = std::chrono::steady_clock::now();
    auto paused = limiter.pause(ONE_MB);
    ASSERT_TRUE(paused.has_value());
    EXPECT_EQ(paused.value(), 0);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
              50);
}

TEST(MergeRateLimiterTest, ConvertsTheRateIntoAThreshold) {
    auto progress = make_progress();
    MergeRateLimiter limiter(progress);

    ASSERT_TRUE(limiter.set_mb_per_sec(10.0).has_value());
    EXPECT_DOUBLE_EQ(limiter.mb_per_sec(), 10.0);
    // 25ms of 10 MB/s is 256 KB, below the 1 MB cap.
    EXPECT_EQ(limiter.min_pause_check_bytes(), static_cast<int64_t>(0.025 * 10.0 * ONE_MB));

    ASSERT_TRUE(limiter.set_mb_per_sec(1000.0).has_value());
    EXPECT_EQ(limiter.min_pause_check_bytes(), ONE_MB) << "the threshold is capped at 1 MB";

    ASSERT_TRUE(limiter.set_mb_per_sec(0.0).has_value());
    EXPECT_EQ(limiter.min_pause_check_bytes(), 0);
}

TEST(MergeRateLimiterTest, RejectsANegativeRate) {
    auto progress = make_progress();
    MergeRateLimiter limiter(progress);
    ASSERT_TRUE(limiter.set_mb_per_sec(5.0).has_value());

    auto rejected = limiter.set_mb_per_sec(-1.0);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_DOUBLE_EQ(limiter.mb_per_sec(), 5.0) << "the old rate survives";
}

TEST(MergeRateLimiterTest, PausesWhenTheRateIsExceeded) {
    auto progress = make_progress();
    MergeRateLimiter limiter(progress);
    // 100 MB/s: one megabyte takes ~10ms, well above the 2ms minimum pause.
    ASSERT_TRUE(limiter.set_mb_per_sec(100.0).has_value());

    const auto start = std::chrono::steady_clock::now();
    auto paused = limiter.pause(ONE_MB);
    ASSERT_TRUE(paused.has_value()) << (paused ? "" : paused.error().to_string());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 4);
    EXPECT_GT(paused.value(), 0);
    EXPECT_EQ(limiter.total_bytes_written(), ONE_MB);
    EXPECT_GT(limiter.total_paused_ns(), 0U);
    EXPECT_EQ(limiter.total_stopped_ns(), 0U);
}

TEST(MergeRateLimiterTest, SmallWritesAreNotPaused) {
    auto progress = make_progress();
    MergeRateLimiter limiter(progress);
    ASSERT_TRUE(limiter.set_mb_per_sec(100.0).has_value());

    // 4 KB at 100 MB/s is ~40µs, below the 2ms minimum pause.
    auto paused = limiter.pause(4096);
    ASSERT_TRUE(paused.has_value());
    EXPECT_EQ(paused.value(), 0);
    EXPECT_EQ(limiter.total_paused_ns(), 0U);
    EXPECT_EQ(limiter.total_bytes_written(), 4096);
}

TEST(MergeRateLimiterTest, AnAbortedMergeFailsInsteadOfPausing) {
    auto progress = make_progress();
    MergeRateLimiter limiter(progress);
    ASSERT_TRUE(limiter.set_mb_per_sec(100.0).has_value());
    progress->abort();

    EXPECT_TRUE(limiter.is_aborted());
    auto paused = limiter.pause(ONE_MB);
    ASSERT_FALSE(paused.has_value());
    EXPECT_EQ(paused.error().kind(), ErrorKind::kMergeAborted);
}

TEST(MergeRateLimiterTest, RateZeroParksTheMergeAndCountsStoppedTime) {
    auto progress = make_progress();
    MergeRateLimiter limiter(progress);
    ASSERT_TRUE(limiter.set_mb_per_sec(0.0).has_value());

    std::thread aborter([&progress] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        progress->abort();
    });

    const auto start = std::chrono::steady_clock::now();
    auto paused = limiter.pause(ONE_MB);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    aborter.join();

    // The pause ends as an abort, and the time spent parked is booked as
    // "stopped" rather than "paused".
    ASSERT_FALSE(paused.has_value());
    EXPECT_EQ(paused.error().kind(), ErrorKind::kMergeAborted);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 4);
    EXPECT_GT(limiter.total_stopped_ns(), 0U);
    EXPECT_EQ(limiter.total_paused_ns(), 0U);
}
