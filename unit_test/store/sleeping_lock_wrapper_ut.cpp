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

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.sleeping_lock_wrapper;

import lucene_core;

using namespace lucene;

namespace {

/// A directory whose lock fails `failures` times before it is handed out.
class FlakyDirectory final : public ByteBuffersDirectory {
public:
    explicit FlakyDirectory(int32_t failures) : failures_(failures) {}

    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override {
        ++attempts_;
        if (attempts_ <= failures_) {
            return err(LuceneError::lock_obtain_failed("the lock is held by another process"));
        }
        return ByteBuffersDirectory::obtain_lock(name);
    }

    [[nodiscard]] int32_t attempts() const { return attempts_; }

private:
    int32_t failures_;
    int32_t attempts_{0};
};

/// A directory whose lock always fails with an error that waiting cannot fix.
class BrokenDirectory final : public ByteBuffersDirectory {
public:
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view /*name*/) override {
        ++attempts_;
        return err(LuceneError::illegal_state("the directory is broken"));
    }

    [[nodiscard]] int32_t attempts() const { return attempts_; }

private:
    int32_t attempts_{0};
};

} // namespace

TEST(SleepingLockWrapperTest, ValidatesTheTimeout) {
    auto delegate = std::make_shared<ByteBuffersDirectory>();

    auto negative = SleepingLockWrapper::create(delegate, -2);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);

    auto negative_poll = SleepingLockWrapper::create(delegate, 1000, -1);
    ASSERT_FALSE(negative_poll.has_value());
    EXPECT_EQ(negative_poll.error().kind(), ErrorKind::kIllegalArgument);

    auto null_directory = SleepingLockWrapper::create(nullptr, 1000);
    ASSERT_FALSE(null_directory.has_value());
    EXPECT_EQ(null_directory.error().kind(), ErrorKind::kIllegalArgument);

    // Waiting forever is allowed, and is the only negative timeout that is.
    auto forever = SleepingLockWrapper::create(delegate, SleepingLockWrapper::LOCK_OBTAIN_WAIT_FOREVER);
    ASSERT_TRUE(forever.has_value()) << (forever ? "" : forever.error().to_string());
    EXPECT_EQ(forever.value()->lock_wait_timeout(), SleepingLockWrapper::LOCK_OBTAIN_WAIT_FOREVER);
    EXPECT_EQ(forever.value()->poll_interval(), SleepingLockWrapper::DEFAULT_POLL_INTERVAL);
}

TEST(SleepingLockWrapperTest, RetriesUntilTheLockIsAvailable) {
    auto flaky = std::make_shared<FlakyDirectory>(2);
    auto wrapper = SleepingLockWrapper::create(flaky, 1000, 1);
    ASSERT_TRUE(wrapper.has_value()) << (wrapper ? "" : wrapper.error().to_string());

    auto lock = wrapper.value()->obtain_lock("write.lock");
    ASSERT_TRUE(lock.has_value()) << (lock ? "" : lock.error().to_string());
    EXPECT_EQ(flaky->attempts(), 3);

    // The wrapper delegates everything else.
    EXPECT_EQ(wrapper.value()->to_string(), "SleepingLockWrapper(" + flaky->to_string() + ")");
    auto names = wrapper.value()->list_all();
    EXPECT_TRUE(names.has_value());
}

TEST(SleepingLockWrapperTest, GivesUpAfterTheTimeout) {
    // The third attempt would succeed, but the timeout only allows two.
    auto flaky = std::make_shared<FlakyDirectory>(100);
    auto wrapper = SleepingLockWrapper::create(flaky, 2, 1);
    ASSERT_TRUE(wrapper.has_value());

    auto lock = wrapper.value()->obtain_lock("write.lock");
    ASSERT_FALSE(lock.has_value());
    EXPECT_EQ(lock.error().kind(), ErrorKind::kLockObtainFailed);
    // The first attempt plus the attempts the timeout allows.
    EXPECT_EQ(flaky->attempts(), 3);
}

TEST(SleepingLockWrapperTest, ReportsRealErrorsWithoutRetrying) {
    // An unrelated failure is not retried, even with an unlimited timeout.
    auto broken = std::make_shared<BrokenDirectory>();
    auto wrapper = SleepingLockWrapper::create(broken, SleepingLockWrapper::LOCK_OBTAIN_WAIT_FOREVER, 0);
    ASSERT_TRUE(wrapper.has_value());

    auto lock = wrapper.value()->obtain_lock("write.lock");
    ASSERT_FALSE(lock.has_value());
    EXPECT_EQ(lock.error().kind(), ErrorKind::kIllegalState);
    EXPECT_EQ(broken->attempts(), 1);
}

TEST(SleepingLockWrapperTest, DoesNotRetryWhenTheLockIsFree) {
    auto free_directory = std::make_shared<FlakyDirectory>(0);
    auto wrapper = SleepingLockWrapper::create(free_directory, 1000, 0);
    ASSERT_TRUE(wrapper.has_value());

    auto lock = wrapper.value()->obtain_lock("write.lock");
    ASSERT_TRUE(lock.has_value()) << (lock ? "" : lock.error().to_string());
    EXPECT_EQ(free_directory->attempts(), 1);
}
