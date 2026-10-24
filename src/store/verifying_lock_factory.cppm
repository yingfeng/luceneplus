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

export module lucene_core:verifying_lock_factory;

import :types;
import std;

import :error;
import :lock;
import :lock_factory;

namespace lucene {

/// Protocol byte telling the verification server that a lock was released.
export inline constexpr uint8_t MSG_LOCK_RELEASED = 0;
/// Protocol byte telling the verification server that a lock was acquired.
export inline constexpr uint8_t MSG_LOCK_ACQUIRED = 1;

/// The byte stream between a [`VerifyingLockFactory`] and its lock server.
///
/// Deviation from the reference implementation (C++ mechanics only): the
/// reference is generic over `Read`/`Write`, which in practice are the two
/// halves of one socket.  Here they are one interface, so the object can be a
/// `std::shared_ptr` and stay type-erased.
export class LockVerifyChannel {
public:
    virtual ~LockVerifyChannel() = default;

    /// Sends one protocol byte.
    [[nodiscard]] virtual VoidResult write_byte(uint8_t b) = 0;
    /// Flushes what has been sent, so the server can answer.
    [[nodiscard]] virtual VoidResult flush() = 0;
    /// Reads one protocol byte; `std::nullopt` means the server closed the
    /// connection (it considers the locking state corrupt).
    [[nodiscard]] virtual Result<std::optional<uint8_t>> read_byte() = 0;
};

/// One request/response exchange with the lock server, serialized.
///
/// The server echoes the message back when it agrees.  A silent answer means it
/// died because of a locking error, and a different answer is a protocol
/// violation: in both cases the caller must not touch the index.
export class LockVerifyProtocol {
public:
    explicit LockVerifyProtocol(std::shared_ptr<LockVerifyChannel> channel);

    /// Sends `message` and waits for the echo.
    [[nodiscard]] VoidResult verify(uint8_t message);

    [[nodiscard]] const LockVerifyChannel &channel() const noexcept { return *channel_; }

private:
    std::mutex mutex_;
    std::shared_ptr<LockVerifyChannel> channel_;
};

/// A [`Lock`] that also verifies its acquisition and release on the server.
///
/// Port of `CheckedLock`; every close reports [`MSG_LOCK_RELEASED`], so a
/// failure to reach the server releases the local lock but is still reported.
export class CheckedLock final : public Lock {
public:
    /// Verifies the acquisition of `lock` on `protocol`.
    ///
    /// If the server refuses, the local lock is released (the reference drops
    /// it, which closes it) and the verification failure is returned.
    [[nodiscard]] static Result<std::unique_ptr<CheckedLock>> create(std::unique_ptr<Lock> lock,
                                                                     std::shared_ptr<LockVerifyProtocol> protocol);

    [[nodiscard]] VoidResult ensure_valid() const override;

    /// Reports the release to the server, then closes the local lock.
    ///
    /// A verification failure wins over a close failure, but the local lock is
    /// closed either way.  Closing again is a no-op.
    [[nodiscard]] VoidResult close() override;

    [[nodiscard]] std::string to_string() const override;

    /// The wrapped lock.
    [[nodiscard]] Lock &delegate() noexcept { return *lock_; }
    [[nodiscard]] const Lock &delegate() const noexcept { return *lock_; }

private:
    CheckedLock(std::unique_ptr<Lock> lock, std::shared_ptr<LockVerifyProtocol> protocol);

    std::unique_ptr<Lock> lock_;
    std::shared_ptr<LockVerifyProtocol> protocol_;
    bool closed_{false};
};

/// A [`LockFactory`] whose locks are verified against an external lock server.
///
/// Port of `VerifyingLockFactory`.  The wrapped factory does the real locking;
/// every lock it hands out is checked with the server, which is how Lucene's own
/// tests detect a process that writes to an index it no longer owns.
export class VerifyingLockFactory final : public LockFactory {
public:
    /// Takes ownership of `delegate`; `protocol` is shared by all the locks of
    /// this factory, so exchanges never interleave.
    VerifyingLockFactory(std::unique_ptr<LockFactory> delegate, std::shared_ptr<LockVerifyProtocol> protocol);

    /// Convenience overload: builds the protocol around `channel`.
    VerifyingLockFactory(std::unique_ptr<LockFactory> delegate, std::shared_ptr<LockVerifyChannel> channel);

    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view lock_name) override;
    [[nodiscard]] VoidResult clear_lock(std::string_view lock_name) override;

    [[nodiscard]] std::string to_string() const { return "VerifyingLockFactory"; }

private:
    std::unique_ptr<LockFactory> delegate_;
    std::shared_ptr<LockVerifyProtocol> protocol_;
};

} // namespace lucene
