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

export module lucene_core:tracking_directory_wrapper;

import :types;
import std;

import :directory;
import :error;

namespace lucene {

/// A [`Directory`] that remembers which files were created through it.
///
/// Port of `TrackingDirectoryWrapper` (`core/store/tracking_directory_wrapper.rs`).
///
/// The writer wraps its directory in one of these while it flushes a segment, so
/// it learns the exact file list of the new segment without having to diff the
/// directory.  Deleting a file through the wrapper forgets it.
export class TrackingDirectoryWrapper final : public Directory {
public:
    /// Wraps `delegate`, which must outlive the wrapper.
    explicit TrackingDirectoryWrapper(Directory &delegate);

    // ---- tracking ----

    /// The files created so far, sorted.
    [[nodiscard]] std::set<std::string> created_files() const;
    /// Whether any file was created so far.
    [[nodiscard]] bool has_created_files() const;
    /// The files created so far, leaving the wrapper empty.
    [[nodiscard]] std::set<std::string> take_created_files();
    /// Forgets the files created so far.
    void clear_created_files();

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
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override;
    [[nodiscard]] bool is_fs_directory() const override { return delegate_.is_fs_directory(); }
    [[nodiscard]] std::string to_string() const override;

private:
    Directory &delegate_;
    /// Mutable because creating a file is not a `const` operation on the
    /// delegate, but the bookkeeping has to work from both paths.
    mutable std::mutex mutex_;
    mutable std::set<std::string> created_;
};

} // namespace lucene
