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

export module lucene_core:index_deletion_policy;

import :types;
import std;

import :error;
import :index_commit;

namespace lucene {

/// Decides which of the commit points of an index survive a commit.
///
/// Port of the `IndexDeletionPolicy` trait (`core/index/index_deletion_policy.rs`).
///
/// The writer calls [`on_init`] once, with the commits it found on disk (oldest
/// first), and [`on_commit`] after every commit, with the commits that are still
/// around.  A policy deletes a commit by calling
/// [`IndexCommit::delete_commit`] on it.
///
/// **Deviation:** the reference implementation parameterises the policy over the
/// concrete commit type; the port always passes the [`IndexCommit`] interface,
/// which is what the policies use anyway.  The commits are handed over as owning
/// pointers (the reference implementation's `&[Arc<CommitPoint>]`), so a policy may
/// keep one: [`SnapshotDeletionPolicy`](:snapshot_deletion_policy) does exactly
/// that.
export class IndexDeletionPolicy {
public:
    virtual ~IndexDeletionPolicy() = default;

    /// Called once, when the writer starts.
    [[nodiscard]] virtual VoidResult on_init(std::span<const std::shared_ptr<IndexCommit>> commits) = 0;

    /// Called after every commit.
    [[nodiscard]] virtual VoidResult on_commit(std::span<const std::shared_ptr<IndexCommit>> commits) = 0;

    /// The name of the policy, for diagnostics.
    [[nodiscard]] virtual std::string to_string() const = 0;
};

/// Keeps every commit point around, deleting nothing.
///
/// Port of `NoDeletionPolicy` (`core/index/no_deletion_policy.rs`).
export class NoDeletionPolicy final : public IndexDeletionPolicy {
public:
    [[nodiscard]] VoidResult on_init(std::span<const std::shared_ptr<IndexCommit>> /*commits*/) override { return {}; }
    [[nodiscard]] VoidResult on_commit(std::span<const std::shared_ptr<IndexCommit>> /*commits*/) override { return {}; }
    [[nodiscard]] std::string to_string() const override { return "NoDeletionPolicy"; }
};

/// Keeps only the most recent commit point, the default policy.
///
/// Port of `KeepOnlyLastCommitDeletionPolicy`
/// (`core/index/keep_only_last_commit_deletion_policy.rs`).
export class KeepOnlyLastCommitDeletionPolicy final : public IndexDeletionPolicy {
public:
    /// Deletes every commit but the last one.
    [[nodiscard]] VoidResult on_init(std::span<const std::shared_ptr<IndexCommit>> commits) override {
        return on_commit(commits);
    }

    /// Deletes every commit but the last one.
    [[nodiscard]] VoidResult on_commit(std::span<const std::shared_ptr<IndexCommit>> commits) override;

    [[nodiscard]] std::string to_string() const override { return "KeepOnlyLastCommitDeletionPolicy"; }
};

} // namespace lucene
