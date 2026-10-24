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
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.verifying_lock_factory;

import lucene_core;

using namespace lucene;

namespace {

/// A lock server connection whose answers the test scripts.
class FakeChannel final : public LockVerifyChannel {
public:
    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        if (fail_writes) {
            return std::unexpected(LuceneError::io("write failed"));
        }
        written.push_back(b);
        return {};
    }

    [[nodiscard]] VoidResult flush() override {
        if (fail_flush) {
            return std::unexpected(LuceneError::io("flush failed"));
        }
        ++flushes;
        return {};
    }

    [[nodiscard]] Result<std::optional<uint8_t>> read_byte() override {
        if (responses.empty()) {
            // Server closed the connection without answering.
            return std::optional<uint8_t>{};
        }
        const std::optional<uint8_t> next = responses.front();
        responses.pop_front();
        return next;
    }

    std::vector<uint8_t> written;
    std::deque<std::optional<uint8_t>> responses;
    int flushes{0};
    bool fail_writes{false};
    bool fail_flush{false};
};

/// A lock whose close is counted, so the factory's bookkeeping is observable.
class TrackingLock final : public Lock {
public:
    explicit TrackingLock(std::shared_ptr<int> closes) : closes_(std::move(closes)) {}

    [[nodiscard]] VoidResult ensure_valid() const override {
        if (!valid) {
            return std::unexpected(LuceneError::illegal_state("lock is no longer valid"));
        }
        return {};
    }

    [[nodiscard]] VoidResult close() override {
        ++*closes_;
        return {};
    }

    [[nodiscard]] std::string to_string() const override { return "TrackingLock"; }

    bool valid{true};

private:
    std::shared_ptr<int> closes_;
};

/// Returns [`TrackingLock`]s, or fails when told to.
class TrackingLockFactory final : public LockFactory {
public:
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view lock_name) override {
        ++obtains;
        obtained_name = std::string(lock_name);
        if (fail_obtain) {
            return std::unexpected(LuceneError::lock_obtain_failed("already held"));
        }
        auto lock = std::make_unique<TrackingLock>(closes);
        last_lock = lock.get();
        return std::unique_ptr<Lock>(std::move(lock));
    }

    [[nodiscard]] VoidResult clear_lock(std::string_view lock_name) override {
        cleared.emplace_back(lock_name);
        return {};
    }

    std::shared_ptr<int> closes = std::make_shared<int>(0);
    std::vector<std::string> cleared;
    std::string obtained_name;
    /// The lock handed out last, so a test can invalidate it.
    TrackingLock *last_lock{nullptr};
    int obtains{0};
    bool fail_obtain{false};
};

} // namespace

TEST(VerifyingLockFactoryTest, VerifiesTheAcquisition) {
    auto channel = std::make_shared<FakeChannel>();
    channel->responses.push_back(MSG_LOCK_ACQUIRED);
    auto delegate = std::make_unique<TrackingLockFactory>();
    auto *delegate_ptr = delegate.get();
    VerifyingLockFactory factory(std::move(delegate), channel);

    auto lock = factory.obtain_lock("write.lock");
    ASSERT_TRUE(lock.has_value()) << (lock ? "" : lock.error().to_string());

    EXPECT_EQ(delegate_ptr->obtains, 1);
    EXPECT_EQ(delegate_ptr->obtained_name, "write.lock");
    EXPECT_EQ(channel->written, (std::vector<uint8_t>{MSG_LOCK_ACQUIRED}));
    EXPECT_EQ(channel->flushes, 1);
    EXPECT_EQ(lock.value()->to_string(), "TrackingLock");
    EXPECT_TRUE(lock.value()->ensure_valid().has_value());

    channel->responses.push_back(MSG_LOCK_RELEASED);
    ASSERT_TRUE(lock.value()->close().has_value());
}

