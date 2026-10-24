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

module lucene_core:ut.directory_reader_sorter;

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

/// Two segments: the first holds one document, the second two.
void write_index(ByteBuffersDirectory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    ASSERT_TRUE(writer.value()->add_document(make_doc("a")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("b")).has_value());
    ASSERT_TRUE(writer.value()->add_document(make_doc("c")).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

} // namespace

TEST(DirectoryReaderSorterTest, ReordersTheLeaves) {
    ByteBuffersDirectory directory;
    write_index(directory);

    // Biggest segment first.
    DirectoryReader::LeafSorter largest_first = [](const LeafReader &left, const LeafReader &right) -> Result<int32_t> {
        auto left_max = left.max_doc();
        if (!left_max.has_value()) {
            return std::unexpected(std::move(left_max.error()));
        }
        auto right_max = right.max_doc();
        if (!right_max.has_value()) {
            return std::unexpected(std::move(right_max.error()));
        }
        if (left_max.value() == right_max.value()) {
            return 0;
        }
        return left_max.value() > right_max.value() ? -1 : 1;
    };

    auto sorted = DirectoryReader::open_with_sorter(directory, largest_first);
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());

    auto leaves = sorted.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves.value().size(), 2U);

    auto first_max = leaves.value()[0].reader()->max_doc();
    auto second_max = leaves.value()[1].reader()->max_doc();
    ASSERT_TRUE(first_max.has_value());
    ASSERT_TRUE(second_max.has_value());
    EXPECT_GE(first_max.value(), second_max.value()) << "the sorter ordered the leaves";
    EXPECT_EQ(leaves.value()[0].doc_base(), 0);
    EXPECT_EQ(leaves.value()[1].doc_base(), first_max.value()) << "doc bases follow the new order";

    // The documents themselves are all still there.
    IndexSearcher searcher(sorted.value());
    EXPECT_EQ(searcher.count(MatchAllDocsQuery()).value(), 3);
    EXPECT_EQ(sorted.value()->num_docs().value(), 3);
    EXPECT_GE(sorted.value()->max_doc().value(), 3);
}

TEST(DirectoryReaderSorterTest, TheUnorderedReaderKeepsTheCommitOrder) {
    ByteBuffersDirectory directory;
    write_index(directory);

    auto plain = DirectoryReader::open(directory);
    ASSERT_TRUE(plain.has_value()) << (plain ? "" : plain.error().to_string());
    auto leaves = plain.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves.value().size(), 2U);
    EXPECT_EQ(leaves.value()[0].doc_base(), 0)
            << "without a sorter the leaves keep the order the commit recorded";
}

TEST(DirectoryReaderSorterTest, ASorterThatFailsAbortsTheOpen) {
    ByteBuffersDirectory directory;
    write_index(directory);

    DirectoryReader::LeafSorter failing = [](const LeafReader &, const LeafReader &) -> Result<int32_t> {
        return err(LuceneError::illegal_state("the sorter gave up"));
    };
    auto failed = DirectoryReader::open_with_sorter(directory, failing);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().kind(), ErrorKind::kIllegalState);
}

TEST(DirectoryReaderSorterTest, AnEmptySorterIsRejected) {
    ByteBuffersDirectory directory;
    write_index(directory);

    auto rejected = DirectoryReader::open_with_sorter(directory, DirectoryReader::LeafSorter{});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);
}
