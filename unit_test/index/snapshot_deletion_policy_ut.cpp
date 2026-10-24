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

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.snapshot_deletion_policy;

import lucene_core;

using namespace lucene;

namespace {

/// A commit point that only counts how often it was asked to delete itself.
class FakeCommit final : public IndexCommit {
public:
    FakeCommit(int64_t generation, Directory &directory) : generation_(generation), directory_(directory) {
        segments_file_ = "segments_" + std::to_string(generation);
        files_ = {segments_file_, "_" + std::to_string(generation) + ".si"};
    }

    [[nodiscard]] const std::string &segments_file_name() const override { return segments_file_; }
    [[nodiscard]] const std::vector<std::string> &file_names() const override { return files_; }
    [[nodiscard]] Directory &directory() const override { return directory_; }

    [[nodiscard]] VoidResult delete_commit() const override {
        ++delete_calls_;
        deleted_ = true;
        return {};
    }

    [[nodiscard]] bool is_deleted() const override { return deleted_; }
    [[nodiscard]] size_t segment_count() const override { return 2; }
    [[nodiscard]] int64_t generation() const override { return generation_; }
    [[nodiscard]] const std::map<std::string, std::string> &user_data() const override { return user_data_; }
    [[nodiscard]] std::string to_string() const override { return "FakeCommit(" + segments_file_ + ")"; }

    [[nodiscard]] int32_t delete_calls() const { return delete_calls_; }

private:
    int64_t generation_;
    Directory &directory_;
    std::string segments_file_;
    std::vector<std::string> files_;
    std::map<std::string, std::string> user_data_;
    mutable bool deleted_{false};
    mutable int32_t delete_calls_{0};
};

/// A policy that only records what it was handed.
class RecordingPolicy final : public IndexDeletionPolicy {
public:
    [[nodiscard]] VoidResult on_init(std::span<const std::shared_ptr<IndexCommit>> commits) override {
        ++init_calls;
        seen.assign(commits.begin(), commits.end());
        if (failure.has_value()) {
            return std::unexpected(failure.value());
        }
        return {};
    }

    [[nodiscard]] VoidResult on_commit(std::span<const std::shared_ptr<IndexCommit>> commits) override {
        ++commit_calls;
        seen.assign(commits.begin(), commits.end());
        if (failure.has_value()) {
            return std::unexpected(failure.value());
        }
        return {};
    }

    [[nodiscard]] std::string to_string() const override { return "RecordingPolicy"; }

    std::vector<std::shared_ptr<IndexCommit>> seen;
    std::optional<LuceneError> failure;
    int32_t init_calls{0};
    int32_t commit_calls{0};
};

/// `std::span` cannot be built from a braced list, so the commit lists are
/// materialised first.
std::vector<std::shared_ptr<IndexCommit>> commits_of(std::initializer_list<std::shared_ptr<IndexCommit>> list) {
    return std::vector<std::shared_ptr<IndexCommit>>(list);
}

} // namespace

TEST(SnapshotDeletionPolicyTest, SnapshottedCommitsSurviveARegularCommit) {
    ByteBuffersDirectory directory;
    auto first = std::make_shared<FakeCommit>(1, directory);
    auto second = std::make_shared<FakeCommit>(2, directory);

    std::unique_ptr<IndexDeletionPolicy> primary = std::make_unique<KeepOnlyLastCommitDeletionPolicy>();
    SnapshotDeletionPolicy policy(std::move(primary));

    ASSERT_TRUE(policy.on_init(commits_of({first})).has_value());
    auto snapshot = policy.snapshot();
    ASSERT_TRUE(snapshot.has_value()) << (snapshot ? "" : snapshot.error().to_string());
    EXPECT_EQ(snapshot.value()->generation(), 1);
    EXPECT_EQ(policy.get_snapshot_count(), 1);

    // `KeepOnlyLast` would delete the first commit; the snapshot suppresses it.
    const std::vector<std::shared_ptr<IndexCommit>> commits{first, second};
    ASSERT_TRUE(policy.on_commit(commits).has_value());
    EXPECT_FALSE(first->is_deleted());
    EXPECT_EQ(first->delete_calls(), 0);
    EXPECT_FALSE(second->is_deleted());
}

