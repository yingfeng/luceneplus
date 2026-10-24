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

export module lucene_core:index_file_deleter;

import :types;
import std;

import :directory;
import :error;
import :file_deleter;
import :index_commit;
import :index_deletion_policy;
import :info_stream;
import :segment_infos;

namespace lucene {

/// Name of the write lock file, which the deleter never touches.
export inline constexpr std::string_view WRITE_LOCK_NAME = "write.lock";

/// One commit point (`segments_N`) as the [`IndexDeletionPolicy`] sees it.
///
/// Port of `CommitPoint` (`index/index_file_deleter.rs`).
///
/// Deleting it only marks it: the [`IndexFileDeleter`] notices the flag at its
/// next `delete_commits` pass, decrements the file references and lets the files
/// with a count of zero go.
class CommitPoint final : public IndexCommit {
public:
    /// Snapshots `infos`; `commits_to_delete` is shared with the deleter that
    /// owns this point, so a policy can mark a commit from any thread.
    CommitPoint(Directory &directory, const SegmentInfos &infos,
                std::shared_ptr<std::atomic<bool>> commits_to_delete);

    // ---- IndexCommit ----

    [[nodiscard]] const std::string &segments_file_name() const override { return segments_file_name_; }
    [[nodiscard]] const std::vector<std::string> &file_names() const override { return files_; }
    [[nodiscard]] Directory &directory() const override { return directory_; }
    [[nodiscard]] VoidResult delete_commit() const override;
    [[nodiscard]] bool is_deleted() const override { return deleted_.load(std::memory_order_seq_cst); }
    [[nodiscard]] size_t segment_count() const override { return segment_count_; }
    [[nodiscard]] int64_t generation() const override { return generation_; }
    [[nodiscard]] const std::map<std::string, std::string> &user_data() const override { return user_data_; }
    [[nodiscard]] std::string to_string() const override { return "CommitPoint(" + segments_file_name_ + ")"; }

    // ---- introspection ----

    /// The files of this commit without its `segments_N`.
    [[nodiscard]] const std::vector<std::string> &segment_files() const noexcept { return segment_files_; }

private:
    Directory &directory_;
    std::vector<std::string> files_;
    std::vector<std::string> segment_files_;
    std::string segments_file_name_;
    int64_t generation_{0};
    size_t segment_count_{0};
    std::map<std::string, std::string> user_data_;
    mutable std::atomic<bool> deleted_{false};
    std::shared_ptr<std::atomic<bool>> commits_to_delete_;
};

export class IndexFileDeleter;

/// Scans `directory`, builds the commit points and applies `policy`.
///
/// The factory of [`IndexFileDeleter`].  **Deviation:** a free function instead of
/// a static member: clang mis-compiles a module interface whose exported class has
/// a static factory returning `Result<std::unique_ptr<Itself>>`, and every later
/// translation unit that includes `<memory>` then fails with "cannot befriend
/// target of using declaration".
[[nodiscard]] Result<std::shared_ptr<IndexFileDeleter>>
create_index_file_deleter(Directory &directory, IndexDeletionPolicy &policy, SegmentInfos &infos);

/// Same, with an explicit info stream and index-existence flag.
///
/// `infos` is the writer's in-memory state: its files are protected from
/// deletion.  `initial_index_exists` is `false` only when the writer is creating
/// the index from scratch.
[[nodiscard]] Result<std::shared_ptr<IndexFileDeleter>>
create_index_file_deleter(Directory &directory, IndexDeletionPolicy &policy, SegmentInfos &infos,
                          InfoStreamMT info_stream, bool initial_index_exists);

/// Keeps every live [`SegmentInfos`] reference-counted, so a file is removed only
/// once no commit (and no checkpoint) needs it any more.
///
/// Port of `IndexFileDeleter` (`index/index_file_deleter.rs`).
///
/// The writer calls [`create_index_file_deleter`] once (which loads every
/// `segments_N`, lets the policy delete the ones it does not want and removes the
/// files nothing references), then [`checkpoint`] after every change that writes
/// files.
///
/// **Deviations** (C++ mechanics only):
///  * the reference implementation wraps the writer's directory in an
///    `IndexWriterDir`; this port talks to the [`Directory`] directly, because it
///    has no writer-owned file layer;
///  * "is this a codec file" is decided by the `_`-prefixed segment-name
///    convention instead of a codec regex ([`is_codec_file`]);
///  * the pending-deletions set of the writer directory is not consulted (this
///    port never defers deletions).
export class IndexFileDeleter final {
public:
    /// Protects the files of `infos` and releases the previous checkpoint.
    ///
    /// With `is_commit`, `infos` becomes a new commit point: it is added to the
    /// list, the policy runs and the commits it deleted are dropped.
    [[nodiscard]] VoidResult checkpoint(SegmentInfos &infos, bool is_commit, IndexDeletionPolicy &policy);

