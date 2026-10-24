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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lat_lon_doc_values;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kPointField = "loc";

std::vector<int32_t> search_docs(const Query &query, std::shared_ptr<DirectoryReader> &reader) {
    IndexSearcher searcher(reader);
    auto top = searcher.search(query, 10);
    EXPECT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    std::vector<int32_t> docs;
    if (!top.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::ranges::sort(docs);
    return docs;
}

/// Indexes one doc-values location per entry and opens the reader.
std::shared_ptr<DirectoryReader> index_points(const std::vector<std::pair<double, double>> &points) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const auto &[lat, lon] : points) {
        Document doc;
        auto field = LatLonDocValuesField::create(std::string(kPointField), lat, lon);
        EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
        doc.add(std::make_unique<Field>(std::move(field.value())));
        auto added = writer.value()->add_document(doc);
        EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(LatLonDocValuesTest, PacksTheLocationIntoOneValue) {
    auto packed = LatLonDocValuesField::pack(12.5, -30.25);
    ASSERT_TRUE(packed.has_value()) << (packed ? "" : packed.error().to_string());
    EXPECT_NEAR(LatLonDocValuesField::decode_latitude(packed.value()), 12.5, 1e-6);
    EXPECT_NEAR(LatLonDocValuesField::decode_longitude(packed.value()), -30.25, 1e-6);

    auto bad_lat = LatLonDocValuesField::pack(91.0, 0.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = LatLonDocValuesField::pack(0.0, -181.0);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonDocValuesTest, CreatesASortedNumericField) {
    auto field = LatLonDocValuesField::create(std::string(kPointField), 10.0, 20.0);
    ASSERT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    EXPECT_EQ(field.value().name(), kPointField);
    EXPECT_EQ(field.value().field_type().doc_values_type(), DocValuesType::SortedNumeric);
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 0U);
}

TEST(LatLonDocValuesTest, FindsTheLocationsInsideABox) {
    auto reader = index_points({{0.0, 0.0}, {10.0, 10.0}, {-20.0, 30.0}});
    ASSERT_NE(reader, nullptr);

    auto near = LatLonDocValuesField::new_slow_box_query(std::string(kPointField), -1.0, 1.0, -1.0, 1.0);
    ASSERT_TRUE(near.has_value()) << (near ? "" : near.error().to_string());
    EXPECT_EQ(search_docs(*near.value(), reader), (std::vector<int32_t>{0}));

    // The box reaches the second location but not the third.
    auto wider = LatLonDocValuesField::new_slow_box_query(std::string(kPointField), -1.0, 20.0, -1.0, 20.0);
    ASSERT_TRUE(wider.has_value());
    EXPECT_EQ(search_docs(*wider.value(), reader), (std::vector<int32_t>{0, 1}));

    // A box that covers the world matches every document.
    auto world = LatLonDocValuesField::new_slow_box_query(std::string(kPointField), -90.0, 90.0, -180.0, 180.0);
    ASSERT_TRUE(world.has_value());
    EXPECT_EQ(search_docs(*world.value(), reader), (std::vector<int32_t>{0, 1, 2}));

    // A box that covers nothing matches nothing.
    auto empty = LatLonDocValuesField::new_slow_box_query(std::string(kPointField), 40.0, 50.0, 40.0, 50.0);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(search_docs(*empty.value(), reader).empty());
}

TEST(LatLonDocValuesTest, FindsTheLocationsAcrossTheDateline) {
    auto reader = index_points({{0.0, 179.0}, {0.0, -179.0}, {0.0, 0.0}});
    ASSERT_NE(reader, nullptr);

    // `minLon > maxLon` means the box wraps around the dateline.
    auto query = LatLonDocValuesField::new_slow_box_query(std::string(kPointField), -1.0, 1.0, 170.0, -170.0);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(*query.value(), reader), (std::vector<int32_t>{0, 1}));
}

TEST(LatLonDocValuesTest, ValidatesTheBox) {
    auto bad_lat = LatLonDocValuesField::new_slow_box_query(std::string(kPointField), -91.0, 0.0, 0.0, 1.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = LatLonDocValuesField::new_slow_box_query(std::string(kPointField), 0.0, 1.0, 0.0, 181.0);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);

    // An inverted latitude range is not a dateline crossing, it is a mistake.
    auto inverted = LatLonDocValuesField::new_slow_box_query(std::string(kPointField), 10.0, 1.0, 0.0, 1.0);
    ASSERT_FALSE(inverted.has_value());
    EXPECT_EQ(inverted.error().kind(), ErrorKind::kIllegalArgument);

    auto no_field = LatLonDocValuesField::new_slow_box_query("", 0.0, 1.0, 0.0, 1.0);
    ASSERT_FALSE(no_field.has_value());
    EXPECT_EQ(no_field.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonDocValuesTest, DescribesTheBoxQuery) {
    auto query = LatLonDocValuesBoxQuery::create(std::string(kPointField), -10.0, 10.0, -20.0, 20.0);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());

    auto text = query.value()->to_string("");
    ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().to_string());
    // The bounds are printed decoded, so they read as the requested coordinates
    // (up to the quantization).
    EXPECT_EQ(text.value().find("loc:box(minLat=-9.99"), 0U) << text.value();
    EXPECT_NE(text.value().find("maxLon=19.99"), std::string::npos) << text.value();

    auto same = LatLonDocValuesBoxQuery::create(std::string(kPointField), -10.0, 10.0, -20.0, 20.0);
    ASSERT_TRUE(same.has_value());
    ASSERT_NE(same.value(), nullptr);
    EXPECT_TRUE(query.value()->equals(*same.value()));
    EXPECT_EQ(query.value()->hash(), same.value()->hash());

    auto other = LatLonDocValuesBoxQuery::create(std::string(kPointField), -10.0, 10.0, -20.0, 21.0);
    ASSERT_TRUE(other.has_value());
    EXPECT_FALSE(query.value()->equals(*other.value()));
}

