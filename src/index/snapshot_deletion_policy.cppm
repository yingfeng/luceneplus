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

export module lucene_core:snapshot_deletion_policy;

import :types;
import std;

import :error;
import :index_commit;
import :index_deletion_policy;

namespace lucene {

/// Reported when a snapshot policy is used without the writer having adopted it.
export inline constexpr std::string_view SNAPSHOT_MISUSE_MESSAGE =
        "this instance is not being used by IndexWriter; be sure to use the instance returned "
        "from writer.get_config().get_index_deletion_policy()";

/// One commit point protected from deletion while a snapshot is held.
///
/// Port of `SnapshotCommitPoint` (`index/snapshot_deletion_policy.rs`).
///
/// **Deviation:** it stays inside this module; callers get it through the
/// [`IndexCommit`] interface, which is why the snapshots are handed out as
/// `std::shared_ptr<IndexCommit>`.
class SnapshotCommitPoint final : public IndexCommit {
public:
    SnapshotCommitPoint(std::shared_ptr<IndexCommit> commit, std::shared_ptr<struct SnapshotInner> inner);

    [[nodiscard]] const std::string &segments_file_name() const override { return commit_->segments_file_name(); }
    [[nodiscard]] const std::vector<std::string> &file_names() const override { return commit_->file_names(); }
    [[nodiscard]] Directory &directory() const override { return commit_->directory(); }

    /// Suppressed while the commit's generation is snapshotted, ignored
    /// otherwise: that suppression is what keeps the files alive.
    [[nodiscard]] VoidResult delete_commit() const override;

    [[nodiscard]] bool is_deleted() const override { return commit_->is_deleted(); }
    [[nodiscard]] size_t segment_count() const override { return commit_->segment_count(); }
    [[nodiscard]] int64_t generation() const override { return commit_->generation(); }
    [[nodiscard]] const std::map<std::string, std::string> &user_data() const override {
        return commit_->user_data();
    }
    [[nodiscard]] std::string to_string() const override {
        return "SnapshotDeletionPolicy.SnapshotCommitPoint(" + commit_->to_string() + ")";
    }

private:
    std::shared_ptr<IndexCommit> commit_;
    std::shared_ptr<struct SnapshotInner> inner_;
};

/// The bookkeeping a [`SnapshotDeletionPolicy`] and its commit wrappers share.
///
/// Port of `Inner` (`index/snapshot_deletion_policy.rs`).  It stays inside this
/// module; the wrappers only ever see it as a `shared_ptr`.
///
/// The mutex is **recursive** because the reference implementation's is: a
/// policy callback runs while the lock is held, and the commits it is handed can
/// re-enter through [`SnapshotCommitPoint::delete_commit`].
struct SnapshotInner {
    SnapshotInner() = default;

    [[nodiscard]] std::unique_lock<std::recursive_mutex> lock() const {
        return std::unique_lock<std::recursive_mutex>(mutex_);
    }

    /// How many snapshots are held against each commit generation.
    std::map<int64_t, int32_t> ref_counts;
    /// Generation to the commit it stands for.
    std::map<int64_t, std::shared_ptr<IndexCommit>> index_commits;
    /// Most recently committed commit point.
    std::shared_ptr<IndexCommit> last_commit;
    /// Set by the first `on_init`, to detect misuse.
    bool init_called{false};

private:
    mutable std::recursive_mutex mutex_;
};

/// An [`IndexDeletionPolicy`] that wraps another one and can hold snapshots.
///
/// Port of `SnapshotDeletionPolicy` (`index/snapshot_deletion_policy.rs`).
///
/// While a snapshot is held the writer keeps every file that commit references,
/// even as the index changes underneath; [`release`] (followed by a writer
/// `delete_unused_files`) lets them go again.  Because the wrapped policy keeps
/// doing its job for the non-snapshotted commits, this composes with any policy,
/// normally [`KeepOnlyLastCommitDeletionPolicy`].
///
/// The snapshots live in memory only: they survive neither a restart nor a
/// crash.  Use `PersistentSnapshotDeletionPolicy` when that matters.
///
/// **Experimental**, like the reference implementation.
///
/// **Deviations** (C++ mechanics only):
///  * snapshots are handed out as `std::shared_ptr<IndexCommit>` and are always
///    the protected wrapper (the reference implementation returns the bare
///    commit from `snapshot()`, which lets a caller delete it by accident);
///  * the shared state is guarded by a `std::recursive_mutex` instead of the
///    reference implementation's reentrant monitor — the two behave the same.
export class SnapshotDeletionPolicy final : public IndexDeletionPolicy {
public:
    /// Takes ownership of the policy to wrap.
    explicit SnapshotDeletionPolicy(std::unique_ptr<IndexDeletionPolicy> primary);

