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

module lucene_core:snapshot_deletion_policy.impl;

import :snapshot_deletion_policy;

import std;

import :error;
import :index_commit;
import :index_deletion_policy;
import :types;

namespace lucene {

// ---------------- SnapshotCommitPoint ----------------

SnapshotCommitPoint::SnapshotCommitPoint(std::shared_ptr<IndexCommit> commit,
                                        std::shared_ptr<SnapshotInner> inner)
    : commit_(std::move(commit)), inner_(std::move(inner)) {}

VoidResult SnapshotCommitPoint::delete_commit() const {
    const auto lock = inner_->lock();
    // Suppress the delete request while this generation is snapshotted; that is
    // the whole point of the wrapper.
    if (inner_->ref_counts.find(commit_->generation()) != inner_->ref_counts.end()) {
        return {};
    }
    return commit_->delete_commit();
}

// ---------------- SnapshotDeletionPolicy ----------------

SnapshotDeletionPolicy::SnapshotDeletionPolicy(std::unique_ptr<IndexDeletionPolicy> primary)
    : primary_(std::move(primary)), inner_(std::make_shared<SnapshotInner>()) {}

std::vector<std::shared_ptr<IndexCommit>>
SnapshotDeletionPolicy::wrap_commits(std::span<const std::shared_ptr<IndexCommit>> commits) const {
    std::vector<std::shared_ptr<IndexCommit>> wrapped;
    wrapped.reserve(commits.size());
    for (const std::shared_ptr<IndexCommit> &commit : commits) {
        if (commit == nullptr) {
            wrapped.push_back(nullptr);
            continue;
        }
        wrapped.push_back(std::make_shared<SnapshotCommitPoint>(commit, inner_));
    }
    return wrapped;
}

void SnapshotDeletionPolicy::inc_ref(const std::shared_ptr<IndexCommit> &commit) {
    const auto lock = inner_->lock();
    const int64_t generation = commit->generation();
    const auto found = inner_->ref_counts.find(generation);
    const int32_t ref_count = found == inner_->ref_counts.end() ? 0 : found->second;
    if (ref_count == 0) {
        inner_->index_commits[generation] = commit;
    }
    inner_->ref_counts[generation] = ref_count + 1;
}

VoidResult SnapshotDeletionPolicy::on_init(std::span<const std::shared_ptr<IndexCommit>> commits) {
    const auto lock = inner_->lock();
    const std::vector<std::shared_ptr<IndexCommit>> wrapped = wrap_commits(commits);
    inner_->init_called = true;

    // The lock is recursive, so the wrapped commits may re-enter it from the
    // primary policy's `delete_commit` calls.
    if (auto initialized = primary_->on_init(wrapped); !initialized.has_value()) {
        return initialized;
    }

    for (const std::shared_ptr<IndexCommit> &commit : commits) {
        if (commit == nullptr) {
            continue;
        }
        if (inner_->ref_counts.find(commit->generation()) != inner_->ref_counts.end()) {
            inner_->index_commits[commit->generation()] = commit;
        }
    }
    if (!commits.empty()) {
        inner_->last_commit = commits.back();
    }
    return {};
}

VoidResult SnapshotDeletionPolicy::on_commit(std::span<const std::shared_ptr<IndexCommit>> commits) {
    const auto lock = inner_->lock();
    const std::vector<std::shared_ptr<IndexCommit>> wrapped = wrap_commits(commits);
    if (auto committed = primary_->on_commit(wrapped); !committed.has_value()) {
        return committed;
    }
    inner_->last_commit = commits.empty() ? nullptr : commits.back();
    return {};
}

Result<std::shared_ptr<IndexCommit>> SnapshotDeletionPolicy::snapshot() {
    const auto lock = inner_->lock();
    if (!inner_->init_called) {
        return err(LuceneError::illegal_state(std::string(SNAPSHOT_MISUSE_MESSAGE)));
    }
    if (inner_->last_commit == nullptr) {
        return err(LuceneError::illegal_state("No index commit to snapshot"));
    }

    const std::shared_ptr<IndexCommit> commit = inner_->last_commit;
    inc_ref(commit);
    // Hand out the protected view, so a stray `delete_commit` cannot undo it.
    return std::make_shared<SnapshotCommitPoint>(commit, inner_);
}

VoidResult SnapshotDeletionPolicy::release(const IndexCommit &commit) { return release_gen(commit.generation()); }

VoidResult SnapshotDeletionPolicy::release_gen(int64_t generation) {
    const auto lock = inner_->lock();
    if (!inner_->init_called) {
        return err(LuceneError::illegal_state(std::string(SNAPSHOT_MISUSE_MESSAGE)));
    }
    const auto found = inner_->ref_counts.find(generation);
    if (found == inner_->ref_counts.end()) {
        return err(LuceneError::illegal_argument("commit gen=" + std::to_string(generation) +
                                                 " is not currently snapshotted"));
    }
    debug_check(found->second > 0);
    --found->second;
    if (found->second == 0) {
        inner_->ref_counts.erase(found);
        inner_->index_commits.erase(generation);
    }
    return {};
}

std::vector<std::shared_ptr<IndexCommit>> SnapshotDeletionPolicy::get_snapshots() const {
    const auto lock = inner_->lock();
    std::vector<std::shared_ptr<IndexCommit>> snapshots;
    snapshots.reserve(inner_->index_commits.size());
    for (const auto &[generation, commit] : inner_->index_commits) {
        snapshots.push_back(std::make_shared<SnapshotCommitPoint>(commit, inner_));
    }
    return snapshots;
}

int32_t SnapshotDeletionPolicy::get_snapshot_count() const {
    const auto lock = inner_->lock();
    int32_t count = 0;
    for (const auto &[generation, ref_count] : inner_->ref_counts) {
        count += ref_count;
    }
    return count;
}

std::shared_ptr<IndexCommit> SnapshotDeletionPolicy::get_index_commit(int64_t generation) const {
    const auto lock = inner_->lock();
    const auto found = inner_->index_commits.find(generation);
    if (found == inner_->index_commits.end()) {
        return nullptr;
    }
    return std::make_shared<SnapshotCommitPoint>(found->second, inner_);
}

bool SnapshotDeletionPolicy::init_called() const {
    const auto lock = inner_->lock();
    return inner_->init_called;
}

std::map<int64_t, int32_t> SnapshotDeletionPolicy::ref_counts() const {
    const auto lock = inner_->lock();
    return inner_->ref_counts;
}

VoidResult SnapshotDeletionPolicy::restore_ref_counts(std::map<int64_t, int32_t> counts) {
    const auto lock = inner_->lock();
    for (const auto &[generation, ref_count] : counts) {
        if (ref_count <= 0) {
            return err(LuceneError::illegal_argument("the snapshot count of generation " +
                                                     std::to_string(generation) + " must be positive"));
        }
    }
    inner_->ref_counts = std::move(counts);
    inner_->index_commits.clear();
    return {};
}

std::string SnapshotDeletionPolicy::to_string() const {
    return "SnapshotDeletionPolicy(" + primary_->to_string() + ")";
}

} // namespace lucene
