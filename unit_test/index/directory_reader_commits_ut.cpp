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
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.directory_reader_commits;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_doc(std::string id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, text_type()));
    return doc;
}

} // namespace

TEST(DirectoryReaderCommitsTest, IndexExistsIsFalseForAnEmptyDirectory) {
    ByteBuffersDirectory directory;
    auto exists = DirectoryReader::index_exists(directory);
    ASSERT_TRUE(exists.has_value()) << (exists ? "" : exists.error().to_string());
    EXPECT_FALSE(exists.value());

    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value()) << (commits ? "" : commits.error().to_string());
    EXPECT_TRUE(commits.value().empty()) << "a directory without a commit has no commit points";
}

TEST(DirectoryReaderCommitsTest, ListsEveryCommitOldestFirst) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    // Three commits, each with one more document than the last.
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("3")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    // A closed writer keeps the commits it wrote.
    ASSERT_TRUE(writer.value()->close().has_value());

    auto exists = DirectoryReader::index_exists(directory);
    ASSERT_TRUE(exists.has_value());
    EXPECT_TRUE(exists.value());

    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value()) << (commits ? "" : commits.error().to_string());
    ASSERT_GE(commits.value().size(), 3U) << "each commit keeps its own segments file";

    for (size_t i = 1; i < commits.value().size(); ++i) {
        EXPECT_LT(commits.value()[i - 1]->generation(), commits.value()[i]->generation())
                << "the commit points are ordered oldest first";
    }

    // The newest commit is the one a directory reader opens.
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(commits.value().back()->generation(), reader.value()->segment_infos().generation);
    EXPECT_EQ(commits.value().back()->segment_count(), reader.value()->segment_infos().segments.size());

    // Each commit names the files it references, including its own segments file.
    for (const std::shared_ptr<IndexCommit> &commit : commits.value()) {
        const std::vector<std::string> &files = commit->file_names();
        EXPECT_FALSE(files.empty());
        EXPECT_NE(std::find(files.begin(), files.end(), commit->segments_file_name()), files.end())
                << "a commit lists its own segments file";
        for (const std::string &file : files) {
            auto length = directory.file_length(file);
            EXPECT_TRUE(length.has_value()) << file << " must exist";
        }
        EXPECT_FALSE(commit->is_deleted());
        EXPECT_TRUE(commit->to_string().find(commit->segments_file_name()) != std::string::npos);
        EXPECT_EQ(&commit->directory(), &directory);
    }
}

TEST(DirectoryReaderCommitsTest, TheCommitsAreReadOnly) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value());
    // `commit` and `close` both move the generation on, so the exact count is not
    // the point here; the commit points must be read-only.
    ASSERT_GE(commits.value().size(), 1U);

    auto deleted = commits.value()[0]->delete_commit();
    ASSERT_FALSE(deleted.has_value());
    EXPECT_EQ(deleted.error().kind(), ErrorKind::kUnsupportedOperation);
    EXPECT_FALSE(commits.value()[0]->is_deleted());
}

TEST(DirectoryReaderCommitsTest, OpensAnOlderCommit) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value()) << (commits ? "" : commits.error().to_string());
    ASSERT_GE(commits.value().size(), 2U);

    // The newest commit sees both documents, the oldest one only the first.
    auto newest = DirectoryReader::open_commit(*commits.value().back());
    ASSERT_TRUE(newest.has_value()) << (newest ? "" : newest.error().to_string());
    EXPECT_EQ(newest.value()->segment_infos().generation, commits.value().back()->generation());

    auto oldest = DirectoryReader::open_commit(*commits.value().front());
    ASSERT_TRUE(oldest.has_value()) << (oldest ? "" : oldest.error().to_string());
    EXPECT_LT(oldest.value()->num_docs().value(), newest.value()->num_docs().value());

    // Searching the older point in time finds fewer documents.
    IndexSearcher old_searcher(oldest.value());
    const TermQuery query(Term("id", BytesRef::from_string("2")));
    EXPECT_EQ(old_searcher.count(query).value(), 0) << "the second document did not exist yet";
    IndexSearcher new_searcher(newest.value());
    EXPECT_EQ(new_searcher.count(query).value(), 1);
}

TEST(DirectoryReaderCommitsTest, ReopeningForTheSameCommitAnswersNothing) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value());
    ASSERT_GE(commits.value().size(), 2U);
    const std::shared_ptr<IndexCommit> newest = commits.value().back();
    const std::shared_ptr<IndexCommit> oldest = commits.value().front();

    auto reader = DirectoryReader::open_commit(*newest);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    // The same commit: nothing to do.
    auto unchanged = reader.value()->open_if_changed_with_commit(*newest);
    ASSERT_TRUE(unchanged.has_value()) << (unchanged ? "" : unchanged.error().to_string());
    EXPECT_EQ(unchanged.value(), nullptr);

    // An older commit is a change, and yields that point in time.
    auto changed = reader.value()->open_if_changed_with_commit(*oldest);
    ASSERT_TRUE(changed.has_value()) << (changed ? "" : changed.error().to_string());
    ASSERT_NE(changed.value(), nullptr);
    EXPECT_EQ(changed.value()->segment_infos().generation, oldest->generation());
}

TEST(DirectoryReaderCommitsTest, RefusesAnIndexOlderThanTheMinimumVersion) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto commits = DirectoryReader::list_commits(directory);
    ASSERT_TRUE(commits.has_value());
    ASSERT_FALSE(commits.value().empty());
    const IndexCommit &commit = *commits.value().back();

    // The port writes the current version, so any supported minimum is fine.
    auto supported = DirectoryReader::open_with_version(commit, LUCENE_MIN_SUPPORTED_VERSION.major);
    ASSERT_TRUE(supported.has_value()) << (supported ? "" : supported.error().to_string());
    EXPECT_EQ(supported.value()->num_docs().value(), 1);

    // A minimum above the version that created the index is refused.
    auto too_new = DirectoryReader::open_with_version(commit, LUCENE_CURRENT_VERSION.major + 1);
    ASSERT_FALSE(too_new.has_value());
    EXPECT_EQ(too_new.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(too_new.error().message().find("older than the minimum supported major version"), std::string::npos);
}
