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

export module lucene_core:persistent_snapshot_deletion_policy;

import :types;
import std;

import :directory;
import :error;
import :index_commit;
import :index_deletion_policy;
import :info_stream;
import :snapshot_deletion_policy;

namespace lucene {

/// Prefix of the files a [`PersistentSnapshotDeletionPolicy`] writes.
export inline constexpr std::string_view SNAPSHOTS_PREFIX = "snapshots_";

/// How a [`PersistentSnapshotDeletionPolicy`] treats the snapshots already on disk.
///
/// Port of `OpenMode` (`index/index_writer_config.rs`), which this port does not
/// have yet.
export enum class SnapshotOpenMode : uint8_t {
    /// Keep the stored snapshots, and start empty when there are none.
    kCreateOrAppend = 0,
    /// Delete the stored snapshots and start empty.
    kCreate = 1,
    /// Keep the stored snapshots; it is an error when there are none.
    kAppend = 2,
};

export class PersistentSnapshotDeletionPolicy;

/// Creates a policy that persists its snapshots in `directory`.
///
/// The factory is a free function rather than a static member because clang
/// mis-compiles a module interface whose exported class has a static factory
/// returning `Result<std::...<Itself>>` (see [`IndexFileDeleter`](:index_file_deleter)).
[[nodiscard]] Result<std::shared_ptr<PersistentSnapshotDeletionPolicy>>
create_persistent_snapshot_deletion_policy(std::unique_ptr<IndexDeletionPolicy> primary, Directory &directory);

/// Same, with an explicit open mode.
[[nodiscard]] Result<std::shared_ptr<PersistentSnapshotDeletionPolicy>>
create_persistent_snapshot_deletion_policy(std::unique_ptr<IndexDeletionPolicy> primary, Directory &directory,
                                           SnapshotOpenMode mode);

/// A [`SnapshotDeletionPolicy`] that persists its snapshots, so they survive the
/// application.
///
/// Port of `PersistentSnapshotDeletionPolicy`
/// (`index/persistent_snapshot_deletion_policy.rs`).
///
/// After every [`snapshot`] and [`release`] the reference counts are written to a
/// `snapshots_<n>` file and the previous one is deleted, so reopening the policy
/// restores the snapshots that are still held.
///
/// **NOTE:** two policies writing to the same directory corrupt each other's
/// snapshots; every writer needs its own policy (and its own directory).  Using
/// the directory that holds the index is fine.
///
/// **Experimental**, like the reference implementation.
///
/// **Deviations** (C++ mechanics only):
///  * writes are serialized by this object's own mutex instead of the base
///    policy's monitor;
///  * a `snapshots_<n>` file that cannot be read is skipped (the newest readable
///    one wins, like the reference implementation), but the error is only kept when no
///    file could be read at all.
export class PersistentSnapshotDeletionPolicy final : public IndexDeletionPolicy {
public:
    ~PersistentSnapshotDeletionPolicy() override = default;

    // ---- IndexDeletionPolicy: delegated to the in-memory policy ----

    [[nodiscard]] VoidResult on_init(std::span<const std::shared_ptr<IndexCommit>> commits) override;
    [[nodiscard]] VoidResult on_commit(std::span<const std::shared_ptr<IndexCommit>> commits) override;
    [[nodiscard]] std::string to_string() const override;

    // ---- snapshots ----

    /// Snapshots the last commit and persists the snapshot before returning.
    [[nodiscard]] Result<std::shared_ptr<IndexCommit>> snapshot();

    /// Releases `commit`'s snapshot and persists the new state.
    [[nodiscard]] VoidResult release(const IndexCommit &commit);
    /// Releases one snapshot of `generation` and persists the new state.
    [[nodiscard]] VoidResult release_gen(int64_t generation);

    // ---- introspection ----

    /// The file the snapshots are currently stored in, or `nullopt` when nothing
    /// has been saved yet.
    [[nodiscard]] std::optional<std::string> last_save_file() const;

    [[nodiscard]] std::vector<std::shared_ptr<IndexCommit>> get_snapshots() const;
    [[nodiscard]] int32_t get_snapshot_count() const;
    [[nodiscard]] std::shared_ptr<IndexCommit> get_index_commit(int64_t generation) const;

    /// The in-memory policy underneath.
    [[nodiscard]] SnapshotDeletionPolicy &base() noexcept { return *base_; }
    [[nodiscard]] const SnapshotDeletionPolicy &base() const noexcept { return *base_; }
    [[nodiscard]] Directory &directory() const noexcept { return *directory_; }

private:
    PersistentSnapshotDeletionPolicy(std::unique_ptr<IndexDeletionPolicy> primary, Directory &directory);

    friend Result<std::shared_ptr<PersistentSnapshotDeletionPolicy>>
    create_persistent_snapshot_deletion_policy(std::unique_ptr<IndexDeletionPolicy> primary, Directory &directory,
                                               SnapshotOpenMode mode);

    /// Reads the snapshots stored in the directory.
    [[nodiscard]] VoidResult load_prior_snapshots();
    /// Deletes every stored snapshots file.
    [[nodiscard]] VoidResult clear_prior_snapshots();
    /// Writes the current counts to `snapshots_<n>` and drops the previous file.
    [[nodiscard]] VoidResult persist();

    std::shared_ptr<SnapshotDeletionPolicy> base_;
    Directory *directory_;
    mutable std::mutex mutex_;
    int64_t next_write_gen_{0};
};

} // namespace lucene
