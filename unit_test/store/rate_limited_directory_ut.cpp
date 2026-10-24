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
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

module lucene_core:ut.rate_limited_directory;

import lucene_core;

using namespace lucene;

namespace {

/// A limiter that records what it was asked to pause for, and can change its
/// threshold after each pause so the cached value in the wrapper is observable.
class RecordingRateLimiter final : public RateLimiter {
public:
    explicit RecordingRateLimiter(int64_t min_pause_check_bytes)
        : min_pause_check_bytes_(min_pause_check_bytes) {}

    [[nodiscard]] VoidResult set_mb_per_sec(double mb_per_sec) override {
        mb_per_sec_ = mb_per_sec;
        return {};
    }
    [[nodiscard]] double mb_per_sec() const override { return mb_per_sec_; }

    [[nodiscard]] Result<int64_t> pause(int64_t bytes) override {
        reported.push_back(bytes);
        if (next_threshold_.has_value()) {
            min_pause_check_bytes_ = next_threshold_.value();
            next_threshold_.reset();
        }
        return static_cast<int64_t>(0);
    }

    [[nodiscard]] int64_t min_pause_check_bytes() const override { return min_pause_check_bytes_; }

    /// Every byte count that reached [`pause`], in order.
    std::vector<int64_t> reported;
    /// Threshold to install at the next pause.
    std::optional<int64_t> next_threshold_;

private:
    double mb_per_sec_{0.0};
    int64_t min_pause_check_bytes_;
};

/// Reads back the whole of `name` from `directory`.
std::vector<uint8_t> read_all(Directory &directory, std::string_view name) {
    auto input = directory.open_input(name, io_context_default());
    EXPECT_TRUE(input.has_value()) << (input ? "" : input.error().to_string());
    if (!input.has_value()) {
        return {};
    }
    auto length = input.value()->length();
    EXPECT_TRUE(length.has_value());
    if (!length.has_value()) {
        return {};
    }
    std::vector<uint8_t> bytes(length.value());
    if (!bytes.empty()) {
        EXPECT_TRUE(input.value()->read_bytes(bytes).has_value());
    }
    EXPECT_TRUE(input.value()->close().has_value());
    return bytes;
}

/// Writes `count` bytes of a fixed value to make the byte accounting visible.
VoidResult write_filler(IndexOutput &output, size_t count, uint8_t value = 0x5A) {
    const std::vector<uint8_t> bytes(count, value);
    return output.write_bytes(bytes);
}

} // namespace

TEST(RateLimitedIndexOutputTest, WritesAreForwardedAndTheBytesMatchTheDelegate) {
    ByteBuffersDirectory directory;

    // The same payload through a plain output, to compare checksums.
    uint64_t expected_checksum = 0;
    {
        auto plain = directory.create_output("plain", io_context_default());
        ASSERT_TRUE(plain.has_value()) << (plain ? "" : plain.error().to_string());
        ASSERT_TRUE(plain.value()->write_byte(0x01).has_value());
        ASSERT_TRUE(plain.value()->write_short(0x0203).has_value());
        ASSERT_TRUE(plain.value()->write_int(0x04050607).has_value());
        ASSERT_TRUE(plain.value()->write_long(0x08090A0B0C0D0E0FLL).has_value());
        ASSERT_TRUE(plain.value()->write_string("abc").has_value());
        expected_checksum = plain.value()->get_checksum();
        ASSERT_TRUE(plain.value()->close().has_value());
    }

    auto delegate = directory.create_output("limited", io_context_default());
    ASSERT_TRUE(delegate.has_value()) << (delegate ? "" : delegate.error().to_string());
    RateLimitedIndexOutput output(std::make_shared<SimpleRateLimiter>(0.0), std::move(delegate.value()));

    EXPECT_EQ(output.get_name(), "limited");
    ASSERT_TRUE(output.write_byte(0x01).has_value());
    ASSERT_TRUE(output.write_short(0x0203).has_value());
    ASSERT_TRUE(output.write_int(0x04050607).has_value());
    ASSERT_TRUE(output.write_long(0x08090A0B0C0D0E0FLL).has_value());
    ASSERT_TRUE(output.write_string("abc").has_value());

    // 1 + 2 + 4 + 8 + (1 length + 3 payload).
    auto pointer = output.get_file_pointer();
    ASSERT_TRUE(pointer.has_value());
    EXPECT_EQ(pointer.value(), 19U);
    EXPECT_EQ(output.get_checksum(), expected_checksum);
    EXPECT_NE(output.to_string().find("RateLimitedIndexOutput("), std::string::npos);

    ASSERT_TRUE(output.close().has_value());

    // The bytes themselves are the ones the delegate would have written.
    auto repeated = directory.create_output("plain2", io_context_default());
    ASSERT_TRUE(repeated.has_value());
    ASSERT_TRUE(repeated.value()->write_byte(0x01).has_value());
    ASSERT_TRUE(repeated.value()->write_short(0x0203).has_value());
    ASSERT_TRUE(repeated.value()->write_int(0x04050607).has_value());
    ASSERT_TRUE(repeated.value()->write_long(0x08090A0B0C0D0E0FLL).has_value());
    ASSERT_TRUE(repeated.value()->write_string("abc").has_value());
    ASSERT_TRUE(repeated.value()->close().has_value());
    EXPECT_EQ(read_all(directory, "limited"), read_all(directory, "plain2"));
}