TEST(SnapshotDeletionPolicyTest, ReleasingTheSnapshotLetsTheCommitGo) {
    ByteBuffersDirectory directory;
    auto first = std::make_shared<FakeCommit>(1, directory);
    auto second = std::make_shared<FakeCommit>(2, directory);

    SnapshotDeletionPolicy policy(std::make_unique<KeepOnlyLastCommitDeletionPolicy>());
    ASSERT_TRUE(policy.on_init(commits_of({first})).has_value());
    auto snapshot = policy.snapshot();
    ASSERT_TRUE(snapshot.has_value());

    ASSERT_TRUE(policy.release(*snapshot.value()).has_value());
    EXPECT_EQ(policy.get_snapshot_count(), 0);

    const std::vector<std::shared_ptr<IndexCommit>> commits{first, second};
    ASSERT_TRUE(policy.on_commit(commits).has_value());
    EXPECT_TRUE(first->is_deleted()) << "no snapshot holds it any more";
}

TEST(SnapshotDeletionPolicyTest, MisuseWithoutAWriterIsRejected) {
    SnapshotDeletionPolicy policy(std::make_unique<NoDeletionPolicy>());
    EXPECT_FALSE(policy.init_called());

    auto snapshot = policy.snapshot();
    ASSERT_FALSE(snapshot.has_value());
    EXPECT_EQ(snapshot.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(snapshot.error().message().find("not being used by IndexWriter"), std::string::npos);

    EXPECT_EQ(policy.release_gen(1).error().kind(), ErrorKind::kIllegalState);
    EXPECT_FALSE(policy.init_called());
}

TEST(SnapshotDeletionPolicyTest, AnEmptyIndexHasNothingToSnapshot) {
    SnapshotDeletionPolicy policy(std::make_unique<NoDeletionPolicy>());
    ASSERT_TRUE(policy.on_init(commits_of({})).has_value());
    EXPECT_TRUE(policy.init_called());

    auto snapshot = policy.snapshot();
    ASSERT_FALSE(snapshot.has_value());
    EXPECT_EQ(snapshot.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(snapshot.error().message().find("No index commit to snapshot"), std::string::npos);
}

TEST(SnapshotDeletionPolicyTest, ReleasingAGenerationNobodySnapshottedIsRejected) {
    ByteBuffersDirectory directory;
    auto first = std::make_shared<FakeCommit>(1, directory);
    SnapshotDeletionPolicy policy(std::make_unique<NoDeletionPolicy>());
    ASSERT_TRUE(policy.on_init(commits_of({first})).has_value());

    auto released = policy.release_gen(7);
    ASSERT_FALSE(released.has_value());
    EXPECT_EQ(released.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SnapshotDeletionPolicyTest, TheSameGenerationCanBeSnapshottedTwice) {
    ByteBuffersDirectory directory;
    auto first = std::make_shared<FakeCommit>(1, directory);
    SnapshotDeletionPolicy policy(std::make_unique<NoDeletionPolicy>());
    ASSERT_TRUE(policy.on_init(commits_of({first})).has_value());

    ASSERT_TRUE(policy.snapshot().has_value());
    ASSERT_TRUE(policy.snapshot().has_value());
    EXPECT_EQ(policy.get_snapshot_count(), 2);
    ASSERT_EQ(policy.get_snapshots().size(), 1U) << "one commit, two references";

    ASSERT_TRUE(policy.release_gen(1).has_value());
    EXPECT_EQ(policy.get_snapshot_count(), 1);

    // Still protected while one reference is left.
    ASSERT_TRUE(policy.get_index_commit(1)->delete_commit().has_value());
    EXPECT_EQ(first->delete_calls(), 0);

    ASSERT_TRUE(policy.release_gen(1).has_value());
    EXPECT_EQ(policy.get_snapshot_count(), 0);
    EXPECT_TRUE(policy.get_snapshots().empty());
    EXPECT_EQ(policy.get_index_commit(1), nullptr);
}

TEST(SnapshotDeletionPolicyTest, SnapshotsExposeTheCommitInterface) {
    ByteBuffersDirectory directory;
    auto first = std::make_shared<FakeCommit>(3, directory);
    SnapshotDeletionPolicy policy(std::make_unique<NoDeletionPolicy>());
    ASSERT_TRUE(policy.on_init(commits_of({first})).has_value());
    auto snapshot = policy.snapshot();
    ASSERT_TRUE(snapshot.has_value());

    const IndexCommit &commit = *snapshot.value();
    EXPECT_EQ(commit.segments_file_name(), "segments_3");
    EXPECT_EQ(commit.file_names().size(), 2U);
    EXPECT_EQ(&commit.directory(), &directory);
    EXPECT_EQ(commit.segment_count(), 2U);
    EXPECT_EQ(commit.generation(), 3);
    EXPECT_TRUE(commit.user_data().empty());
    EXPECT_NE(commit.to_string().find("SnapshotDeletionPolicy.SnapshotCommitPoint(FakeCommit(segments_3))"),
              std::string::npos);

    // Even deleting through the snapshot itself is suppressed.
    ASSERT_TRUE(commit.delete_commit().has_value());
    EXPECT_FALSE(commit.is_deleted());
    EXPECT_EQ(first->delete_calls(), 0);

    EXPECT_EQ(policy.get_index_commit(3)->to_string().rfind("SnapshotDeletionPolicy.SnapshotCommitPoint(", 0), 0U);
    EXPECT_EQ(policy.get_index_commit(9), nullptr);
    EXPECT_EQ(policy.to_string(), "SnapshotDeletionPolicy(NoDeletionPolicy)");
}

TEST(SnapshotDeletionPolicyTest, TheWrappedPolicySeesProtectedCommits) {
    ByteBuffersDirectory directory;
    auto first = std::make_shared<FakeCommit>(1, directory);
    auto second = std::make_shared<FakeCommit>(2, directory);

    auto recording = std::make_unique<RecordingPolicy>();
    RecordingPolicy *observed = recording.get();
    SnapshotDeletionPolicy policy(std::move(recording));

    ASSERT_TRUE(policy.on_init(commits_of({first, second})).has_value());
    EXPECT_EQ(observed->init_calls, 1);
    ASSERT_EQ(observed->seen.size(), 2U);
    EXPECT_EQ(observed->seen[0]->generation(), 1);
    EXPECT_EQ(observed->seen[1]->generation(), 2);
    EXPECT_EQ(observed->seen[1]->to_string().rfind("SnapshotDeletionPolicy.SnapshotCommitPoint(", 0), 0U)
            << "the wrapped policy must not be able to delete a snapshotted commit";
    EXPECT_EQ(policy.primary().to_string(), "RecordingPolicy");
}

TEST(SnapshotDeletionPolicyTest, PrimaryFailuresPropagate) {
    ByteBuffersDirectory directory;
    auto first = std::make_shared<FakeCommit>(1, directory);

    auto recording = std::make_unique<RecordingPolicy>();
    RecordingPolicy *observed = recording.get();
    SnapshotDeletionPolicy policy(std::move(recording));
    observed->failure = LuceneError::illegal_state("primary refuses");

    auto initialized = policy.on_init(commits_of({first}));
    ASSERT_FALSE(initialized.has_value());
    EXPECT_EQ(initialized.error().kind(), ErrorKind::kIllegalState);
    EXPECT_EQ(initialized.error().message(), "primary refuses");
}

TEST(SnapshotDeletionPolicyTest, TheSnapshotFollowsTheLastCommit) {
    ByteBuffersDirectory directory;
    auto first = std::make_shared<FakeCommit>(1, directory);
    auto second = std::make_shared<FakeCommit>(2, directory);

    SnapshotDeletionPolicy policy(std::make_unique<NoDeletionPolicy>());
    ASSERT_TRUE(policy.on_init(commits_of({first})).has_value());
    ASSERT_TRUE(policy.on_commit(commits_of({first, second})).has_value());

    auto snapshot = policy.snapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot.value()->generation(), 2);
}
