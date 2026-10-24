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
#include <utility>

module lucene_core:ut.single_leaf_composite_reader;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

/// A one-segment index: the leaf plus the reader that owns it.
struct OneSegment {
    std::shared_ptr<DirectoryReader> owner;
    std::shared_ptr<LeafReader> leaf;
};

/// Builds a one-segment index and aliases its single leaf.
OneSegment one_segment_leaf(ByteBuffersDirectory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return {};
    }
    for (const std::pair<const char *, const char *> &doc : {std::pair{"1", "hello world"}, std::pair{"2", "hello"}}) {
        Document document;
        document.add(std::make_unique<Field>("id", FieldValue{std::string(doc.first)}, text_type()));
        document.add(std::make_unique<Field>("title", FieldValue{std::string(doc.second)}, text_type()));
        EXPECT_TRUE(writer.value()->add_document(std::move(document)).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    if (!reader.has_value()) {
        return {};
    }
    auto leaves = reader.value()->leaves();
    EXPECT_TRUE(leaves.has_value());
    if (!leaves.has_value() || leaves.value().empty()) {
        return {};
    }
    // The leaf context only holds a raw pointer; the alias keeps the owner alive.
    std::shared_ptr<LeafReader> leaf(reader.value(), leaves.value()[0].reader());
    return OneSegment{reader.value(), std::move(leaf)};
}

} // namespace

TEST(SingleLeafCompositeReaderTest, DelegatesToTheLeaf) {
    ByteBuffersDirectory directory;
    auto segment = one_segment_leaf(directory);
    ASSERT_NE(segment.leaf, nullptr);

    auto reader = SingleLeafCompositeReader::create(segment.leaf);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    EXPECT_EQ(reader.value()->max_doc().value(), 2);
    EXPECT_EQ(reader.value()->num_docs().value(), 2);
    EXPECT_EQ(reader.value()->leaf().get(), segment.leaf.get());
    EXPECT_EQ(&reader.value()->identity(), &segment.leaf->identity()) << "the identity comes from the leaf";
    EXPECT_NE(reader.value()->to_string().find("SingleLeafCompositeReader"), std::string::npos);

    auto stored = reader.value()->stored_fields();
    ASSERT_TRUE(stored.has_value()) << (stored ? "" : stored.error().to_string());
    auto document = stored.value()->document(0);
    ASSERT_TRUE(document.has_value()) << (document ? "" : document.error().to_string());

    auto vectors = reader.value()->term_vectors();
    EXPECT_TRUE(vectors.has_value());

    // Term statistics come from the leaf's dictionary.
    const Term hello("title", BytesRef::from_string("hello"));
    EXPECT_EQ(reader.value()->doc_freq(hello).value(), 2);
    EXPECT_EQ(reader.value()->total_term_freq(hello).value(), 2);
    const Term missing("title", BytesRef::from_string("zzz"));
    EXPECT_EQ(reader.value()->doc_freq(missing).value(), 0);
    EXPECT_EQ(reader.value()->total_term_freq(missing).value(), 0);
    EXPECT_GE(reader.value()->sum_doc_freq("title").value(), 3);
    EXPECT_EQ(reader.value()->doc_count("title").value(), 2);
    EXPECT_GE(reader.value()->sum_total_term_freq("title").value(), 3);
    EXPECT_EQ(reader.value()->doc_count("absent").value(), 0);
}

TEST(SingleLeafCompositeReaderTest, PublishesTheLeafWithDocBaseZero) {
    ByteBuffersDirectory directory;
    auto segment = one_segment_leaf(directory);
    ASSERT_NE(segment.leaf, nullptr);

    auto reader = SingleLeafCompositeReader::create(segment.leaf);
    ASSERT_TRUE(reader.has_value());

    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value()) << (leaves ? "" : leaves.error().to_string());
    ASSERT_EQ(leaves.value().size(), 1U);
    EXPECT_EQ(leaves.value()[0].reader(), segment.leaf.get());
    EXPECT_EQ(leaves.value()[0].doc_base(), 0);
    EXPECT_EQ(leaves.value()[0].ord(), 0);

    // The same span comes back on every call.
    auto again = reader.value()->leaves();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again.value().data(), leaves.value().data());
}

TEST(SingleLeafCompositeReaderTest, CanDriveAnIndexSearcher) {
    ByteBuffersDirectory directory;
    auto segment = one_segment_leaf(directory);
    ASSERT_NE(segment.leaf, nullptr);

    auto reader = SingleLeafCompositeReader::create(segment.leaf);
    ASSERT_TRUE(reader.has_value());

    IndexSearcher searcher(reader.value());
    const TermQuery query(Term("title", BytesRef::from_string("hello")));
    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    EXPECT_EQ(top.value().score_docs.size(), 2U);
    EXPECT_EQ(searcher.count(query).value(), 2);
}

TEST(SingleLeafCompositeReaderTest, RejectsANullLeaf) {
    auto reader = SingleLeafCompositeReader::create(nullptr);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kIllegalArgument);
}
