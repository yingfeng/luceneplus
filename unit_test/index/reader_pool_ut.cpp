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

module lucene_core:ut.reader_pool;

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

/// Writes two documents and commits once, so the index has one segment.
void write_index(ByteBuffersDirectory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("2")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

/// The commit state the directory holds now.
SegmentInfos current_infos(ByteBuffersDirectory &directory) {
    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value()->segment_infos() : SegmentInfos{};
}

} // namespace

TEST(ReaderPoolTest, ReusesTheSameReader) {
    ByteBuffersDirectory directory;
    write_index(directory);
    const SegmentInfos infos = current_infos(directory);
    ASSERT_EQ(infos.segments.size(), 1U);

    ReaderPool pool(directory);
    SegmentCommitInfo segment = infos.segments[0];
    auto first = pool.get(segment, infos.index_created_version_major, io_context_default());
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    EXPECT_EQ(pool.num_cached(), 1U);
    EXPECT_TRUE(pool.is_cached(segment.info.name));
    EXPECT_EQ(pool.find(segment.info.name).get(), first.value().get());

    auto second = pool.get(segment, infos.index_created_version_major, io_context_default());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value().get(), first.value().get()) << "the reader is cached";
    EXPECT_EQ(second.value()->num_docs().value(), 2);
}

TEST(ReaderPoolTest, FollowsARewrittenSegment) {
    ByteBuffersDirectory directory;
    write_index(directory);

    SegmentInfos infos = current_infos(directory);
    ASSERT_EQ(infos.segments.size(), 1U);

    ReaderPool pool(directory);
    SegmentCommitInfo before = infos.segments[0];
    const std::string old_name = before.info.name;
    auto old_reader = pool.get(before, infos.index_created_version_major, io_context_default());
    ASSERT_TRUE(old_reader.has_value()) << (old_reader ? "" : old_reader.error().to_string());
    EXPECT_EQ(old_reader.value()->num_docs().value(), 2);
    EXPECT_TRUE(pool.is_cached(old_name));

    // A commit that applies a delete rewrites the segment under a new name, so the
    // cached reader describes a segment that no longer exists.
    {
        WhitespaceAnalyzer analyzer;
        IndexWriterConfig config(analyzer);
        auto writer = IndexWriter::open(directory, config);
        ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
        ASSERT_TRUE(writer.value()->delete_documents(Term("id", BytesRef::from_string("1"))).has_value());
        ASSERT_TRUE(writer.value()->commit().has_value());
        ASSERT_TRUE(writer.value()->close().has_value());
    }

    infos = current_infos(directory);
    ASSERT_EQ(infos.segments.size(), 1U);
    ASSERT_EQ(infos.segments[0].doc_count(), 1);

    auto new_reader = pool.get(infos.segments[0], infos.index_created_version_major, io_context_default());
    ASSERT_TRUE(new_reader.has_value()) << (new_reader ? "" : new_reader.error().to_string());
    EXPECT_EQ(new_reader.value()->num_docs().value(), 1) << "the reader reflects the applied delete";

    // Pruning drops the reader of the segment that was replaced.
    pool.retain_only(infos);
    EXPECT_FALSE(pool.is_cached(old_name));
    EXPECT_TRUE(pool.is_cached(infos.segments[0].info.name));
    EXPECT_EQ(pool.num_cached(), 1U);
}

TEST(ReaderPoolTest, ForgetsReaders) {
    ByteBuffersDirectory directory;
    write_index(directory);
    const SegmentInfos infos = current_infos(directory);
    ASSERT_EQ(infos.segments.size(), 1U);

    ReaderPool pool(directory);
    SegmentCommitInfo segment = infos.segments[0];
    ASSERT_TRUE(pool.get(segment, infos.index_created_version_major, io_context_default()).has_value());
    EXPECT_EQ(pool.num_cached(), 1U);

    pool.drop(segment);
    EXPECT_EQ(pool.num_cached(), 0U);
    EXPECT_FALSE(pool.is_cached(segment.info.name));

    ASSERT_TRUE(pool.get(segment, infos.index_created_version_major, io_context_default()).has_value());
    pool.drop_all();
    EXPECT_EQ(pool.num_cached(), 0U);
}

TEST(ReaderPoolTest, CloseIsIdempotentAndStopsServing) {
    ByteBuffersDirectory directory;
    write_index(directory);
    const SegmentInfos infos = current_infos(directory);
    ASSERT_EQ(infos.segments.size(), 1U);

    ReaderPool pool(directory);
    SegmentCommitInfo segment = infos.segments[0];
    auto reader = pool.get(segment, infos.index_created_version_major, io_context_default());
    ASSERT_TRUE(reader.has_value());

    ASSERT_TRUE(pool.close().has_value());
    EXPECT_TRUE(pool.is_closed());
    EXPECT_EQ(pool.num_cached(), 0U);
    ASSERT_TRUE(pool.close().has_value()) << "closing twice is fine";

    auto after_close = pool.get(segment, infos.index_created_version_major, io_context_default());
    ASSERT_FALSE(after_close.has_value());
    EXPECT_EQ(after_close.error().kind(), ErrorKind::kAlreadyClosed);
}

TEST(ReaderPoolTest, TheWriterReusesItsReaders) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    ASSERT_TRUE(writer.value()->add_document(make_doc("1")).has_value());
    auto first = writer.value()->get_reader();
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    ASSERT_EQ(first.value()->segment_readers().size(), 1U);
    const SegmentReader *first_segment = first.value()->segment_readers()[0].get();

    // Nothing changed, so the next near-real-time read hands out the same reader.
    auto second = writer.value()->get_reader();
    ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
    ASSERT_EQ(second.value()->segment_readers().size(), 1U);
    EXPECT_EQ(second.value()->segment_readers()[0].get(), first_segment);

    // A new segment appears, so a third read opens one more reader.
    ASSERT_TRUE(writer.value()->add_document(make_doc("2")).has_value());
    auto third = writer.value()->get_reader();
    ASSERT_TRUE(third.has_value()) << (third ? "" : third.error().to_string());
    EXPECT_EQ(third.value()->segment_readers().size(), 2U);
    EXPECT_EQ(third.value()->segment_readers()[0].get(), first_segment) << "the old segment is still cached";
    EXPECT_EQ(third.value()->num_docs().value(), 2);

    ASSERT_TRUE(writer.value()->close().has_value());
}
