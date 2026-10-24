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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.check_index;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTitleField = "title";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

void add_text(Document &doc, std::string_view name, std::string value) {
    doc.add(std::make_unique<Field>(std::string(name), FieldValue{std::move(value)}, text_type()));
}

FieldType term_vector_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    EXPECT_TRUE(type.set_store_term_vectors(true).has_value());
    EXPECT_TRUE(type.set_store_term_vector_positions(true).has_value());
    return type;
}

Document make_doc(std::string id, std::string title) {
    Document doc;
    add_text(doc, kIdField, std::move(id));
    add_text(doc, kTitleField, std::move(title));
    return doc;
}

/// Builds an index from `docs` in one commit.  Variadic because [`Document`] is
/// move-only, so it cannot go through a `std::vector`.
template <class... Docs>
void write_docs(Directory &directory, Docs &&...docs) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    auto add = [&writer](Document &&doc) {
        ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    };
    (add(std::forward<Docs>(docs)), ...);
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

} // namespace

TEST(CheckIndexTest, AnEmptyDirectoryIsReportedAsMissingSegmentsFile) {
    ByteBuffersDirectory directory;
    CheckIndex check_index(directory);

    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_TRUE(result.value().missing_segments_file);
    EXPECT_FALSE(result.value().clean());
    EXPECT_EQ(result.value().to_string(), "no segments file");
}

TEST(CheckIndexTest, ACleanIndexPasses) {
    ByteBuffersDirectory directory;
    write_docs(directory, make_doc("a", "hello world"), make_doc("b", "hello lucene"), make_doc("c", "world"));

    CheckIndex check_index(directory);
    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

    const CheckIndexResult &status = result.value();
    EXPECT_TRUE(status.clean()) << status.to_string();
    EXPECT_FALSE(status.missing_segments_file);
    EXPECT_EQ(status.segments_file_name.rfind("segments_", 0), 0U);
    EXPECT_GE(status.generation, 1);
    ASSERT_EQ(status.segments.size(), 1U);
    EXPECT_EQ(status.segments[0].segment_name, "_0");
    EXPECT_EQ(status.segments[0].max_doc, 3);
    EXPECT_EQ(status.segments[0].num_docs, 3);
    EXPECT_TRUE(status.segments[0].clean());
    EXPECT_EQ(status.total_docs(), 3);
    EXPECT_NE(status.to_string().find("OK"), std::string::npos);
    EXPECT_EQ(&check_index.directory(), &directory);
}

TEST(CheckIndexTest, TheTermDictionaryAndPostingsAreWalked) {
    ByteBuffersDirectory directory;
    write_docs(directory, make_doc("a", "hello world"), make_doc("b", "hello lucene"), make_doc("c", "world"));

    CheckIndex check_index(directory);
    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    ASSERT_EQ(result.value().segments.size(), 1U);
    const SegmentCheckResult &segment = result.value().segments[0];
    EXPECT_TRUE(segment.clean()) << segment.to_string();

    // `id` has a/b/c, `title` has hello/lucene/world.
    EXPECT_EQ(segment.term_count, 6);
    // `id` contributes 3 postings, `title` 2 + 1 + 2.
    EXPECT_EQ(segment.posting_count, 8);
    EXPECT_NE(segment.to_string().find("terms=6"), std::string::npos);
    EXPECT_NE(segment.to_string().find("postings=8"), std::string::npos);
}

TEST(CheckIndexTest, TermVectorsAreCrossCheckedAgainstThePostings) {
    ByteBuffersDirectory directory;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

        Document first;
        add_text(first, kIdField, "a");
        first.add(std::make_unique<Field>(std::string(kTitleField), FieldValue{std::string("hello world")},
                                          term_vector_type()));
        ASSERT_TRUE(writer.value()->add_document(std::move(first)).has_value());

        Document second;
        add_text(second, kIdField, "b");
        // "hello" twice, so the frequency comparison has something to check.
        second.add(std::make_unique<Field>(std::string(kTitleField), FieldValue{std::string("hello hello lucene")},
                                           term_vector_type()));
        ASSERT_TRUE(writer.value()->add_document(std::move(second)).has_value());

        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    CheckIndex check_index(directory);
    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    ASSERT_EQ(result.value().segments.size(), 1U);
    EXPECT_TRUE(result.value().clean()) << result.value().to_string();
    // `id` has a/b, `title` hello/lucene/world.
    EXPECT_EQ(result.value().segments[0].term_count, 5);
}

TEST(CheckIndexTest, TermsOutsideTheIntersectProbesAreHandled) {
    // `x`, `y`, `z`, `m`, `n` are all outside the `[a-e]` the probe automaton looks
    // for, so the intersection has to come back empty while the scan walks every
    // term.
    ByteBuffersDirectory directory;
    write_docs(directory, make_doc("xyz", "mmm nnn"), make_doc("qqq", "mmm"));

    CheckIndex check_index(directory);
    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    ASSERT_EQ(result.value().segments.size(), 1U);
    EXPECT_TRUE(result.value().clean()) << result.value().to_string();
    EXPECT_EQ(result.value().segments[0].term_count, 4) << "xyz, qqq, mmm, nnn";
}

TEST(CheckIndexTest, DeletedDocumentsAreAccountedFor) {
    ByteBuffersDirectory directory;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("a", "keep me")).has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("b", "drop me")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()
                            ->delete_documents(Term(std::string(kIdField), BytesRef::from_string("b")))
                            .has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    CheckIndex check_index(directory);
    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_TRUE(result.value().clean()) << result.value().to_string();
    // Each commit flushed its own segment and the writer drops a segment whose
    // documents are all deleted, so only the surviving total is stable here.
    for (const SegmentCheckResult &segment : result.value().segments) {
        EXPECT_TRUE(segment.clean()) << segment.to_string();
    }
    EXPECT_EQ(result.value().total_docs(), 1) << "one of the two documents was deleted";
}