TEST(VerifyingLockFactoryTest, ReportsADeadServerAndReleasesTheLocalLock) {
    auto channel = std::make_shared<FakeChannel>(); // no response: the server died
    auto delegate = std::make_unique<TrackingLockFactory>();
    auto *delegate_ptr = delegate.get();
    VerifyingLockFactory factory(std::move(delegate), channel);

    auto lock = factory.obtain_lock("write.lock");
    ASSERT_FALSE(lock.has_value());
    EXPECT_EQ(lock.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(lock.error().message().find("Lock server died"), std::string::npos);
    EXPECT_EQ(*delegate_ptr->closes, 1) << "a refused acquisition must not keep the local lock";
}

TEST(VerifyingLockFactoryTest, ReportsAProtocolViolation) {
    auto channel = std::make_shared<FakeChannel>();
    channel->responses.push_back(MSG_LOCK_RELEASED); // wrong echo
    auto delegate = std::make_unique<TrackingLockFactory>();
    auto *delegate_ptr = delegate.get();
    VerifyingLockFactory factory(std::move(delegate), channel);

    auto lock = factory.obtain_lock("write.lock");
    ASSERT_FALSE(lock.has_value());
    EXPECT_EQ(lock.error().kind(), ErrorKind::kIo);
    EXPECT_EQ(lock.error().message(), "Protocol violation.");
    EXPECT_EQ(*delegate_ptr->closes, 1);
}

TEST(VerifyingLockFactoryTest, ForwardsObtainFailuresWithoutTalkingToTheServer) {
    auto channel = std::make_shared<FakeChannel>();
    channel->responses.push_back(MSG_LOCK_ACQUIRED);
    auto delegate = std::make_unique<TrackingLockFactory>();
    delegate->fail_obtain = true;
    VerifyingLockFactory factory(std::move(delegate), channel);

    auto lock = factory.obtain_lock("write.lock");
    ASSERT_FALSE(lock.has_value());
    EXPECT_EQ(lock.error().kind(), ErrorKind::kLockObtainFailed);
    EXPECT_TRUE(channel->written.empty());
}

TEST(VerifyingLockFactoryTest, VerifiesTheReleaseOnClose) {
    auto channel = std::make_shared<FakeChannel>();
    channel->responses.push_back(MSG_LOCK_ACQUIRED);
    channel->responses.push_back(MSG_LOCK_RELEASED);
    auto delegate = std::make_unique<TrackingLockFactory>();
    auto *delegate_ptr = delegate.get();
    VerifyingLockFactory factory(std::move(delegate), channel);

    auto lock = factory.obtain_lock("write.lock");
    ASSERT_TRUE(lock.has_value()) << (lock ? "" : lock.error().to_string());

    ASSERT_TRUE(lock.value()->close().has_value());
    EXPECT_EQ(channel->written, (std::vector<uint8_t>{MSG_LOCK_ACQUIRED, MSG_LOCK_RELEASED}));
    EXPECT_EQ(*delegate_ptr->closes, 1);

    // Closing again neither talks to the server nor closes the lock twice.
    ASSERT_TRUE(lock.value()->close().has_value());
    EXPECT_EQ(channel->written.size(), 2U);
    EXPECT_EQ(*delegate_ptr->closes, 1);
}

TEST(VerifyingLockFactoryTest, ReleaseFailureStillClosesTheLocalLock) {
    auto channel = std::make_shared<FakeChannel>();
    channel->responses.push_back(MSG_LOCK_ACQUIRED);
    // The release gets no answer.
    auto delegate = std::make_unique<TrackingLockFactory>();
    auto *delegate_ptr = delegate.get();
    VerifyingLockFactory factory(std::move(delegate), channel);

    auto lock = factory.obtain_lock("write.lock");
    ASSERT_TRUE(lock.has_value()) << (lock ? "" : lock.error().to_string());

    auto closed = lock.value()->close();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().kind(), ErrorKind::kIllegalState);
    EXPECT_EQ(*delegate_ptr->closes, 1) << "a dead server must not leave the file locked";
}

