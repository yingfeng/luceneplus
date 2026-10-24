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

export module lucene_core:lock_validating_directory_wrapper;

import :types;
import std;

import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :lock;

namespace lucene {

/// A [`Directory`] that re-checks the write lock before every destructive
/// operation.
///
/// Port of `LockValidatingDirectoryWrapper`.
///
/// Losing the write lock means another process took the index over, so touching
/// its files from here on would corrupt it.  This wrapper turns that into an
/// error before the damage happens: the writer hands the lock to the wrapper
/// while it writes, and every `delete_file` / `create_output` / `sync` /
/// `rename` / `copy_from` first asks the lock whether it is still valid.
///
/// Reads are not checked: a stale reader sees a consistent snapshot anyway.
export class LockValidatingDirectoryWrapper final : public Directory {
public:
    using Directory::create_output;
    using Directory::open_input;

    /// Wraps `delegate`; `write_lock` must outlive the wrapper.
    LockValidatingDirectoryWrapper(Directory &delegate, Lock &write_lock);

    // ---- checked operations ----

    [[nodiscard]] VoidResult delete_file(std::string_view name) override;
    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                    const IOContext &context) override;
    [[nodiscard]] VoidResult sync(std::span<const std::string> names) override;
    [[nodiscard]] VoidResult sync_metadata() override;
    [[nodiscard]] VoidResult rename(std::string_view source, std::string_view dest) override;
    [[nodiscard]] VoidResult copy_from(const Directory &from, std::string_view src, std::string_view dest,
                                       const IOContext &context) override;

    // ---- unchecked operations ----

    [[nodiscard]] Result<std::vector<std::string>> list_all() const override;
    [[nodiscard]] Result<size_t> file_length(std::string_view name) const override;
    [[nodiscard]] Result<CreatedOutput> create_temp_output(std::string_view prefix, std::string_view suffix,
                                                          const IOContext &context) override;
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                const IOContext &context) const override;
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override;
    [[nodiscard]] bool is_fs_directory() const override { return delegate_.is_fs_directory(); }
    [[nodiscard]] std::string to_string() const override;

    // ---- introspection ----

    [[nodiscard]] Directory &delegate() noexcept { return delegate_; }
    [[nodiscard]] const Directory &delegate() const noexcept { return delegate_; }

private:
    Directory &delegate_;
    Lock &write_lock_;
};

} // namespace lucene
