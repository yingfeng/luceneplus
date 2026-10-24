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

module lucene_core:ut.persistent_snapshot_deletion_policy;

import lucene_core;

using namespace lucene;

namespace {

/// A commit point that only counts how often it was asked to delete itself.
class FakeCommit final : public IndexCommit {
public:
    FakeCommit(int64_t generation, Directory &directory) : generation_(generation), directory_(directory) {
        segments_file_ = "segments_" + std::to_string(generation);
        files_ = {segments_file_};
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
    [[nodiscard]] size_t segment_count() const override { return 1; }
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

/// A directory that refuses to create the snapshot files.
class FailingSnapshotDirectory final : public Directory {
public:
    using Directory::create_output;
    using Directory::open_input;

    explicit FailingSnapshotDirectory(Directory &delegate) : delegate_(delegate) {}

    [[nodiscard]] Result<std::vector<std::string>> list_all() const override { return delegate_.list_all(); }
    [[nodiscard]] VoidResult delete_file(std::string_view name) override { return delegate_.delete_file(name); }
    [[nodiscard]] Result<size_t> file_length(std::string_view name) const override {
        return delegate_.file_length(name);
    }
    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                    const IOContext &context) override {
        if (name.starts_with(SNAPSHOTS_PREFIX)) {
            return std::unexpected(LuceneError::io("this directory cannot write snapshots"));
        }
        return delegate_.create_output(name, context);
    }
    [[nodiscard]] Result<CreatedOutput> create_temp_output(std::string_view prefix, std::string_view suffix,
                                                          const IOContext &context) override {
        return delegate_.create_temp_output(prefix, suffix, context);
    }
    [[nodiscard]] VoidResult sync(std::span<const std::string> names) override { return delegate_.sync(names); }
    [[nodiscard]] VoidResult rename(std::string_view source, std::string_view dest) override {
        return delegate_.rename(source, dest);
    }
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                const IOContext &context) const override {
        return delegate_.open_input(name, context);
    }
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override {
        return delegate_.obtain_lock(name);
    }
    [[nodiscard]] std::string to_string() const override { return "FailingSnapshotDirectory"; }

private:
    Directory &delegate_;
};

std::vector<std::shared_ptr<IndexCommit>> commits_of(std::initializer_list<std::shared_ptr<IndexCommit>> list) {
    return std::vector<std::shared_ptr<IndexCommit>>(list);
}

bool contains(const std::vector<std::string> &names, std::string_view name) {
    for (const std::string &candidate : names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

void write_garbage(Directory &directory, std::string_view name) {
    auto output = directory.create_output(name, io_context_default());
    ASSERT_TRUE(output.has_value()) << (output ? "" : output.error().to_string());
    ASSERT_TRUE(output.value()->write_byte(0x2A).has_value());
    ASSERT_TRUE(output.value()->close().has_value());
}

} // namespace

TEST(PersistentSnapshotDeletionPolicyTest, ANewPolicyHasNoSavedSnapshots) {
    ByteBuffersDirectory directory;
    auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
    ASSERT_TRUE(policy.has_value()) << (policy ? "" : policy.error().to_string());

    EXPECT_FALSE(policy.value()->last_save_file().has_value());
    EXPECT_EQ(policy.value()->get_snapshot_count(), 0);
    EXPECT_EQ(policy.value()->to_string(), "PersistentSnapshotDeletionPolicy(NoDeletionPolicy)");
    EXPECT_EQ(&policy.value()->directory(), &directory);
}

TEST(PersistentSnapshotDeletionPolicyTest, SnapshotWritesTheFile) {
    ByteBuffersDirectory directory;
    auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
    ASSERT_TRUE(policy.has_value());

    auto first = std::make_shared<FakeCommit>(1, directory);
    ASSERT_TRUE(policy.value()->on_init(commits_of({first})).has_value());

    auto snapshot = policy.value()->snapshot();
    ASSERT_TRUE(snapshot.has_value()) << (snapshot ? "" : snapshot.error().to_string());
    EXPECT_EQ(snapshot.value()->generation(), 1);
    EXPECT_EQ(policy.value()->last_save_file(), std::optional<std::string>("snapshots_0"));
    EXPECT_TRUE(contains(directory.list_all().value(), "snapshots_0"));
    EXPECT_EQ(policy.value()->get_snapshot_count(), 1);
}

TEST(PersistentSnapshotDeletionPolicyTest, OnlyTheNewestSnapshotFileIsKept) {
    ByteBuffersDirectory directory;
    auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
    ASSERT_TRUE(policy.has_value());
    auto first = std::make_shared<FakeCommit>(1, directory);
    ASSERT_TRUE(policy.value()->on_init(commits_of({first})).has_value());

    ASSERT_TRUE(policy.value()->snapshot().has_value());
    ASSERT_TRUE(policy.value()->snapshot().has_value());

    EXPECT_EQ(policy.value()->last_save_file(), std::optional<std::string>("snapshots_1"));
    const std::vector<std::string> files = directory.list_all().value();
    EXPECT_TRUE(contains(files, "snapshots_1"));
    EXPECT_FALSE(contains(files, "snapshots_0")) << "the superseded file is removed";
}

TEST(PersistentSnapshotDeletionPolicyTest, ReopeningRestoresTheSnapshots) {
    ByteBuffersDirectory directory;
    {
        auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<KeepOnlyLastCommitDeletionPolicy>(),
                                                                directory);
        ASSERT_TRUE(policy.has_value());
        auto first = std::make_shared<FakeCommit>(1, directory);
        ASSERT_TRUE(policy.value()->on_init(commits_of({first})).has_value());
        ASSERT_TRUE(policy.value()->snapshot().has_value());
    }

    auto reopened = create_persistent_snapshot_deletion_policy(std::make_unique<KeepOnlyLastCommitDeletionPolicy>(),
                                                              directory);
    ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
    EXPECT_EQ(reopened.value()->get_snapshot_count(), 1);
    EXPECT_EQ(reopened.value()->last_save_file(), std::optional<std::string>("snapshots_0"));

    // The restored snapshot still protects its commit.
    auto first = std::make_shared<FakeCommit>(1, directory);
    auto second = std::make_shared<FakeCommit>(2, directory);
    ASSERT_TRUE(reopened.value()->on_init(commits_of({first})).has_value());
    ASSERT_TRUE(reopened.value()->on_commit(commits_of({first, second})).has_value());
    EXPECT_FALSE(first->is_deleted());
    EXPECT_EQ(first->delete_calls(), 0);
}

TEST(PersistentSnapshotDeletionPolicyTest, ReleasePersistsTheNewState) {
    ByteBuffersDirectory directory;
    {
        auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
        ASSERT_TRUE(policy.has_value());
        auto first = std::make_shared<FakeCommit>(1, directory);
        ASSERT_TRUE(policy.value()->on_init(commits_of({first})).has_value());
        auto snapshot = policy.value()->snapshot();
        ASSERT_TRUE(snapshot.has_value());
        ASSERT_TRUE(policy.value()->release(*snapshot.value()).has_value());
        EXPECT_EQ(policy.value()->get_snapshot_count(), 0);
    }

    auto reopened = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(reopened.value()->get_snapshot_count(), 0);
    EXPECT_EQ(reopened.value()->get_snapshots().size(), 0U);
}

TEST(PersistentSnapshotDeletionPolicyTest, AppendModeNeedsStoredSnapshots) {
    ByteBuffersDirectory directory;
    auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory,
                                                            SnapshotOpenMode::kAppend);
    ASSERT_FALSE(policy.has_value());
    EXPECT_EQ(policy.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(policy.error().message().find("no snapshots stored in this directory"), std::string::npos);
}

TEST(PersistentSnapshotDeletionPolicyTest, CreateModeDropsTheStoredSnapshots) {
    ByteBuffersDirectory directory;
    {
        auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
        ASSERT_TRUE(policy.has_value());
        auto first = std::make_shared<FakeCommit>(1, directory);
        ASSERT_TRUE(policy.value()->on_init(commits_of({first})).has_value());
        ASSERT_TRUE(policy.value()->snapshot().has_value());
    }
    ASSERT_TRUE(contains(directory.list_all().value(), "snapshots_0"));

    auto fresh = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory,
                                                           SnapshotOpenMode::kCreate);
    ASSERT_TRUE(fresh.has_value()) << (fresh ? "" : fresh.error().to_string());
    EXPECT_EQ(fresh.value()->get_snapshot_count(), 0);
    EXPECT_FALSE(contains(directory.list_all().value(), "snapshots_0"));
}

