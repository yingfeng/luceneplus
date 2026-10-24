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

module lucene_core:ut.xy_point_field;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kPointField = "xy";

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
std::shared_ptr<DirectoryReader> index_points(const std::vector<std::pair<float, float>> &points) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const auto &[x, y] : points) {
        Document doc;
        auto field = XYPointField::create(std::string(kPointField), x, y);
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

TEST(XYPointFieldTest, PacksBothCoordinatesInSortableOrder) {
    auto packed = XYPointField::pack(1.5F, -2.5F);
    ASSERT_TRUE(packed.has_value()) << (packed ? "" : packed.error().to_string());
    ASSERT_EQ(packed.value().size(), 8U);
    EXPECT_EQ(XYPointField::decode_x(packed.value()), 1.5F);
    EXPECT_EQ(XYPointField::decode_y(packed.value()), -2.5F);

    // The encoding is monotonic, which is what lets the point index prune on it.
    auto negative = XYPointField::pack(-1.0F, 0.0F);
    auto zero = XYPointField::pack(0.0F, 0.0F);
    auto positive = XYPointField::pack(1.0F, 0.0F);
    ASSERT_TRUE(negative.has_value());
    ASSERT_TRUE(zero.has_value());
    ASSERT_TRUE(positive.has_value());
    EXPECT_LT(NumericUtils::sortable_bytes_to_int(negative.value(), 0),
              NumericUtils::sortable_bytes_to_int(zero.value(), 0));
    EXPECT_LT(NumericUtils::sortable_bytes_to_int(zero.value(), 0),
              NumericUtils::sortable_bytes_to_int(positive.value(), 0));
}

TEST(XYPointFieldTest, RejectsInvalidCoordinates) {
    auto nan = XYPointField::pack(std::numeric_limits<float>::quiet_NaN(), 0.0F);
    ASSERT_FALSE(nan.has_value());
    EXPECT_EQ(nan.error().kind(), ErrorKind::kIllegalArgument);

    auto infinite = XYPointField::create(std::string(kPointField), 0.0F, std::numeric_limits<float>::infinity());
    ASSERT_FALSE(infinite.has_value());
    EXPECT_EQ(infinite.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(XYPointFieldTest, CreatesATwoDimensionalPointField) {
    auto field = XYPointField::create(std::string(kPointField), 3.0F, 4.0F);
    ASSERT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    EXPECT_EQ(field.value().field_type().point_dimension_count(), 2U);
    EXPECT_EQ(field.value().field_type().point_num_bytes(), 4U);
    EXPECT_EQ(field.value().name(), kPointField);
}

TEST(XYPointFieldTest, FindsThePointsInsideABox) {
    auto reader = index_points({{0.0F, 0.0F}, {5.0F, 5.0F}, {10.0F, 10.0F}});
    ASSERT_NE(reader, nullptr);

    auto query = XYPointField::new_box_query(std::string(kPointField), 4.0F, 6.0F, 4.0F, 6.0F);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(*query.value(), reader), (std::vector<int32_t>{1}));

    // A box that covers everything matches every document.
    auto wide = XYPointField::new_box_query(std::string(kPointField), -100.0F, 100.0F, -100.0F, 100.0F);
    ASSERT_TRUE(wide.has_value());
    EXPECT_EQ(search_docs(*wide.value(), reader), (std::vector<int32_t>{0, 1, 2}));

    // A box that covers nothing matches nothing.
    auto empty = XYPointField::new_box_query(std::string(kPointField), 20.0F, 30.0F, 20.0F, 30.0F);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(search_docs(*empty.value(), reader).empty());
}

TEST(XYPointFieldTest, FindsThePointsWithinACartesianDistance) {
    auto reader = index_points({{0.0F, 0.0F}, {5.0F, 5.0F}, {10.0F, 10.0F}});
    ASSERT_NE(reader, nullptr);

    // A radius of 2 around the origin only reaches the first point.
    auto near = XYPointField::new_distance_query(std::string(kPointField), 0.0F, 0.0F, 2.0F);
    ASSERT_TRUE(near.has_value()) << (near ? "" : near.error().to_string());
    EXPECT_EQ(search_docs(*near.value(), reader), (std::vector<int32_t>{0}));

    // A radius of 8 reaches the second point as well (about 7.07 away).
    auto wider = XYPointField::new_distance_query(std::string(kPointField), 0.0F, 0.0F, 8.0F);
    ASSERT_TRUE(wider.has_value());
    EXPECT_EQ(search_docs(*wider.value(), reader), (std::vector<int32_t>{0, 1}));
}

TEST(XYPointFieldTest, FindsThePointsInsideAPolygon) {
    auto reader = index_points({{0.0F, 0.0F}, {5.0F, 5.0F}, {10.0F, 10.0F}});
    ASSERT_NE(reader, nullptr);

    auto polygon = XYPolygon::create({2.0F, 8.0F, 8.0F, 2.0F, 2.0F}, {2.0F, 2.0F, 8.0F, 8.0F, 2.0F});
    ASSERT_TRUE(polygon.has_value()) << (polygon ? "" : polygon.error().to_string());
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<XYPolygon>(std::move(polygon.value())));

    auto query = XYPointField::new_geometry_query(std::string(kPointField), std::move(geometries));
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    EXPECT_EQ(search_docs(*query.value(), reader), (std::vector<int32_t>{1}));
}

TEST(XYPointFieldTest, RejectsInvalidQueries) {
    auto inverted = XYPointField::new_box_query(std::string(kPointField), 5.0F, 1.0F, 0.0F, 1.0F);
    ASSERT_FALSE(inverted.has_value());
    EXPECT_EQ(inverted.error().kind(), ErrorKind::kIllegalArgument);

    auto negative_radius = XYPointField::new_distance_query(std::string(kPointField), 0.0F, 0.0F, -1.0F);
    ASSERT_FALSE(negative_radius.has_value());
    EXPECT_EQ(negative_radius.error().kind(), ErrorKind::kIllegalArgument);

    auto no_geometries = XYPointField::new_geometry_query(std::string(kPointField), {});
    ASSERT_FALSE(no_geometries.has_value());
    EXPECT_EQ(no_geometries.error().kind(), ErrorKind::kIllegalArgument);

    auto no_field = XYPointField::new_geometry_query("", {});
    ASSERT_FALSE(no_field.has_value());
    EXPECT_EQ(no_field.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(XYPointFieldTest, DescribesTheGeometryQuery) {
    auto box = XYRectangle::create(0.0F, 1.0F, 0.0F, 1.0F);
    ASSERT_TRUE(box.has_value());
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<XYRectangle>(std::move(box.value())));

    auto query = XYPointInGeometryQuery::create(std::string(kPointField), std::move(geometries));
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    ASSERT_NE(query.value(), nullptr);

    auto text = query.value()->to_string("");
    ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().to_string());
    EXPECT_EQ(text.value(), "XYPointInGeometryQuery: field=xy:[geometries=1]");

    auto without_field = query.value()->to_string(std::string(kPointField));
    ASSERT_TRUE(without_field.has_value());
    EXPECT_EQ(without_field.value(), "XYPointInGeometryQuery:[geometries=1]");
}

TEST(XYEncodingUtilsTest, QuantizesCartesianValues) {
    EXPECT_EQ(XYEncodingUtils::check_val(1.0F).value(), 1.0F);
    EXPECT_FALSE(XYEncodingUtils::check_val(std::numeric_limits<float>::quiet_NaN()).has_value());

    auto encoded = XYEncodingUtils::encode(-3.5F);
    ASSERT_TRUE(encoded.has_value()) << (encoded ? "" : encoded.error().to_string());
    EXPECT_EQ(XYEncodingUtils::decode(encoded.value()), -3.5F);

    // Bytes written by the point field decode back to the coordinate.
    auto packed = XYPointField::pack(2.5F, -4.5F);
    ASSERT_TRUE(packed.has_value());
    EXPECT_EQ(XYEncodingUtils::decode_bytes(packed.value(), 0), 2.5F);
    EXPECT_EQ(XYEncodingUtils::decode_bytes(packed.value(), 4), -4.5F);

    const std::vector<float> values{1.0F, -2.0F};
    EXPECT_EQ(XYEncodingUtils::float_array_to_double_array(values), (std::vector<double>{1.0, -2.0}));
}
