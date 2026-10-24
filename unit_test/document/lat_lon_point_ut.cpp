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
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lat_lon_point;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kPointField = "loc";

/// Searches with `query` and returns the matching document ids, sorted.
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

/// Indexes one point per entry of `points` and opens the reader.
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
        auto field = LatLonPoint::create(std::string(kPointField), lat, lon);
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

TEST(LatLonPointTest, PacksBothCoordinates) {
    auto packed = LatLonPoint::pack(12.5, -30.25);
    ASSERT_TRUE(packed.has_value()) << (packed ? "" : packed.error().to_string());
    ASSERT_EQ(packed.value().size(), 8U);

    const int32_t lat = NumericUtils::sortable_bytes_to_int(packed.value(), 0);
    const int32_t lon = NumericUtils::sortable_bytes_to_int(packed.value(), LatLonPoint::BYTES);
    EXPECT_NEAR(GeoEncodingUtils::decode_latitude(lat), 12.5, 1e-6);
    EXPECT_NEAR(GeoEncodingUtils::decode_longitude(lon), -30.25, 1e-6);

    // The rounding variant is never smaller than the value.
    auto ceil = LatLonPoint::pack_ceil(12.5, -30.25);
    ASSERT_TRUE(ceil.has_value());
    EXPECT_GE(GeoEncodingUtils::decode_latitude(NumericUtils::sortable_bytes_to_int(ceil.value(), 0)), 12.5);
}

TEST(LatLonPointTest, RejectsInvalidCoordinates) {
    auto bad_lat = LatLonPoint::pack(90.5, 0.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = LatLonPoint::pack(0.0, -180.5);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_field = LatLonPoint::create(std::string(kPointField), 0.0, 200.0);
    ASSERT_FALSE(bad_field.has_value());
    EXPECT_EQ(bad_field.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonPointTest, CreatesATwoDimensionalPointField) {
    auto field = LatLonPoint::create(std::string(kPointField), 10.0, 20.0);
    ASSERT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 2U);
    EXPECT_EQ(field.value().field_type().point_num_bytes(), 4U);
    EXPECT_EQ(field.value().name(), kPointField);
}

TEST(LatLonPointTest, FindsTheExactPoint) {
    auto reader = index_points({{0.0, 0.0}, {10.0, 10.0}, {-20.0, 30.0}});
    ASSERT_NE(reader, nullptr);

    auto query = LatLonPoint::new_exact_query(std::string(kPointField), 10.0, 10.0);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(*query.value(), reader), (std::vector<int32_t>{1}));

    // A point that is not in the index matches nothing.
    auto missing = LatLonPoint::new_exact_query(std::string(kPointField), 1.0, 1.0);
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(search_docs(*missing.value(), reader).empty());
}

TEST(LatLonPointTest, FindsThePointsInsideACircle) {
    auto reader = index_points({{0.0, 0.0}, {10.0, 10.0}, {-20.0, 30.0}});
    ASSERT_NE(reader, nullptr);

    // 100 km around the origin: only the first point is that close.
    auto circle = Circle::create(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(circle.has_value());
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<Circle>(circle.value()));

    auto query = LatLonPoint::new_geometry_query(std::string(kPointField), QueryRelation::Intersects,
                                                std::move(geometries));
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(*query.value(), reader), (std::vector<int32_t>{0}));

    // A circle that covers everything matches every document.
    auto big = Circle::create(0.0, 0.0, 10'000'000.0);
    ASSERT_TRUE(big.has_value());
    std::vector<std::shared_ptr<const Geometry>> wide_geometries;
    wide_geometries.push_back(std::make_shared<Circle>(big.value()));
    auto wide = LatLonPoint::new_geometry_query(std::string(kPointField), QueryRelation::Intersects,
                                               std::move(wide_geometries));
    ASSERT_TRUE(wide.has_value());
    EXPECT_EQ(search_docs(*wide.value(), reader), (std::vector<int32_t>{0, 1, 2}));
}

TEST(LatLonPointTest, FindsThePointsInsideAPolygon) {
    auto reader = index_points({{0.0, 0.0}, {10.0, 10.0}, {-20.0, 30.0}});
    ASSERT_NE(reader, nullptr);

    // A box around the second point, as a polygon.
    auto polygon = Polygon::create({5.0, 5.0, 15.0, 15.0, 5.0}, {5.0, 15.0, 15.0, 5.0, 5.0});
    ASSERT_TRUE(polygon.has_value()) << (polygon ? "" : polygon.error().to_string());
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<Polygon>(polygon.value()));

    auto query = LatLonPoint::new_geometry_query(std::string(kPointField), QueryRelation::Intersects,
                                                std::move(geometries));
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(*query.value(), reader), (std::vector<int32_t>{1}));
}

