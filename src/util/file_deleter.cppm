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

export module lucene_core:file_deleter;

import :types;
import std;

import :directory;
import :error;

namespace lucene {

/// Kinds of message a [`FileDeleterListener`] receives.
export enum class FileDeleterMsg : uint8_t {
    /// A reference count changed (very chatty; only for debugging).
    kRef = 0,
    /// A file is about to be deleted.
    kFile = 1,
};

/// Receives the diagnostics of a [`FileDeleter`].
///
/// Port of the `Messenger` trait (`util/file_deleter.rs`).
export class FileDeleterListener {
public:
    virtual ~FileDeleterListener() = default;

    [[nodiscard]] virtual VoidResult accept(FileDeleterMsg type, std::string_view message) = 0;
};

/// Tracks the reference counts of a set of index files and deletes them when
/// their count drops to zero.
///
/// Port of `FileDeleter` (`util/file_deleter.rs`).
///
/// **Not thread-safe**: the caller (the writer, through
/// [`IndexFileDeleter`](:index_file_deleter)) serializes the access.
///
/// A file becomes known either through [`init_ref_count`] (count `0`) or
/// through [`inc_ref`] (count `1`).  [`delete_files`] removes `segments_N` files
/// before anything else, so a crash can never leave a commit file pointing at
/// files that are already gone.
export class FileDeleter final {
public:
    /// `directory` must outlive the deleter; `listener` may be null.
    explicit FileDeleter(Directory &directory, std::shared_ptr<FileDeleterListener> listener = nullptr);

    /// Increases the count of every file in `files`.
    [[nodiscard]] VoidResult inc_ref(std::span<const std::string> files);
    /// Increases the count of one file.
    [[nodiscard]] VoidResult inc_ref(std::string_view file);

    /// Decreases the counts and deletes the files that reach zero, even when one
    /// of them fails; the first failure is returned.
    [[nodiscard]] VoidResult dec_ref(std::span<const std::string> files);

    /// Registers `file` with count `0` when it is not known yet.
    void init_ref_count(std::string_view file);

    /// The count of `file`, or `0` when it is not known.
    [[nodiscard]] size_t ref_count(std::string_view file) const;

    /// Every file this deleter knows, referenced or not.
    [[nodiscard]] std::vector<std::string> all_files() const;

    /// Whether `file` is known with a count above zero.
    [[nodiscard]] bool exists(std::string_view file) const;

    /// The known files whose count is zero.
    [[nodiscard]] std::vector<std::string> unrefed_files() const;

    /// Deletes the files of `files` whose count is zero.
    [[nodiscard]] VoidResult delete_files_if_no_ref(std::span<const std::string> files);

    /// Forgets `file` and deletes it unconditionally.
    [[nodiscard]] VoidResult force_delete(std::string_view file);

    /// Deletes `file` when its count is zero.
    [[nodiscard]] VoidResult delete_file_if_no_ref(std::string_view file);

    /// Deletes `files`, `segments_N` first.
    [[nodiscard]] VoidResult delete_files(std::span<const std::string> files);

    [[nodiscard]] Directory &directory() noexcept { return directory_; }

private:
    struct RefCount {
        std::string file_name;
        bool init_done{false};
        size_t count{0};
    };

    /// `true` when the count dropped to zero and the file must be deleted.
    [[nodiscard]] bool dec_ref_one(std::string_view file);
    [[nodiscard]] VoidResult delete_file(std::string_view file);
    [[nodiscard]] VoidResult notify(FileDeleterMsg type, const std::string &message) const;

    std::map<std::string, RefCount, std::less<>> ref_counts_;
    Directory &directory_;
    std::shared_ptr<FileDeleterListener> listener_;
};

} // namespace lucene