    /// Deletes files that appeared without being referenced (used after a
    /// rollback).
    [[nodiscard]] VoidResult refresh();

    /// Runs the policy again over the known commits, so a policy that holds
    /// commits can release them later.
    [[nodiscard]] VoidResult revisit_policy(IndexDeletionPolicy &policy);

    /// Protects `files`.
    [[nodiscard]] VoidResult inc_ref(std::span<const std::string> files);
    /// Releases `files`, deleting the ones that reach zero.
    [[nodiscard]] VoidResult dec_ref(std::span<const std::string> files);
    /// Releases the files of `infos` (not its `segments_N`).
    [[nodiscard]] VoidResult dec_ref_from_segment(const SegmentInfos &infos);
    /// Deletes `files` if nothing references them.
    [[nodiscard]] VoidResult delete_new_files(std::span<const std::string> files);

    [[nodiscard]] bool exists(std::string_view file) const { return file_deleter_.exists(file); }

    /// Releases the last checkpoint; call it when the writer closes.
    [[nodiscard]] VoidResult close();

    // ---- introspection ----

    /// The live commit points, oldest first.
    ///
    /// **Deviation:** [`CommitPoint`] stays inside this module; the commit points
    /// are handed out through the [`IndexCommit`] interface they implement.
    [[nodiscard]] std::vector<std::shared_ptr<IndexCommit>> commits() const;
    /// Whether the commit the writer started from has been deleted.
    [[nodiscard]] bool starting_commit_deleted() const noexcept { return starting_commit_deleted_; }
    [[nodiscard]] FileDeleter &file_deleter() noexcept { return file_deleter_; }
    [[nodiscard]] const FileDeleter &file_deleter() const noexcept { return file_deleter_; }

private:
    IndexFileDeleter(Directory &directory, InfoStreamMT info_stream);

    [[nodiscard]] VoidResult inc_ref_from_segment(const SegmentInfos &infos, bool is_commit);
    /// Drops the commits the policy deleted and releases their files.
    [[nodiscard]] VoidResult delete_commits();
    [[nodiscard]] VoidResult log(const std::string &message) const;

    /// The factory builds the object itself.
    friend Result<std::shared_ptr<IndexFileDeleter>>
    create_index_file_deleter(Directory &directory, IndexDeletionPolicy &policy, SegmentInfos &infos,
                              InfoStreamMT info_stream, bool initial_index_exists);

    std::vector<std::shared_ptr<CommitPoint>> commits_;
    /// Files protected by the previous (non-commit) checkpoint.
    std::vector<std::string> last_files_;
    std::shared_ptr<std::atomic<bool>> commits_to_delete_;
    InfoStreamMT info_stream_;
    bool starting_commit_deleted_{false};
    FileDeleter file_deleter_;
};

/// Whether `file` looks like a segment file (`_0.si`, `_12_3.liv`, ...).
///
/// The reference implementation matches a codec extension regex; every segment
/// file of this port starts with `_` followed by the segment name, so the prefix
/// is enough and cannot match `segments_N` or `write.lock`.
[[nodiscard]] bool is_codec_file(std::string_view file);

/// The files `infos` references, including its `segments_N`.
[[nodiscard]] std::vector<std::string> commit_files(const SegmentInfos &infos);

/// The files `infos` references, without its `segments_N`.
[[nodiscard]] std::vector<std::string> segment_files(const SegmentInfos &infos);

/// Pushes every generation counter past what the directory already holds, so a
/// previous writer that crashed cannot make us overwrite its files.
///
/// Port of `inflate_gens` (`index/index_file_deleter.rs`), adapted to this port's
/// single-generation `SegmentCommitInfo` fields.
[[nodiscard]] VoidResult inflate_gens(SegmentInfos &infos, std::span<const std::string> files,
                                      const InfoStreamMT &info_stream);

} // namespace lucene