TEST(LatLonDocValuesTest, ProvidesTheDistanceSortField) {
    auto sort_field = LatLonDocValuesField::new_distance_sort(std::string(kPointField), 12.5, -30.25);
    ASSERT_TRUE(sort_field.has_value()) << (sort_field ? "" : sort_field.error().to_string());
    EXPECT_EQ(sort_field.value().to_string(), "<distance:\"loc\" latitude=12.5 longitude=-30.25>");

    auto bad = LatLonDocValuesField::new_distance_sort(std::string(kPointField), 91.0, 0.0);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonDocValuesTest, FindsTheLocationsWithinACircle) {
    auto reader = index_points({{0.0, 0.0}, {10.0, 10.0}, {-20.0, 30.0}});
    ASSERT_NE(reader, nullptr);

    auto near = LatLonDocValuesField::new_slow_distance_query(std::string(kPointField), 0.0, 0.0, 100'000.0);
    ASSERT_TRUE(near.has_value()) << (near ? "" : near.error().to_string());
    EXPECT_EQ(search_docs(*near.value(), reader), (std::vector<int32_t>{0}));

    auto worldwide = LatLonDocValuesField::new_slow_distance_query(std::string(kPointField), 0.0, 0.0, 30'000'000.0);
    ASSERT_TRUE(worldwide.has_value());
    EXPECT_EQ(search_docs(*worldwide.value(), reader), (std::vector<int32_t>{0, 1, 2}));
}

TEST(LatLonDocValuesTest, RelatesTheLocationsToAShape) {
    auto reader = index_points({{0.0, 0.0}, {10.0, 10.0}, {-20.0, 30.0}});
    ASSERT_NE(reader, nullptr);

    // A box around the first location, as a geometry.
    auto box = Rectangle::create(-1.0, 1.0, -1.0, 1.0);
    ASSERT_TRUE(box.has_value()) << (box ? "" : box.error().to_string());
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<Rectangle>(box.value()));

    auto intersects = LatLonDocValuesField::new_slow_geometry_query(std::string(kPointField),
                                                                   QueryRelation::Intersects, geometries);
    ASSERT_TRUE(intersects.has_value()) << (intersects ? "" : intersects.error().to_string());
    EXPECT_EQ(search_docs(*intersects.value(), reader), (std::vector<int32_t>{0}));

    auto within = LatLonDocValuesField::new_slow_geometry_query(std::string(kPointField), QueryRelation::Within,
                                                               geometries);
    ASSERT_TRUE(within.has_value());
    EXPECT_EQ(search_docs(*within.value(), reader), (std::vector<int32_t>{0}));

    auto disjoint = LatLonDocValuesField::new_slow_geometry_query(std::string(kPointField), QueryRelation::Disjoint,
                                                                 geometries);
    ASSERT_TRUE(disjoint.has_value());
    EXPECT_EQ(search_docs(*disjoint.value(), reader), (std::vector<int32_t>{1, 2}));
}

TEST(LatLonDocValuesTest, ValidatesTheGeometryQuery) {
    auto no_geometries = LatLonDocValuesField::new_slow_geometry_query(std::string(kPointField),
                                                                      QueryRelation::Intersects, {});
    ASSERT_FALSE(no_geometries.has_value());
    EXPECT_EQ(no_geometries.error().kind(), ErrorKind::kIllegalArgument);

    auto no_field = LatLonDocValuesField::new_slow_distance_query("", 0.0, 0.0, 1000.0);
    ASSERT_FALSE(no_field.has_value());
    EXPECT_EQ(no_field.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_radius = LatLonDocValuesField::new_slow_distance_query(std::string(kPointField), 0.0, 0.0, -1.0);
    ASSERT_FALSE(bad_radius.has_value());
    EXPECT_EQ(bad_radius.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonDocValuesTest, DescribesTheGeometryQuery) {
    auto box = Rectangle::create(-1.0, 1.0, -1.0, 1.0);
    ASSERT_TRUE(box.has_value());
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<Rectangle>(box.value()));

    auto query = LatLonDocValuesQuery::create(std::string(kPointField), QueryRelation::Within, geometries);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());

    auto text = query.value()->to_string("");
    ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().to_string());
    EXPECT_EQ(text.value(), "loc:Within:geometries(1)");

    auto without_field = query.value()->to_string(std::string(kPointField));
    ASSERT_TRUE(without_field.has_value());
    EXPECT_EQ(without_field.value(), "Within:geometries(1)");
}
