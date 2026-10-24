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
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lat_lon_point_sort_field;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kPointField = "loc";

/// Indexes the given locations plus one document without the field, and opens the
/// reader; the returned ids are in insertion order.
std::shared_ptr<DirectoryReader> index_locations(const std::vector<std::pair<double, double>> &locations,
                                                 bool with_missing) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }

    for (const auto &[lat, lon] : locations) {
        Document doc;
        auto field = LatLonDocValuesField::create(std::string(kPointField), lat, lon);
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

/// Searches with the distance sort around `(latitude, longitude)` and returns the
/// matching document ids in hit order.
std::vector<int32_t> search_by_distance(std::shared_ptr<DirectoryReader> &reader, double latitude,
                                        double longitude) {
    auto sort_field = LatLonPointSortField::create(std::string(kPointField), latitude, longitude);
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

TEST(LatLonPointSortFieldTest, ValidatesTheOrigin) {
    EXPECT_TRUE(LatLonPointSortField::create(std::string(kPointField), 0.0, 0.0).has_value());

    auto bad_lat = LatLonPointSortField::create(std::string(kPointField), 91.0, 0.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = LatLonPointSortField::create(std::string(kPointField), 0.0, 181.0);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);

    auto no_field = LatLonPointSortField::create("", 0.0, 0.0);
    ASSERT_FALSE(no_field.has_value());
    EXPECT_EQ(no_field.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonPointSortFieldTest, DescribesItself) {
    auto sort_field = LatLonPointSortField::create(std::string(kPointField), 12.5, -30.25);
    ASSERT_TRUE(sort_field.has_value());
    EXPECT_EQ(sort_field.value().to_string(), "<distance:\"loc\" latitude=12.5 longitude=-30.25>");

    // The sort field it produces keeps that text.
    auto base = sort_field.value().to_sort_field().to_string();
    ASSERT_TRUE(base.has_value()) << (base ? "" : base.error().to_string());
    EXPECT_EQ(base.value(), "<distance:\"loc\" latitude=12.5 longitude=-30.25>");
}

TEST(LatLonPointSortFieldTest, SortsTheHitsByDistance) {
    // Insertion order is `b` (far), `a` (at the origin), `c` (close).
    auto reader = index_locations({{10.0, 10.0}, {0.0, 0.0}, {0.5, 0.5}}, false);
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(search_by_distance(reader, 0.0, 0.0), (std::vector<int32_t>{1, 2, 0}));

    // Moving the origin to the far location reverses the outer two.
    EXPECT_EQ(search_by_distance(reader, 10.0, 10.0), (std::vector<int32_t>{0, 2, 1}));
}

TEST(LatLonPointSortFieldTest, SortsTheMissingValuesLast) {
    // The fourth document has no location at all.
    auto reader = index_locations({{10.0, 10.0}, {0.0, 0.0}, {0.5, 0.5}}, true);
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(search_by_distance(reader, 0.0, 0.0), (std::vector<int32_t>{1, 2, 0, 3}));
}
