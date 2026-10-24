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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.xy_point_sort_field;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kPointField = "xy";

/// Indexes the given locations plus, optionally, one document without the field.
std::shared_ptr<DirectoryReader> index_locations(const std::vector<std::pair<float, float>> &locations,
                                                 bool with_missing) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }

    for (const auto &[x, y] : locations) {
        Document doc;
        auto field = XYDocValuesField::create(std::string(kPointField), x, y);
        EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
        doc.add(std::make_unique<Field>(std::move(field.value())));
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }
    if (with_missing) {
        // An empty document: the distance sort has to deal with a missing value.
        Document doc;
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }

    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

/// Searches with the distance sort around `(x, y)` and returns the hit order.
std::vector<int32_t> search_by_distance(std::shared_ptr<DirectoryReader> &reader, float x, float y) {
    auto sort_field = XYPointSortField::create(std::string(kPointField), x, y);
    EXPECT_TRUE(sort_field.has_value()) << (sort_field ? "" : sort_field.error().to_string());
    if (!sort_field.has_value()) {
        return {};
    }
    auto sort = Sort::with_fields({sort_field.value().to_sort_field()});
    EXPECT_TRUE(sort.has_value()) << (sort ? "" : sort.error().to_string());
    if (!sort.has_value()) {
        return {};
    }

    IndexSearcher searcher(reader);
    MatchAllDocsQuery query;
    auto hits = searcher.search(query, sort.value(), 10);
    EXPECT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    std::vector<int32_t> docs;
    if (!hits.has_value()) {
        return docs;
    }
    for (const FieldDoc &hit : hits.value().score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

} // namespace

TEST(XYPointSortFieldTest, ValidatesTheOrigin) {
    EXPECT_TRUE(XYPointSortField::create(std::string(kPointField), 0.0F, 0.0F).has_value());

    auto nan = XYPointSortField::create(std::string(kPointField), std::numeric_limits<float>::quiet_NaN(), 0.0F);
    ASSERT_FALSE(nan.has_value());
    EXPECT_EQ(nan.error().kind(), ErrorKind::kIllegalArgument);

    auto no_field = XYPointSortField::create("", 0.0F, 0.0F);
    ASSERT_FALSE(no_field.has_value());
    EXPECT_EQ(no_field.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(XYPointSortFieldTest, DescribesItself) {
    auto sort_field = XYPointSortField::create(std::string(kPointField), 1.5F, -2.5F);
    ASSERT_TRUE(sort_field.has_value());
    EXPECT_EQ(sort_field.value().to_string(), "<distance:\"xy\" x=1.5 y=-2.5>");

    auto base = sort_field.value().to_sort_field().to_string();
    ASSERT_TRUE(base.has_value()) << (base ? "" : base.error().to_string());
    EXPECT_EQ(base.value(), "<distance:\"xy\" x=1.5 y=-2.5>");
}

TEST(XYPointSortFieldTest, SortsTheHitsByDistance) {
    // Insertion order is `b` (far), `a` (at the origin), `c` (close).
    auto reader = index_locations({{10.0F, 10.0F}, {0.0F, 0.0F}, {0.5F, 0.5F}}, false);
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(search_by_distance(reader, 0.0F, 0.0F), (std::vector<int32_t>{1, 2, 0}));

    // Moving the origin to the far location reverses the outer two.
    EXPECT_EQ(search_by_distance(reader, 10.0F, 10.0F), (std::vector<int32_t>{0, 2, 1}));
}

TEST(XYPointSortFieldTest, SortsTheMissingValuesLast) {
    // The fourth document has no location at all.
    auto reader = index_locations({{10.0F, 10.0F}, {0.0F, 0.0F}, {0.5F, 0.5F}}, true);
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(search_by_distance(reader, 0.0F, 0.0F), (std::vector<int32_t>{1, 2, 0, 3}));
}