TEST(VerifyingLockFactoryTest, AnInvalidLockSkipsTheReleaseVerification) {
    auto channel = std::make_shared<FakeChannel>();
    channel->responses.push_back(MSG_LOCK_ACQUIRED);
    auto delegate = std::make_unique<TrackingLockFactory>();
    auto *delegate_ptr = delegate.get();
    VerifyingLockFactory factory(std::move(delegate), channel);

    auto lock = factory.obtain_lock("write.lock");
    ASSERT_TRUE(lock.has_value()) << (lock ? "" : lock.error().to_string());

    ASSERT_NE(delegate_ptr->last_lock, nullptr);
    delegate_ptr->last_lock->valid = false;

    auto closed = lock.value()->close();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().kind(), ErrorKind::kIllegalState);
    EXPECT_EQ(channel->written, (std::vector<uint8_t>{MSG_LOCK_ACQUIRED})) << "no release is reported";
    EXPECT_EQ(*delegate_ptr->closes, 1);
}

TEST(VerifyingLockFactoryTest, AllLocksShareOneProtocol) {
    auto channel = std::make_shared<FakeChannel>();
    channel->responses.push_back(MSG_LOCK_ACQUIRED);
    channel->responses.push_back(MSG_LOCK_ACQUIRED);
    auto delegate = std::make_unique<TrackingLockFactory>();
    auto *delegate_ptr = delegate.get();
    VerifyingLockFactory factory(std::move(delegate), channel);

    auto first = factory.obtain_lock("write.lock");
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    auto second = factory.obtain_lock("commit.lock");
    ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());

    EXPECT_EQ(delegate_ptr->obtains, 2);
    EXPECT_EQ(channel->written, (std::vector<uint8_t>{MSG_LOCK_ACQUIRED, MSG_LOCK_ACQUIRED}));

    channel->responses.push_back(MSG_LOCK_RELEASED);
    channel->responses.push_back(MSG_LOCK_RELEASED);
    ASSERT_TRUE(first.value()->close().has_value());
    ASSERT_TRUE(second.value()->close().has_value());
    EXPECT_EQ(channel->written,
              (std::vector<uint8_t>{MSG_LOCK_ACQUIRED, MSG_LOCK_ACQUIRED, MSG_LOCK_RELEASED, MSG_LOCK_RELEASED}));
}

TEST(VerifyingLockFactoryTest, ClearLockIsForwarded) {
    auto channel = std::make_shared<FakeChannel>();
    auto delegate = std::make_unique<TrackingLockFactory>();
    auto *delegate_ptr = delegate.get();
    VerifyingLockFactory factory(std::move(delegate), channel);

    ASSERT_TRUE(factory.clear_lock("write.lock").has_value());
    EXPECT_EQ(delegate_ptr->cleared, (std::vector<std::string>{"write.lock"}));
    EXPECT_TRUE(channel->written.empty());
    EXPECT_EQ(factory.to_string(), "VerifyingLockFactory");
}

TEST(VerifyingLockFactoryTest, ChannelFailuresPropagateAndReleaseTheLocalLock) {
    {
        auto channel = std::make_shared<FakeChannel>();
        channel->fail_writes = true;
        channel->responses.push_back(MSG_LOCK_ACQUIRED);
        auto delegate = std::make_unique<TrackingLockFactory>();
        auto *delegate_ptr = delegate.get();
        VerifyingLockFactory factory(std::move(delegate), channel);

        auto lock = factory.obtain_lock("write.lock");
        ASSERT_FALSE(lock.has_value());
        EXPECT_EQ(lock.error().kind(), ErrorKind::kIo);
        EXPECT_EQ(*delegate_ptr->closes, 1);
    }
    {
        auto channel = std::make_shared<FakeChannel>();
        channel->fail_flush = true;
        channel->responses.push_back(MSG_LOCK_ACQUIRED);
        auto delegate = std::make_unique<TrackingLockFactory>();
        auto *delegate_ptr = delegate.get();
        VerifyingLockFactory factory(std::move(delegate), channel);

        auto lock = factory.obtain_lock("write.lock");
        ASSERT_FALSE(lock.has_value());
        EXPECT_EQ(lock.error().kind(), ErrorKind::kIo);
        EXPECT_EQ(*delegate_ptr->closes, 1);
    }
}
