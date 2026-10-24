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

module lucene_core:verifying_lock_factory.impl;

import :verifying_lock_factory;

import std;

import :error;
import :lock;
import :lock_factory;
import :types;

namespace lucene {

// ---------------- LockVerifyProtocol ----------------

LockVerifyProtocol::LockVerifyProtocol(std::shared_ptr<LockVerifyChannel> channel) : channel_(std::move(channel)) {}

VoidResult LockVerifyProtocol::verify(uint8_t message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto written = channel_->write_byte(message); !written.has_value()) {
        return written;
    }
    if (auto flushed = channel_->flush(); !flushed.has_value()) {
        return flushed;
    }
    auto response = channel_->read_byte();
    if (!response.has_value()) {
        return std::unexpected(std::move(response.error()));
    }
    if (!response.value().has_value()) {
        return err(LuceneError::illegal_state("Lock server died because of locking error."));
    }
    if (response.value().value() != message) {
        return err(LuceneError::io("Protocol violation."));
    }
    return {};
}

// ---------------- CheckedLock ----------------

CheckedLock::CheckedLock(std::unique_ptr<Lock> lock, std::shared_ptr<LockVerifyProtocol> protocol)
    : lock_(std::move(lock)), protocol_(std::move(protocol)) {}

Result<std::unique_ptr<CheckedLock>> CheckedLock::create(std::unique_ptr<Lock> lock,
                                                         std::shared_ptr<LockVerifyProtocol> protocol) {
    std::unique_ptr<CheckedLock> checked(new CheckedLock(std::move(lock), std::move(protocol)));
    if (auto verified = checked->protocol_->verify(MSG_LOCK_ACQUIRED); !verified.has_value()) {
        // The reference drops the lock, which closes it; release the local lock
        // so a refused acquisition does not keep it.
        (void)checked->lock_->close();
        return std::unexpected(std::move(verified.error()));
    }
    return checked;
}

VoidResult CheckedLock::ensure_valid() const { return lock_->ensure_valid(); }

VoidResult CheckedLock::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;
    // Report the release first, but always close the local lock: a dead server
    // must not leave the file locked.
    VoidResult released = lock_->ensure_valid();
    if (released.has_value()) {
        released = protocol_->verify(MSG_LOCK_RELEASED);
    }
    auto closed = lock_->close();
    if (!released.has_value()) {
        return released;
    }
    return closed;
}

std::string CheckedLock::to_string() const { return lock_->to_string(); }

// ---------------- VerifyingLockFactory ----------------

VerifyingLockFactory::VerifyingLockFactory(std::unique_ptr<LockFactory> delegate,
                                           std::shared_ptr<LockVerifyProtocol> protocol)
    : delegate_(std::move(delegate)), protocol_(std::move(protocol)) {}

VerifyingLockFactory::VerifyingLockFactory(std::unique_ptr<LockFactory> delegate,
                                           std::shared_ptr<LockVerifyChannel> channel)
    : delegate_(std::move(delegate)), protocol_(std::make_shared<LockVerifyProtocol>(std::move(channel))) {}

Result<std::unique_ptr<Lock>> VerifyingLockFactory::obtain_lock(std::string_view lock_name) {
    auto lock = delegate_->obtain_lock(lock_name);
    if (!lock.has_value()) {
        return std::unexpected(std::move(lock.error()));
    }
    auto checked = CheckedLock::create(std::move(lock.value()), protocol_);
    if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return std::unique_ptr<Lock>(std::move(checked.value()));
}

VoidResult VerifyingLockFactory::clear_lock(std::string_view lock_name) { return delegate_->clear_lock(lock_name); }

} // namespace lucene
