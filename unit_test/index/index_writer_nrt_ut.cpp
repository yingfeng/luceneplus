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

module lucene_core:ut.index_writer_nrt;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

Document make_doc(std::string id, std::string title) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, text_type()));
    doc.add(std::make_unique<Field>("title", FieldValue{std::move(title)}, text_type()));
    return doc;
}

int32_t count_of(const std::shared_ptr<DirectoryReader> &reader, std::string_view term) {
    IndexSearcher searcher(reader);
    const TermQuery query(Term("title", BytesRef::from_string(term)));
    auto count = searcher.count(query);
    EXPECT_TRUE(count.has_value()) << (count ? "" : count.error().to_string());
    return count.has_value() ? count.value() : -1;
}

} // namespace

TEST(IndexWriterNrtTest, TheReaderSeesUncommittedDocuments) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("1", "hello world")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2", "hello")).has_value());

    // No commit yet: the near-real-time reader must still see both documents.
    auto reader = writer.value()->get_reader();
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 2);
    EXPECT_EQ(count_of(reader.value(), "hello"), 2);

    // Nothing is committed yet, so an ordinary directory reader finds no index.
    auto committed = DirectoryReader::open(directory);
    ASSERT_FALSE(committed.has_value()) << "an uncommitted index has no segments file";

    ASSERT_TRUE(writer.value()->commit().has_value());
    auto after_commit = DirectoryReader::open(directory);
    ASSERT_TRUE(after_commit.has_value());
    EXPECT_EQ(after_commit.value()->num_docs().value(), 2);

    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(IndexWriterNrtTest, TheReaderSeesBufferedDeletes) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("1", "hello")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2", "hello")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    ASSERT_TRUE(writer.value()->delete_documents(Term("id", BytesRef::from_string("1"))).has_value());

    auto reader = writer.value()->get_reader();
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->num_docs().value(), 1) << "the buffered delete is applied";
    EXPECT_EQ(count_of(reader.value(), "hello"), 1);

    // Committing afterwards keeps the same view.
    ASSERT_TRUE(writer.value()->commit().has_value());
    auto committed = DirectoryReader::open(directory);
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(committed.value()->num_docs().value(), 1);

    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(IndexWriterNrtTest, TheReaderIsASnapshot) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("1", "first")).has_value());
    auto first = writer.value()->get_reader();
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    EXPECT_EQ(first.value()->num_docs().value(), 1);

    ASSERT_TRUE(writer.value()->add_document(make_doc("2", "second")).has_value());
    EXPECT_EQ(first.value()->num_docs().value(), 1) << "the old snapshot does not change";

    auto second = writer.value()->get_reader();
    ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
    EXPECT_EQ(second.value()->num_docs().value(), 2);

    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(IndexWriterNrtTest, ClosingTheReaderLeavesTheWriterUsable) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("1", "hello")).has_value());
    {
        auto reader = writer.value()->get_reader();
        ASSERT_TRUE(reader.has_value());
        ASSERT_TRUE(reader.value()->close().has_value());
    }

    // The writer still accepts documents and commits.
    ASSERT_TRUE(writer.value()->add_document(make_doc("2", "world")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto committed = DirectoryReader::open(directory);
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(committed.value()->num_docs().value(), 2);

    ASSERT_TRUE(writer.value()->close().has_value());
}

TEST(IndexWriterNrtTest, AClosedWriterHasNoReader) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = writer.value()->get_reader();
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kAlreadyClosed);
}

TEST(IndexWriterNrtTest, DeletesStayBufferedWhenTheyAreNotApplied) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("1", "hello")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2", "hello")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->delete_documents(Term("id", BytesRef::from_string("1"))).has_value());

    // The expert switch: the reader still shows both documents.
    auto without_deletes = writer.value()->get_reader(/*apply_all_deletes=*/false, /*write_all_deletes=*/false);
    ASSERT_TRUE(without_deletes.has_value()) << (without_deletes ? "" : without_deletes.error().to_string());
    EXPECT_EQ(without_deletes.value()->num_docs().value(), 2);
    EXPECT_EQ(count_of(without_deletes.value(), "hello"), 2);

    // With the deletes applied the view shrinks.
    auto with_deletes = writer.value()->get_reader(/*apply_all_deletes=*/true, /*write_all_deletes=*/true);
    ASSERT_TRUE(with_deletes.has_value()) << (with_deletes ? "" : with_deletes.error().to_string());
    EXPECT_EQ(with_deletes.value()->num_docs().value(), 1);

    ASSERT_TRUE(writer.value()->close().has_value());
}
