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
#include <cstdint>

module lucene_core:ut.rate_limiter;

import lucene_core;

using namespace lucene;

namespace {

constexpr int64_t kOneMb = 1024 * 1024;

} // namespace

TEST(RateLimiterTest, ReportsTheRate) {
    SimpleRateLimiter limiter(10.0);
    EXPECT_DOUBLE_EQ(limiter.mb_per_sec(), 10.0);

    // Five milliseconds worth of the rate is how often a caller should report.
    EXPECT_EQ(limiter.min_pause_check_bytes(), static_cast<int64_t>(0.005 * 10.0 * 1024.0 * 1024.0));

    ASSERT_TRUE(limiter.set_mb_per_sec(2.0).has_value());
    EXPECT_DOUBLE_EQ(limiter.mb_per_sec(), 2.0);
    EXPECT_EQ(limiter.min_pause_check_bytes(), static_cast<int64_t>(0.005 * 2.0 * 1024.0 * 1024.0));
}

TEST(RateLimiterTest, RejectsANegativeRate) {
    SimpleRateLimiter limiter(1.0);
    auto negative = limiter.set_mb_per_sec(-1.0);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);
    // The rate is unchanged.
    EXPECT_DOUBLE_EQ(limiter.mb_per_sec(), 1.0);
}

TEST(RateLimiterTest, DoesNotPauseWithoutALimit) {
    SimpleRateLimiter limiter(0.0);
    const auto start = std::chrono::steady_clock::now();

    auto paused = limiter.pause(kOneMb);
    ASSERT_TRUE(paused.has_value()) << (paused ? "" : paused.error().to_string());
    EXPECT_EQ(paused.value(), 0);

    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);
}

TEST(RateLimiterTest, PausesForTheTimeTheBytesTake) {
    // 1000 MB/s makes one megabyte cost one millisecond, which keeps the test fast
    // while still measuring a pause.
    SimpleRateLimiter limiter(1000.0);
    const auto start = std::chrono::steady_clock::now();

    auto paused = limiter.pause(kOneMb);
    ASSERT_TRUE(paused.has_value()) << (paused ? "" : paused.error().to_string());

    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(), 900);
    EXPECT_GT(paused.value(), 0);
}

TEST(RateLimiterTest, PacesTheInstantaneousRate) {
    // Two reports back to back each have to wait, because the limiter paces the
    // instantaneous rate rather than the average since it was created.
    SimpleRateLimiter limiter(1000.0);
    const auto start = std::chrono::steady_clock::now();

    ASSERT_TRUE(limiter.pause(kOneMb).has_value());
    ASSERT_TRUE(limiter.pause(kOneMb).has_value());

    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(), 1800);
}