TEST(PersistentSnapshotDeletionPolicyTest, AnUnreadableFileLosesToTheReadableOne) {
    ByteBuffersDirectory directory;
    {
        auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
        ASSERT_TRUE(policy.has_value());
        auto first = std::make_shared<FakeCommit>(1, directory);
        ASSERT_TRUE(policy.value()->on_init(commits_of({first})).has_value());
        ASSERT_TRUE(policy.value()->snapshot().has_value());
    }
    // A crashed writer could leave a truncated file behind.
    write_garbage(directory, "snapshots_1");

    auto reopened = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
    ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
    EXPECT_EQ(reopened.value()->get_snapshot_count(), 1) << "the readable snapshot survives";
    const std::vector<std::string> files = directory.list_all().value();
    EXPECT_FALSE(contains(files, "snapshots_1")) << "the broken file is removed";
    EXPECT_EQ(reopened.value()->last_save_file(), std::optional<std::string>("snapshots_0"));
}

TEST(PersistentSnapshotDeletionPolicyTest, AFailedWriteRollsTheSnapshotBack) {
    ByteBuffersDirectory storage;
    FailingSnapshotDirectory directory(storage);
    auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
    ASSERT_TRUE(policy.has_value()) << (policy ? "" : policy.error().to_string());

    auto first = std::make_shared<FakeCommit>(1, storage);
    ASSERT_TRUE(policy.value()->on_init(commits_of({first})).has_value());

    auto snapshot = policy.value()->snapshot();
    ASSERT_FALSE(snapshot.has_value());
    EXPECT_EQ(snapshot.error().kind(), ErrorKind::kIo);
    EXPECT_EQ(policy.value()->get_snapshot_count(), 0) << "the in-memory snapshot is rolled back";
}

