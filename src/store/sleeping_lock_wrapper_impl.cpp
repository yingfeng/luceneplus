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

module lucene_core:sleeping_lock_wrapper.impl;

import :sleeping_lock_wrapper;

import std;

import :directory;
import :error;
import :types;

namespace lucene {

SleepingLockWrapper::SleepingLockWrapper(std::shared_ptr<Directory> delegate, int64_t lock_wait_timeout,
                                         int64_t poll_interval)
        : delegate_(std::move(delegate)), lock_wait_timeout_(lock_wait_timeout), poll_interval_(poll_interval) {}

Result<std::unique_ptr<SleepingLockWrapper>> SleepingLockWrapper::create(std::shared_ptr<Directory> delegate,
                                                                        int64_t lock_wait_timeout) {
    return create(std::move(delegate), lock_wait_timeout, DEFAULT_POLL_INTERVAL);
}

Result<std::unique_ptr<SleepingLockWrapper>> SleepingLockWrapper::create(std::shared_ptr<Directory> delegate,
                                                                        int64_t lock_wait_timeout,
                                                                        int64_t poll_interval) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the directory must not be null"));
    }
    if (lock_wait_timeout < 0 && lock_wait_timeout != LOCK_OBTAIN_WAIT_FOREVER) {
        return err(LuceneError::illegal_argument(
                "lockWaitTimeout should be LOCK_OBTAIN_WAIT_FOREVER or a non-negative number (got " +
                std::to_string(lock_wait_timeout) + ")"));
    }
    if (poll_interval < 0) {
        return err(LuceneError::illegal_argument("pollInterval must be a non-negative number (got " +
                                                 std::to_string(poll_interval) + ")"));
    }
    return std::unique_ptr<SleepingLockWrapper>(
            new SleepingLockWrapper(std::move(delegate), lock_wait_timeout, poll_interval));
}

Result<std::vector<std::string>> SleepingLockWrapper::list_all() const { return delegate_->list_all(); }

VoidResult SleepingLockWrapper::delete_file(std::string_view name) { return delegate_->delete_file(name); }

Result<size_t> SleepingLockWrapper::file_length(std::string_view name) const {
    return delegate_->file_length(name);
}

Result<std::unique_ptr<IndexOutput>> SleepingLockWrapper::create_output(std::string_view name,
                                                                       const IOContext &context) {
    return delegate_->create_output(name, context);
}

Result<CreatedOutput> SleepingLockWrapper::create_temp_output(std::string_view prefix, std::string_view suffix,
                                                             const IOContext &context) {
    return delegate_->create_temp_output(prefix, suffix, context);
}

VoidResult SleepingLockWrapper::sync(std::span<const std::string> names) { return delegate_->sync(names); }

VoidResult SleepingLockWrapper::sync_metadata() { return delegate_->sync_metadata(); }

VoidResult SleepingLockWrapper::rename(std::string_view source, std::string_view dest) {
    return delegate_->rename(source, dest);
}

Result<std::unique_ptr<IndexInput>> SleepingLockWrapper::open_input(std::string_view name,
                                                                   const IOContext &context) const {
    return delegate_->open_input(name, context);
}

Result<std::unique_ptr<Lock>> SleepingLockWrapper::obtain_lock(std::string_view name) {
    // With a poll interval of zero the timeout counts attempts instead of
    // milliseconds, which is what the reference implementation does.
    const int64_t max_sleep_count =
            poll_interval_ == 0 ? lock_wait_timeout_ : lock_wait_timeout_ / poll_interval_;

    std::optional<LuceneError> first_failure;
    int64_t sleep_count = 0;
    while (true) {
        auto lock = delegate_->obtain_lock(name);
        if (lock.has_value()) {
            return lock;
        }
        if (lock.error().kind() != ErrorKind::kLockObtainFailed) {
            // A real error: no amount of waiting will fix it.
            return std::unexpected(std::move(lock.error()));
        }
        if (!first_failure.has_value()) {
            first_failure = std::move(lock.error());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_));

        const bool should_continue = sleep_count < max_sleep_count || lock_wait_timeout_ == LOCK_OBTAIN_WAIT_FOREVER;
        ++sleep_count;
        if (!should_continue) {
            break;
        }
    }

    return std::unexpected(std::move(first_failure.value()));
}

std::string SleepingLockWrapper::to_string() const {
    return "SleepingLockWrapper(" + delegate_->to_string() + ")";
}

} // namespace lucene