    ~SnapshotDeletionPolicy() override = default;

    // ---- IndexDeletionPolicy ----

    [[nodiscard]] VoidResult on_init(std::span<const std::shared_ptr<IndexCommit>> commits) override;
    [[nodiscard]] VoidResult on_commit(std::span<const std::shared_ptr<IndexCommit>> commits) override;
    [[nodiscard]] std::string to_string() const override;

    // ---- snapshots ----

    /// Snapshots the last commit and returns it, so it is protected from now on.
    ///
    /// Fails with `illegal_state` when the policy was never used by a writer and
    /// when the index has no commit yet.
    ///
    /// **NOTE:** the files of a snapshot stay on disk, so holding one through a
    /// `force_merge` can cost up to the size of the whole index.
    [[nodiscard]] Result<std::shared_ptr<IndexCommit>> snapshot();

    /// Releases `commit`'s snapshot.
    [[nodiscard]] VoidResult release(const IndexCommit &commit);
    /// Releases one snapshot of `generation`.
    [[nodiscard]] VoidResult release_gen(int64_t generation);

    /// Every commit held by at least one snapshot.
    [[nodiscard]] std::vector<std::shared_ptr<IndexCommit>> get_snapshots() const;
    /// The total number of snapshots currently held.
    [[nodiscard]] int32_t get_snapshot_count() const;
    /// The commit of `generation`, or `nullptr` when it is not snapshotted.
    [[nodiscard]] std::shared_ptr<IndexCommit> get_index_commit(int64_t generation) const;

    // ---- introspection ----

    [[nodiscard]] IndexDeletionPolicy &primary() noexcept { return *primary_; }
    [[nodiscard]] const IndexDeletionPolicy &primary() const noexcept { return *primary_; }

    /// Whether [`on_init`] has run, i.e. whether a writer adopted this policy.
    [[nodiscard]] bool init_called() const;

    // ---- state access for the persistent variant ----

    /// A copy of the snapshot reference counts, keyed by commit generation.
    ///
    /// Exposed for `PersistentSnapshotDeletionPolicy`, which has to write them to
    /// disk.  The reference implementation reaches into the shared state instead.
    [[nodiscard]] std::map<int64_t, int32_t> ref_counts() const;

    /// Replaces the reference counts, e.g. with what was read from disk.
    ///
    /// The remembered commits are dropped because a generation read from disk is
    /// not backed by a commit object yet; the next [`on_init`] re-links the ones
    /// that are still on disk.
    [[nodiscard]] VoidResult restore_ref_counts(std::map<int64_t, int32_t> counts);

    /// Increments the reference count of `commit`.
    ///
    /// Public because the persistent variant has to roll a release back when the
    /// snapshot file cannot be written.
    void inc_ref(const std::shared_ptr<IndexCommit> &commit);

private:
    /// Wraps each commit so its deletion is suppressed while snapshotted.
    [[nodiscard]] std::vector<std::shared_ptr<IndexCommit>>
    wrap_commits(std::span<const std::shared_ptr<IndexCommit>> commits) const;

    std::unique_ptr<IndexDeletionPolicy> primary_;
    std::shared_ptr<SnapshotInner> inner_;
};

} // namespace lucene