TEST(PersistentSnapshotDeletionPolicyTest, SnapshotWithoutAWriterIsMisuse) {
    ByteBuffersDirectory directory;
    auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<NoDeletionPolicy>(), directory);
    ASSERT_TRUE(policy.has_value());

    auto snapshot = policy.value()->snapshot();
    ASSERT_FALSE(snapshot.has_value());
    EXPECT_EQ(snapshot.error().kind(), ErrorKind::kIllegalState);
}

TEST(PersistentSnapshotDeletionPolicyTest, CommitsAreStillPrunedByTheWrappedPolicy) {
    ByteBuffersDirectory directory;
    auto policy = create_persistent_snapshot_deletion_policy(std::make_unique<KeepOnlyLastCommitDeletionPolicy>(),
                                                            directory);
    ASSERT_TRUE(policy.has_value());

    auto first = std::make_shared<FakeCommit>(1, directory);
    auto second = std::make_shared<FakeCommit>(2, directory);
    ASSERT_TRUE(policy.value()->on_init(commits_of({first})).has_value());
    ASSERT_TRUE(policy.value()->on_commit(commits_of({first, second})).has_value());

    EXPECT_TRUE(first->is_deleted()) << "no snapshot holds the first commit";
    EXPECT_FALSE(second->is_deleted());
    EXPECT_EQ(policy.value()->base().primary().to_string(), "KeepOnlyLastCommitDeletionPolicy");
}