TEST(CheckIndexTest, ASegmentWithoutItsFilesIsReported) {
    ByteBuffersDirectory directory;
    write_docs(directory, make_doc("a", "hello world"));

    std::string victim;
    for (const std::string &file : directory.list_all().value()) {
        if (file.starts_with("_0") && file.ends_with(".fdt")) {
            victim = file;
        }
    }
    ASSERT_FALSE(victim.empty()) << "the stored fields file must exist";
    ASSERT_TRUE(directory.delete_file(victim).has_value());

    CheckIndex check_index(directory);
    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

    const CheckIndexResult &status = result.value();
    EXPECT_FALSE(status.clean()) << "a missing segment file must be noticed";
    ASSERT_EQ(status.segments.size(), 1U);
    EXPECT_FALSE(status.segments[0].clean());
    EXPECT_EQ(status.segments[0].segment_name, "_0");
    EXPECT_FALSE(status.segments[0].failures.empty());
    EXPECT_NE(status.to_string().find("FAILED"), std::string::npos);
}

TEST(CheckIndexTest, ASpecificCommitCanBeChecked) {
    ByteBuffersDirectory directory;
    write_docs(directory, make_doc("a", "hello"));

    auto infos = SegmentInfos::read_latest(directory);
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());
    SegmentInfos mutable_infos = infos.value();
    KeepOnlyLastCommitDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, mutable_infos);
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());
    ASSERT_EQ(deleter.value()->commits().size(), 1U);

    CheckIndex check_index(directory);
    auto result = check_index.check(*deleter.value()->commits()[0]);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_TRUE(result.value().clean()) << result.value().to_string();
    EXPECT_EQ(result.value().generation, deleter.value()->commits()[0]->generation());
    EXPECT_EQ(result.value().segments_file_name, deleter.value()->commits()[0]->segments_file_name());
    EXPECT_EQ(result.value().total_docs(), 1);
}

TEST(CheckIndexTest, EverySegmentIsChecked) {
    ByteBuffersDirectory directory;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        config.set_merge_policy(NoMergePolicy::create());
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->add_document(make_doc("a", "first")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->add_document(make_doc("b", "second")).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        EXPECT_EQ(writer.value()->segment_infos().segments.size(), 2U) << "no merging, two segments";
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    CheckIndex check_index(directory);
    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_TRUE(result.value().clean()) << result.value().to_string();
    EXPECT_EQ(result.value().segments.size(), 2U);
    EXPECT_EQ(result.value().total_docs(), 2);
    for (const SegmentCheckResult &segment : result.value().segments) {
        EXPECT_TRUE(segment.clean()) << segment.to_string();
        EXPECT_EQ(segment.num_docs, 1);
    }
}

TEST(CheckIndexTest, ASortedIndexWithEverythingPasses) {
    // One segment that is index sorted, has stored fields, term vectors, numeric
    // and sorted-numeric doc values, and one deleted document: every check has to
    // agree with every other one.
    ByteBuffersDirectory directory;
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto sort_field = SortField::create("rank", SortFieldType::kLong);
        ASSERT_TRUE(sort_field.has_value()) << (sort_field ? "" : sort_field.error().to_string());
        std::vector<SortField> sort_fields;
        sort_fields.push_back(sort_field.value());
        auto sort = Sort::with_fields(std::move(sort_fields));
        ASSERT_TRUE(sort.has_value()) << (sort ? "" : sort.error().to_string());
        config.set_index_sort(sort.value());

        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

        int64_t rank = 30;
        const std::pair<const char *, const char *> docs[] = {{"a", "hello world"}, {"b", "hello lucene"}, {"c", "world"}};
        for (const auto &[id, title] : docs) {
            Document doc;
            doc.add(std::make_unique<Field>(std::string(kIdField), FieldValue{std::string(id)}, text_type()));
            doc.add(std::make_unique<Field>(std::string(kTitleField), FieldValue{std::string(title)},
                                            term_vector_type()));
            auto numeric = NumericDocValuesField::create("rank", rank);
            ASSERT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
            doc.add(std::make_unique<Field>(std::move(numeric.value())));
            auto sorted_numeric = SortedNumericDocValuesField::create("sizes", rank / 10);
            ASSERT_TRUE(sorted_numeric.has_value()) << (sorted_numeric ? "" : sorted_numeric.error().to_string());
            doc.add(std::make_unique<Field>(std::move(sorted_numeric.value())));
            ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
            rank -= 10;
        }
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->delete_documents(Term(std::string(kIdField), BytesRef::from_string("b")))
                            .has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    CheckIndex check_index(directory);
    auto result = check_index.check();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_TRUE(result.value().clean()) << result.value().to_string();
    ASSERT_FALSE(result.value().segments.empty());
    for (const SegmentCheckResult &segment : result.value().segments) {
        EXPECT_TRUE(segment.clean()) << segment.to_string();
        EXPECT_GT(segment.term_count, 0) << "the postings walk ran";
    }
    EXPECT_EQ(result.value().total_docs(), 2);

    // The segment is still sorted by `rank`, and the deleted document is gone.
    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_FALSE(leaves.value().empty());
    EXPECT_NE(leaves.value()[0].reader()->leaf_meta_data().sort(), nullptr);
}
