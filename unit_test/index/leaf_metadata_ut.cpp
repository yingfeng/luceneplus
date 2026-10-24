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
#include <optional>
#include <string>
#include <string_view>

module lucene_core:ut.leaf_metadata;

import lucene_core;

using namespace lucene;

TEST(LeafMetadataTest, CarriesTheVersionFields) {
    auto metadata = LeafMetaData::create(9, Version(7, 0, 0), std::shared_ptr<const Sort>(), false);
    ASSERT_TRUE(metadata.has_value()) << (metadata ? "" : metadata.error().to_string());

    EXPECT_EQ(metadata.value().created_version_major(), 9);
    ASSERT_TRUE(metadata.value().min_version().has_value());
    EXPECT_EQ(metadata.value().min_version()->major, 7);
    EXPECT_EQ(metadata.value().sort(), nullptr) << "the port cannot index-sort yet";
    EXPECT_FALSE(metadata.value().has_blocks());
    EXPECT_NE(metadata.value().to_string().find("created_version_major=9"), std::string::npos);
}

TEST(LeafMetadataTest, RejectsAFutureVersion) {
    auto metadata = LeafMetaData::create(LUCENE_CURRENT_VERSION.major + 1, LUCENE_CURRENT_VERSION,
                                         std::shared_ptr<const Sort>(), false);
    ASSERT_FALSE(metadata.has_value());
    EXPECT_EQ(metadata.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(metadata.error().message().find("in the future"), std::string::npos);
}

TEST(LeafMetadataTest, RejectsAnAncientVersion) {
    auto metadata = LeafMetaData::create(5, std::nullopt, std::shared_ptr<const Sort>(), false);
    ASSERT_FALSE(metadata.has_value());
    EXPECT_EQ(metadata.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(metadata.error().message().find("must be >= 6"), std::string::npos);
}

TEST(LeafMetadataTest, ModernSegmentsMustRecordAMinVersion) {
    auto metadata = LeafMetaData::create(7, std::nullopt, std::shared_ptr<const Sort>(), false);
    ASSERT_FALSE(metadata.has_value());
    EXPECT_EQ(metadata.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(metadata.error().message().find("min_version must be set"), std::string::npos);

    // A 6.x segment may omit it.
    auto old = LeafMetaData::create(6, std::nullopt, std::shared_ptr<const Sort>(), true);
    ASSERT_TRUE(old.has_value()) << (old ? "" : old.error().to_string());
    EXPECT_TRUE(old.value().has_blocks());
}

TEST(LeafMetadataTest, TheUnknownDefaultIsReadable) {
    const LeafMetaData &unknown = LeafMetaData::unknown();
    EXPECT_GE(unknown.created_version_major(), 6);
    EXPECT_TRUE(unknown.min_version().has_value());
    EXPECT_EQ(unknown.sort(), nullptr);
    EXPECT_FALSE(unknown.has_blocks());
    EXPECT_NE(unknown.to_string().find("LeafMetaData"), std::string::npos);
}

TEST(LeafMetadataTest, ASegmentReaderReportsItsMetadata) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    FieldType type;
    ASSERT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    Document doc;
    doc.add(std::make_unique<Field>("title", FieldValue{std::string("hello")}, type));
    ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_FALSE(leaves.value().empty());

    const LeafMetaData &metadata = leaves.value()[0].reader()->leaf_meta_data();
    EXPECT_EQ(metadata.created_version_major(), LUCENE_CURRENT_VERSION.major);
    ASSERT_TRUE(metadata.min_version().has_value());
    EXPECT_EQ(metadata.min_version()->major, LUCENE_CURRENT_VERSION.major);
    EXPECT_EQ(metadata.sort(), nullptr);
    EXPECT_FALSE(metadata.has_blocks());
}

TEST(LeafMetadataTest, ASortedIndexReportsItsSort) {
    ByteBuffersDirectory directory;
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    auto sort_field = SortField::create("ts", SortFieldType::kLong);
    ASSERT_TRUE(sort_field.has_value()) << (sort_field ? "" : sort_field.error().to_string());
    std::vector<SortField> fields;
    fields.push_back(sort_field.value());
    auto sort = Sort::with_fields(std::move(fields));
    ASSERT_TRUE(sort.has_value()) << (sort ? "" : sort.error().to_string());
    config.set_index_sort(sort.value());

    auto writer = IndexWriter::open(directory, config);
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());

    FieldType type;
    ASSERT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    for (const int64_t value : {30, 10, 20}) {
        Document doc;
        doc.add(std::make_unique<Field>("title", FieldValue{std::string("v")}, type));
        auto numeric = NumericDocValuesField::create("ts", value);
        ASSERT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
        doc.add(std::make_unique<Field>(std::move(numeric.value())));
        ASSERT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    }
    ASSERT_TRUE(writer.value()->commit().has_value());
    ASSERT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_FALSE(leaves.value().empty());

    const LeafMetaData &metadata = leaves.value()[0].reader()->leaf_meta_data();
    ASSERT_NE(metadata.sort(), nullptr) << "the segment must remember the index sort";
    EXPECT_TRUE(metadata.sort()->equals(sort.value()));

    // The documents must be in ascending `ts` order, i.e. 10, 20, 30.
    auto values = leaves.value()[0].reader()->get_numeric_doc_values("ts");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    std::vector<int64_t> seen;
    for (auto doc = values.value()->next_doc(); doc.has_value() && doc.value() != NO_MORE_DOCS;
         doc = values.value()->next_doc()) {
        auto value = values.value()->long_value();
        ASSERT_TRUE(value.has_value());
        seen.push_back(value.value());
    }
    EXPECT_EQ(seen, (std::vector<int64_t>{10, 20, 30}));
}
