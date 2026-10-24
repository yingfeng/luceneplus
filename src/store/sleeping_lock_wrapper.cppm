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

export module lucene_core:sleeping_lock_wrapper;

import :types;
import std;

import :directory;
import :error;

namespace lucene {

/// A [`Directory`] that keeps trying to obtain its write lock instead of failing
/// straight away.
///
/// Port of `SleepingLockWrapper` (`core/store/sleeping_lock_wrapper.rs`).
///
/// Every operation is delegated; only [`obtain_lock`] differs: a
/// [`kLockObtainFailed`](ErrorKind::kLockObtainFailed) error makes it sleep for
/// `poll_interval` milliseconds and try again, until `lock_wait_timeout`
/// milliseconds have passed or the lock is obtained.  Any other error is reported
/// immediately, and the first lock failure is what the caller sees once the
/// timeout is exhausted.
///
/// **Deviation:** the reference implementation owns its directory by value; the
/// port shares it, so the caller decides when it closes.
export class SleepingLockWrapper final : public Directory {
public:
    /// Wait for the lock as long as it takes.
    static constexpr int64_t LOCK_OBTAIN_WAIT_FOREVER = -1;
    /// How long to sleep between two attempts, by default.
    static constexpr int64_t DEFAULT_POLL_INTERVAL = 1000;

    /// Wraps `delegate`, waiting up to `lock_wait_timeout` milliseconds.
    [[nodiscard]] static Result<std::unique_ptr<SleepingLockWrapper>> create(std::shared_ptr<Directory> delegate,
                                                                            int64_t lock_wait_timeout);
    /// The same, with an explicit poll interval.
    [[nodiscard]] static Result<std::unique_ptr<SleepingLockWrapper>> create(std::shared_ptr<Directory> delegate,
                                                                            int64_t lock_wait_timeout,
                                                                            int64_t poll_interval);

    [[nodiscard]] Directory &delegate() const noexcept { return *delegate_; }
    [[nodiscard]] int64_t lock_wait_timeout() const noexcept { return lock_wait_timeout_; }
    [[nodiscard]] int64_t poll_interval() const noexcept { return poll_interval_; }

    // ---- Directory ----

    [[nodiscard]] Result<std::vector<std::string>> list_all() const override;
    [[nodiscard]] VoidResult delete_file(std::string_view name) override;
    [[nodiscard]] Result<size_t> file_length(std::string_view name) const override;
    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                     const IOContext &context) override;
    [[nodiscard]] Result<CreatedOutput> create_temp_output(std::string_view prefix, std::string_view suffix,
                                                           const IOContext &context) override;
    [[nodiscard]] VoidResult sync(std::span<const std::string> names) override;
    [[nodiscard]] VoidResult sync_metadata() override;
    [[nodiscard]] VoidResult rename(std::string_view source, std::string_view dest) override;
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                 const IOContext &context) const override;

    /// Retries the delegate's lock until it succeeds or the timeout expires.
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override;

    [[nodiscard]] bool is_fs_directory() const override { return delegate_->is_fs_directory(); }
    [[nodiscard]] std::string to_string() const override;

private:
    SleepingLockWrapper(std::shared_ptr<Directory> delegate, int64_t lock_wait_timeout, int64_t poll_interval);

    std::shared_ptr<Directory> delegate_;
    int64_t lock_wait_timeout_;
    int64_t poll_interval_;
};

} // namespace lucene
