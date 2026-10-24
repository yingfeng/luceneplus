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

module lucene_core:ut.filter_directory_reader;

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

/// Two segments: the writer is asked for two commits so the second segment
/// survives.
std::shared_ptr<DirectoryReader> build_index(ByteBuffersDirectory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    EXPECT_TRUE(writer.value()->add_document(make_doc("1", "hello world")).has_value());
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->add_document(make_doc("2", "hello")).has_value());
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

/// Passes every leaf through.
class IdentityWrapper final : public SubReaderWrapper {
public:
    [[nodiscard]] Result<std::shared_ptr<LeafReader>> wrap(std::shared_ptr<LeafReader> reader) const override {
        return reader;
    }
};

/// Keeps only the first leaf.
class FirstLeafWrapper final : public SubReaderWrapper {
public:
    [[nodiscard]] Result<std::vector<std::shared_ptr<LeafReader>>>
    wrap_readers(std::vector<std::shared_ptr<LeafReader>> readers) const override {
        if (readers.empty()) {
            return readers;
        }
        readers.resize(1);
        return readers;
    }

    [[nodiscard]] Result<std::shared_ptr<LeafReader>> wrap(std::shared_ptr<LeafReader> reader) const override {
        return reader;
    }
};

/// Turns a leaf into null, which the wrapper must reject.
class NullWrapper final : public SubReaderWrapper {
public:
    [[nodiscard]] Result<std::shared_ptr<LeafReader>> wrap(std::shared_ptr<LeafReader>) const override {
        return std::shared_ptr<LeafReader>{};
    }
};

} // namespace

TEST(FilterDirectoryReaderTest, APassThroughWrapperKeepsEverything) {
    ByteBuffersDirectory directory;
    auto delegate = build_index(directory);
    ASSERT_NE(delegate, nullptr);
    ASSERT_FALSE(delegate->leaf_readers().empty());

    const IdentityWrapper wrapper;
    auto filtered = wrap_directory_reader(delegate, wrapper);
    ASSERT_TRUE(filtered.has_value()) << (filtered ? "" : filtered.error().to_string());

    EXPECT_EQ(filtered.value()->num_docs().value(), delegate->num_docs().value());
    EXPECT_EQ(filtered.value()->max_doc().value(), delegate->max_doc().value());
    EXPECT_EQ(filtered.value()->leaf_readers().size(), delegate->leaf_readers().size());
    EXPECT_TRUE(filtered.value()->segment_readers().empty())
            << "a wrapped reader reports no segment readers";
    EXPECT_EQ(filtered.value()->segment_infos().generation, delegate->segment_infos().generation);
    EXPECT_EQ(&filtered.value()->directory(), &directory);

    // Search over the wrapped reader finds the same documents.
    IndexSearcher searcher(filtered.value());
    const TermQuery query(Term("title", BytesRef::from_string("hello")));
    auto count = searcher.count(query);
    ASSERT_TRUE(count.has_value()) << (count ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 2);
}

TEST(FilterDirectoryReaderTest, AWrapperMayDropLeaves) {
    ByteBuffersDirectory directory;
    auto delegate = build_index(directory);
    ASSERT_NE(delegate, nullptr);
    ASSERT_GE(delegate->leaf_readers().size(), 2U) << "the test needs two segments";

    const FirstLeafWrapper wrapper;
    auto filtered = wrap_directory_reader(delegate, wrapper);
    ASSERT_TRUE(filtered.has_value()) << (filtered ? "" : filtered.error().to_string());

    EXPECT_EQ(filtered.value()->leaf_readers().size(), 1U);
    EXPECT_LT(filtered.value()->num_docs().value(), delegate->num_docs().value());
}

TEST(FilterDirectoryReaderTest, WrappingIsNestable) {
    ByteBuffersDirectory directory;
    auto delegate = build_index(directory);
    ASSERT_NE(delegate, nullptr);

    const IdentityWrapper identity;
    auto once = wrap_directory_reader(delegate, identity);
    ASSERT_TRUE(once.has_value()) << (once ? "" : once.error().to_string());

    auto twice = wrap_directory_reader(once.value(), identity);
    ASSERT_TRUE(twice.has_value()) << (twice ? "" : twice.error().to_string());
    EXPECT_EQ(twice.value()->num_docs().value(), delegate->num_docs().value());
    EXPECT_EQ(twice.value()->leaf_readers().size(), delegate->leaf_readers().size());
}

TEST(FilterDirectoryReaderTest, RejectsNullLeavesAndNullDelegates) {
    ByteBuffersDirectory directory;
    auto delegate = build_index(directory);
    ASSERT_NE(delegate, nullptr);

    const NullWrapper null_wrapper;
    auto rejected = wrap_directory_reader(delegate, null_wrapper);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);

    const IdentityWrapper identity;
    auto null_delegate = wrap_directory_reader(nullptr, identity);
    ASSERT_FALSE(null_delegate.has_value());
    EXPECT_EQ(null_delegate.error().kind(), ErrorKind::kIllegalArgument);
}