TEST(LatLonPointTest, RejectsAnEmptyGeometryList) {
    auto query = LatLonPoint::new_geometry_query(std::string(kPointField), QueryRelation::Intersects, {});
    ASSERT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonPointTest, DescribesTheGeometryQuery) {
    auto circle = Circle::create(0.0, 0.0, 1000.0);
    ASSERT_TRUE(circle.has_value());
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<Circle>(circle.value()));

    auto query = LatLonPoint::new_geometry_query(std::string(kPointField), QueryRelation::Intersects,
                                                std::move(geometries));
    ASSERT_TRUE(query.has_value());
    auto text = query.value()->to_string("");
    ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().to_string());
    EXPECT_EQ(text.value(), "SpatialQuery: field=loc:[Intersects, geometries=1]");
}

TEST(LatLonPointTest, FindsThePointsWithinADistance) {
    auto reader = index_points({{0.0, 0.0}, {10.0, 10.0}, {-20.0, 30.0}});
    ASSERT_NE(reader, nullptr);

    // 100 km around the origin: only the first point.
    auto near = LatLonPoint::new_distance_query(std::string(kPointField), 0.0, 0.0, 100'000.0);
    ASSERT_TRUE(near.has_value()) << (near ? "" : near.error().to_string());
    EXPECT_EQ(search_docs(*near.value(), reader), (std::vector<int32_t>{0}));

    // 3000 km also reaches the second point (about 1570 km away), not the third
    // (about 4240 km away).
    auto wider = LatLonPoint::new_distance_query(std::string(kPointField), 0.0, 0.0, 3'000'000.0);
    ASSERT_TRUE(wider.has_value());
    EXPECT_EQ(search_docs(*wider.value(), reader), (std::vector<int32_t>{0, 1}));

    // A radius that covers the whole world matches everything.
    auto worldwide = LatLonPoint::new_distance_query(std::string(kPointField), 0.0, 0.0, 30'000'000.0);
    ASSERT_TRUE(worldwide.has_value());
    EXPECT_EQ(search_docs(*worldwide.value(), reader), (std::vector<int32_t>{0, 1, 2}));

    // A zero radius around a point that is not indexed matches nothing.
    auto empty = LatLonPoint::new_distance_query(std::string(kPointField), 1.0, 1.0, 0.0);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(search_docs(*empty.value(), reader).empty());
}

TEST(LatLonPointTest, FindsThePointsOfACircleAcrossTheDateline) {
    auto reader = index_points({{0.0, 179.9}, {0.0, -179.9}, {0.0, 0.0}});
    ASSERT_NE(reader, nullptr);

    // The circle reaches over the dateline, so both sides match.
    auto query = LatLonPoint::new_distance_query(std::string(kPointField), 0.0, 179.95, 50'000.0);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(*query.value(), reader), (std::vector<int32_t>{0, 1}));
}

TEST(LatLonPointTest, ValidatesTheDistanceQuery) {
    auto negative = LatLonPoint::new_distance_query(std::string(kPointField), 0.0, 0.0, -1.0);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);

    auto infinite = LatLonPoint::new_distance_query(std::string(kPointField), 0.0, 0.0,
                                                    std::numeric_limits<double>::infinity());
    ASSERT_FALSE(infinite.has_value());
    EXPECT_EQ(infinite.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lat = LatLonPoint::new_distance_query(std::string(kPointField), 91.0, 0.0, 1000.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = LatLonPoint::new_distance_query(std::string(kPointField), 0.0, -181.0, 1000.0);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);

    auto no_field = LatLonPoint::new_distance_query("", 0.0, 0.0, 1000.0);
    ASSERT_FALSE(no_field.has_value());
    EXPECT_EQ(no_field.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonPointTest, DescribesTheDistanceQuery) {
    auto query = LatLonPointDistanceQuery::create(std::string(kPointField), 12.34, -56.78, 1000.0);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());

    auto text = query.value()->to_string("");
    ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().to_string());
    EXPECT_EQ(text.value(), "loc:12.3,-56.8 +/- 1000.0 meters");

    // A query over the same field prints just the circle.
    auto without_field = query.value()->to_string(std::string(kPointField));
    ASSERT_TRUE(without_field.has_value());
    EXPECT_EQ(without_field.value(), "12.3,-56.8 +/- 1000.0 meters");

    auto same = LatLonPointDistanceQuery::create(std::string(kPointField), 12.34, -56.78, 1000.0);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(query.value()->equals(*same.value()));
    EXPECT_EQ(query.value()->hash(), same.value()->hash());

    auto other = LatLonPointDistanceQuery::create(std::string(kPointField), 12.34, -56.78, 2000.0);
    ASSERT_TRUE(other.has_value());
    EXPECT_FALSE(query.value()->equals(*other.value()));
}