TEST(RateLimitedIndexOutputTest, ReportsOnlyAfterTheThresholdIsExceeded) {
    ByteBuffersDirectory directory;
    auto limiter = std::make_shared<RecordingRateLimiter>(10);

    auto delegate = directory.create_output("f", io_context_default());
    ASSERT_TRUE(delegate.has_value());
    RateLimitedIndexOutput output(limiter, std::move(delegate.value()));

    ASSERT_TRUE(write_filler(output, 5).has_value());
    EXPECT_TRUE(limiter->reported.empty());

    // 5 + 6 = 11 > 10: the accumulated count is reported, then reset.
    ASSERT_TRUE(write_filler(output, 6).has_value());
    EXPECT_EQ(limiter->reported, (std::vector<int64_t>{11}));

    ASSERT_TRUE(write_filler(output, 3).has_value());
    EXPECT_EQ(limiter->reported.size(), 1U);

    ASSERT_TRUE(write_filler(output, 8).has_value());
    EXPECT_EQ(limiter->reported, (std::vector<int64_t>{11, 11}));

    ASSERT_TRUE(output.close().has_value());
}

TEST(RateLimitedIndexOutputTest, ExactlyTheThresholdDoesNotPause) {
    ByteBuffersDirectory directory;
    auto limiter = std::make_shared<RecordingRateLimiter>(10);

    auto delegate = directory.create_output("f", io_context_default());
    ASSERT_TRUE(delegate.has_value());
    RateLimitedIndexOutput output(limiter, std::move(delegate.value()));

    ASSERT_TRUE(write_filler(output, 10).has_value());
    EXPECT_TRUE(limiter->reported.empty()) << "the reference pauses only when the count is strictly greater";

    ASSERT_TRUE(write_filler(output, 1).has_value());
    EXPECT_EQ(limiter->reported, (std::vector<int64_t>{11}));

    ASSERT_TRUE(output.close().has_value());
}

TEST(RateLimitedIndexOutputTest, PicksUpANewThresholdAfterPausing) {
    ByteBuffersDirectory directory;
    auto limiter = std::make_shared<RecordingRateLimiter>(10);
    limiter->next_threshold_ = 100;

    auto delegate = directory.create_output("f", io_context_default());
    ASSERT_TRUE(delegate.has_value());
    RateLimitedIndexOutput output(limiter, std::move(delegate.value()));

    ASSERT_TRUE(write_filler(output, 11).has_value());
    EXPECT_EQ(limiter->reported, (std::vector<int64_t>{11}));

    // The new threshold (100) is cached by the wrapper.
    ASSERT_TRUE(write_filler(output, 50).has_value());
    EXPECT_EQ(limiter->reported.size(), 1U);

    ASSERT_TRUE(write_filler(output, 51).has_value());
    EXPECT_EQ(limiter->reported, (std::vector<int64_t>{11, 101}));

    ASSERT_TRUE(output.close().has_value());
}

TEST(RateLimitedIndexOutputTest, CountsTheWidthOfEveryPrimitive) {
    ByteBuffersDirectory directory;
    auto limiter = std::make_shared<RecordingRateLimiter>(1000);

    auto delegate = directory.create_output("f", io_context_default());
    ASSERT_TRUE(delegate.has_value());
    RateLimitedIndexOutput output(limiter, std::move(delegate.value()));

    ASSERT_TRUE(output.write_byte(1).has_value());        // 1
    ASSERT_TRUE(output.write_short(2).has_value());       // 2
    ASSERT_TRUE(output.write_int(3).has_value());         // 4
    ASSERT_TRUE(output.write_long(4).has_value());        // 8
    ASSERT_TRUE(output.write_vint(300).has_value());      // 2, through write_byte
    EXPECT_TRUE(limiter->reported.empty());

    // 17 + 986 = 1003 > 1000.
    ASSERT_TRUE(write_filler(output, 986).has_value());
    EXPECT_EQ(limiter->reported, (std::vector<int64_t>{1003}));

    ASSERT_TRUE(output.close().has_value());
}

TEST(RateLimitedIndexOutputTest, AlignedPaddingComesFromTheDelegate) {
    ByteBuffersDirectory directory;
    auto limiter = std::make_shared<RecordingRateLimiter>(1000);

    auto delegate = directory.create_output("f", io_context_default());
    ASSERT_TRUE(delegate.has_value());
    RateLimitedIndexOutput output(limiter, std::move(delegate.value()));

    ASSERT_TRUE(write_filler(output, 3).has_value());
    auto aligned = output.align_file_pointer(8);
    ASSERT_TRUE(aligned.has_value());

    // 3 -> 8, and the padding is written by the delegate so it is not counted
    // by the wrapper (the reference implementation behaves the same way).
    EXPECT_EQ(aligned.value(), 8U);
    auto pointer = output.get_file_pointer();
    ASSERT_TRUE(pointer.has_value());
    EXPECT_EQ(pointer.value(), 8U);
    EXPECT_TRUE(limiter->reported.empty());

    ASSERT_TRUE(output.close().has_value());
    EXPECT_EQ(read_all(directory, "f").size(), 8U);
}

