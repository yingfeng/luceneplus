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
#include <map>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.index_deletion_policy;

import lucene_core;

using namespace lucene;

namespace {

/// A commit point that only counts how often it was asked to delete itself.
class FakeIndexCommit final : public IndexCommit {
public:
    FakeIndexCommit(int64_t generation, const std::string &segments_file, Directory &directory)
            : generation_(generation), segments_file_(segments_file), directory_(directory) {}

    [[nodiscard]] const std::string &segments_file_name() const override { return segments_file_; }
    [[nodiscard]] const std::vector<std::string> &file_names() const override { return files_; }
    [[nodiscard]] Directory &directory() const override { return directory_; }

    [[nodiscard]] VoidResult delete_commit() const override {
        ++delete_calls_;
        deleted_ = true;
        return {};
    }

    [[nodiscard]] bool is_deleted() const override { return deleted_; }
    [[nodiscard]] size_t segment_count() const override { return 1; }
    [[nodiscard]] int64_t generation() const override { return generation_; }
    [[nodiscard]] const std::map<std::string, std::string> &user_data() const override { return user_data_; }
    [[nodiscard]] std::string to_string() const override {
        return "FakeIndexCommit(" + segments_file_ + ")";
    }

    [[nodiscard]] int32_t delete_calls() const { return delete_calls_; }

private:
    int64_t generation_;
    std::string segments_file_;
    Directory &directory_;
    std::vector<std::string> files_;
    std::map<std::string, std::string> user_data_;
    mutable bool deleted_{false};
    mutable int32_t delete_calls_{0};
};

} // namespace

TEST(IndexDeletionPolicyTest, NoPolicyDeletesNothing) {
    ByteBuffersDirectory dir;
    auto first = std::make_shared<FakeIndexCommit>(1, "segments_1", dir);
    auto second = std::make_shared<FakeIndexCommit>(2, "segments_2", dir);
    std::vector<std::shared_ptr<IndexCommit>> commits{first, second};

    NoDeletionPolicy policy;
    EXPECT_TRUE(policy.on_init(commits).has_value());
    EXPECT_TRUE(policy.on_commit(commits).has_value());
    EXPECT_FALSE(first->is_deleted());
    EXPECT_FALSE(second->is_deleted());
    EXPECT_EQ(policy.to_string(), "NoDeletionPolicy");
}

TEST(IndexDeletionPolicyTest, KeepOnlyLastDeletesTheOlderCommits) {
    ByteBuffersDirectory dir;
    auto first = std::make_shared<FakeIndexCommit>(1, "segments_1", dir);
    auto second = std::make_shared<FakeIndexCommit>(2, "segments_2", dir);
    auto third = std::make_shared<FakeIndexCommit>(3, "segments_3", dir);
    std::vector<std::shared_ptr<IndexCommit>> commits{first, second, third};

    KeepOnlyLastCommitDeletionPolicy policy;
    ASSERT_TRUE(policy.on_commit(commits).has_value());

    EXPECT_TRUE(first->is_deleted());
    EXPECT_TRUE(second->is_deleted());
    EXPECT_FALSE(third->is_deleted());
    EXPECT_EQ(first->delete_calls(), 1);
    EXPECT_EQ(policy.to_string(), "KeepOnlyLastCommitDeletionPolicy");
}

TEST(IndexDeletionPolicyTest, KeepOnlyLastBehavesTheSameOnInit) {
    ByteBuffersDirectory dir;
    auto only = std::make_shared<FakeIndexCommit>(1, "segments_1", dir);
    auto latest = std::make_shared<FakeIndexCommit>(2, "segments_2", dir);
    std::vector<std::shared_ptr<IndexCommit>> commits{only, latest};

    KeepOnlyLastCommitDeletionPolicy policy;
    ASSERT_TRUE(policy.on_init(commits).has_value());
    EXPECT_TRUE(only->is_deleted());
    EXPECT_FALSE(latest->is_deleted());

    // An empty list (a brand new index) is fine.
    EXPECT_TRUE(policy.on_init({}).has_value());
    EXPECT_TRUE(policy.on_commit({}).has_value());
}

TEST(IndexDeletionPolicyTest, KeepOnlyLastRejectsNullCommits) {
    KeepOnlyLastCommitDeletionPolicy policy;
    std::vector<std::shared_ptr<IndexCommit>> commits{nullptr, nullptr};
    auto committed = policy.on_commit(commits);
    ASSERT_FALSE(committed.has_value());
    EXPECT_EQ(committed.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(IndexCommitTest, ComparesCommits) {
    ByteBuffersDirectory dir;
    ByteBuffersDirectory other_dir;
    FakeIndexCommit first(1, "segments_1", dir);
    FakeIndexCommit second(2, "segments_2", dir);
    FakeIndexCommit same_generation(1, "segments_1", dir);
    FakeIndexCommit other_directory(1, "segments_1", other_dir);

    EXPECT_TRUE(is_same_commit(first, same_generation));
    EXPECT_FALSE(is_same_commit(first, second));
    // Different directory objects are never the same commit, even with the same
    // generation (see the deviation on `is_same_commit`).
    EXPECT_FALSE(is_same_commit(first, other_directory));

    EXPECT_LT(cmp_commit(first, second), 0);
    EXPECT_GT(cmp_commit(second, first), 0);
    EXPECT_EQ(cmp_commit(first, same_generation), 0);
}