TEST(RateLimitedIndexOutputTest, RealLimiterPausesOnLargeWrites) {
    ByteBuffersDirectory directory;
    // 1 MB/s: the threshold is 5ms worth of bytes, ~5242.
    auto limiter = std::make_shared<SimpleRateLimiter>(1.0);

    auto delegate = directory.create_output("f", io_context_default());
    ASSERT_TRUE(delegate.has_value());
    RateLimitedIndexOutput output(limiter, std::move(delegate.value()));

    const auto start = std::chrono::steady_clock::now();
    // ~20000 bytes at 1 MB/s is ~19ms of budget.
    ASSERT_TRUE(write_filler(output, 20000).has_value());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5);
    ASSERT_TRUE(output.close().has_value());
}

TEST(RateLimitedDirectoryTest, WrapsTheCreatedOutputs) {
    ByteBuffersDirectory delegate;
    auto limiter = std::make_shared<RecordingRateLimiter>(4);
    RateLimitedDirectory directory(delegate, limiter);

    auto output = directory.create_output("_0.cfs", io_context_default());
    ASSERT_TRUE(output.has_value()) << (output ? "" : output.error().to_string());

    auto *wrapped = dynamic_cast<RateLimitedIndexOutput *>(output.value().get());
    ASSERT_NE(wrapped, nullptr);
    EXPECT_EQ(&wrapped->rate_limiter(), limiter.get());

    ASSERT_TRUE(write_filler(*output.value(), 5).has_value());
    EXPECT_EQ(limiter->reported, (std::vector<int64_t>{5}));

    ASSERT_TRUE(output.value()->close().has_value());
    auto length = delegate.file_length("_0.cfs");
    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), 5U);

    EXPECT_EQ(directory.to_string(), "RateLimitedDirectory(ByteBuffersDirectory)");
    EXPECT_FALSE(directory.is_fs_directory());
}

TEST(RateLimitedDirectoryTest, DoesNotWrapTemporaryOutputs) {
    ByteBuffersDirectory delegate;
    auto limiter = std::make_shared<RecordingRateLimiter>(1);
    RateLimitedDirectory directory(delegate, limiter);

    auto temp = directory.create_temp_output("seg", ".tmp", io_context_default());
    ASSERT_TRUE(temp.has_value()) << (temp ? "" : temp.error().to_string());
    EXPECT_EQ(dynamic_cast<RateLimitedIndexOutput *>(temp.value().output.get()), nullptr);

    ASSERT_TRUE(write_filler(*temp.value().output, 5).has_value());
    EXPECT_TRUE(limiter->reported.empty()) << "temporary outputs are not rate limited";

    ASSERT_TRUE(temp.value().output->close().has_value());
    auto names = delegate.list_all();
    ASSERT_TRUE(names.has_value());
    ASSERT_EQ(names.value().size(), 1U);
    EXPECT_EQ(names.value().front(), temp.value().name);
}

TEST(RateLimitedDirectoryTest, SharesOneLimiterBetweenOutputs) {
    ByteBuffersDirectory delegate;
    auto limiter = std::make_shared<RecordingRateLimiter>(2);
    RateLimitedDirectory directory(delegate, limiter);

    for (const std::string_view name : {"a", "b"}) {
        auto output = directory.create_output(name, io_context_default());
        ASSERT_TRUE(output.has_value());
        ASSERT_TRUE(write_filler(*output.value(), 3).has_value());
        ASSERT_TRUE(output.value()->close().has_value());
    }

    EXPECT_EQ(limiter->reported, (std::vector<int64_t>{3, 3}));
}

TEST(RateLimitedDirectoryTest, ForwardsTheDirectoryOperations) {
    ByteBuffersDirectory delegate;
    auto limiter = std::make_shared<SimpleRateLimiter>(0.0);
    RateLimitedDirectory directory(delegate, limiter);

    {
        auto output = directory.create_output("a", io_context_default());
        ASSERT_TRUE(output.has_value());
        ASSERT_TRUE(write_filler(*output.value(), 4, 0x11).has_value());
        ASSERT_TRUE(output.value()->close().has_value());
    }

    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(names.value(), (std::vector<std::string>{"a"}));

    ASSERT_TRUE(directory.rename("a", "b").has_value());
    auto renamed = directory.list_all();
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(renamed.value(), (std::vector<std::string>{"b"}));

    auto length = directory.file_length("b");
    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), 4U);

    std::vector<uint8_t> expected(4, 0x11);
    EXPECT_EQ(read_all(directory, "b"), expected);

    ASSERT_TRUE(directory.sync(std::vector<std::string>{"b"}).has_value());
    ASSERT_TRUE(directory.sync_metadata().has_value());

    ASSERT_TRUE(directory.delete_file("b").has_value());
    auto empty = directory.list_all();
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty.value().empty());
}
