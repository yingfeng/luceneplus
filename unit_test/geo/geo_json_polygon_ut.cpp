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

#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.geo_json_polygon;

import lucene_core;

using namespace lucene;

namespace {

/// A GeoJSON polygon with a clockwise shell and no holes.
constexpr std::string_view kSquare =
        R"({"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]})";

/// The same square with a hole.
constexpr std::string_view kSquareWithHole = R"({"type":"Polygon","coordinates":[
    [[0,0],[10,0],[10,10],[0,10],[0,0]],
    [[4,4],[6,4],[6,6],[4,6],[4,4]]
]})";

} // namespace

TEST(GeoJsonPolygonParserTest, ParsesAPolygon) {
    auto polygons = polygons_from_geo_json(kSquare);
    ASSERT_TRUE(polygons.has_value()) << (polygons ? "" : polygons.error().to_string());
    ASSERT_EQ(polygons.value().size(), 1U);

    const Polygon &polygon = polygons.value()[0];
    EXPECT_EQ(polygon.num_points(), 5U);
    EXPECT_EQ(polygon.num_holes(), 0U);
    EXPECT_EQ(polygon.min_lat(), 0.0);
    EXPECT_EQ(polygon.max_lat(), 10.0);
    EXPECT_EQ(polygon.min_lon(), 0.0);
    EXPECT_EQ(polygon.max_lon(), 10.0);
    EXPECT_EQ(polygon.lat(2), 10.0);
    EXPECT_EQ(polygon.lon(2), 10.0);
}

TEST(GeoJsonPolygonParserTest, ParsesAHoleAndForgetsTheWhitespace) {
    auto polygons = polygons_from_geo_json(kSquareWithHole);
    ASSERT_TRUE(polygons.has_value()) << (polygons ? "" : polygons.error().to_string());
    ASSERT_EQ(polygons.value().size(), 1U);
    EXPECT_EQ(polygons.value()[0].num_holes(), 1U);
    EXPECT_EQ(polygons.value()[0].holes()[0].min_lat(), 4.0);
    EXPECT_EQ(polygons.value()[0].holes()[0].max_lon(), 6.0);
}

TEST(GeoJsonPolygonParserTest, ParsesAMultiPolygon) {
    const std::string_view multi = R"({"type":"MultiPolygon","coordinates":[
        [[[0,0],[1,0],[1,1],[0,1],[0,0]]],
        [[[10,10],[11,10],[11,11],[10,11],[10,10]]]
    ]})";
    auto polygons = polygons_from_geo_json(multi);
    ASSERT_TRUE(polygons.has_value()) << (polygons ? "" : polygons.error().to_string());
    ASSERT_EQ(polygons.value().size(), 2U);
    EXPECT_EQ(polygons.value()[0].min_lon(), 0.0);
    EXPECT_EQ(polygons.value()[1].min_lon(), 10.0);
}

TEST(GeoJsonPolygonParserTest, ParsesAFeatureAndAFeatureCollection) {
    const std::string_view feature = R"({"type":"Feature","geometry":{"type":"Polygon",
        "coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]},"properties":{}})";
    auto polygons = polygons_from_geo_json(feature);
    ASSERT_TRUE(polygons.has_value()) << (polygons ? "" : polygons.error().to_string());
    ASSERT_EQ(polygons.value().size(), 1U);

    const std::string_view collection = R"({"type":"FeatureCollection","features":[{"type":"Feature",
        "geometry":{"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]},
        "properties":null}]})";
    auto from_collection = polygons_from_geo_json(collection);
    ASSERT_TRUE(from_collection.has_value()) << (from_collection ? "" : from_collection.error().to_string());
    ASSERT_EQ(from_collection.value().size(), 1U);
    EXPECT_EQ(from_collection.value()[0].max_lat(), 10.0);
}

TEST(GeoJsonPolygonParserTest, AcceptsTheCrs84Declaration) {
    const std::string_view geo_json = R"({"type":"Polygon","crs":{"type":"name",
        "properties":{"name":"urn:ogc:def:crs:OGC:1.3:CRS84"}},
        "coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]})";
    auto polygons = polygons_from_geo_json(geo_json);
    ASSERT_TRUE(polygons.has_value()) << (polygons ? "" : polygons.error().to_string());
    EXPECT_EQ(polygons.value().size(), 1U);
}

TEST(GeoJsonPolygonParserTest, RejectsOtherCrs) {
    const std::string_view linked = R"({"type":"Polygon","crs":{"type":"link",
        "properties":{"href":"http://example.com/crs"}},
        "coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]})";
    auto bad_link = polygons_from_geo_json(linked);
    ASSERT_FALSE(bad_link.has_value());
    EXPECT_EQ(bad_link.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(bad_link.error().to_string().find("cannot handle linked crs"), std::string::npos);

    const std::string_view wrong_name = R"({"type":"Polygon","crs":{"type":"name",
        "properties":{"name":"urn:ogc:def:crs:EPSG::4326"}},
        "coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]})";
    auto bad_name = polygons_from_geo_json(wrong_name);
    ASSERT_FALSE(bad_name.has_value());
    EXPECT_EQ(bad_name.error().kind(), ErrorKind::kIllegalState);
}

TEST(GeoJsonPolygonParserTest, RejectsOtherGeometries) {
    const std::string_view line = R"({"type":"LineString","coordinates":[[0,0],[1,1]]})";
    auto bad_type = polygons_from_geo_json(line);
    ASSERT_FALSE(bad_type.has_value());
    EXPECT_EQ(bad_type.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(bad_type.error().to_string().find("can only handle type"), std::string::npos);

    // Two geometries are not supported: the reader only keeps one.
    const std::string_view two = R"({"type":"FeatureCollection","features":[
        {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[1,0],[1,1],[0,1],[0,0]]]}},
        {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[2,2],[3,2],[3,3],[2,3],[2,2]]]}}]})";
    auto too_many = polygons_from_geo_json(two);
    ASSERT_FALSE(too_many.has_value());
    EXPECT_EQ(too_many.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(too_many.error().to_string().find("only one Polygon or MultiPolygon"), std::string::npos);
}

TEST(GeoJsonPolygonParserTest, RejectsMalformedInput) {
    auto no_type = polygons_from_geo_json(R"({"coordinates":[[[0,0],[1,0],[1,1],[0,1],[0,0]]]})");
    ASSERT_FALSE(no_type.has_value());
    EXPECT_EQ(no_type.error().kind(), ErrorKind::kIllegalState);

    auto no_coordinates = polygons_from_geo_json(R"({"type":"Polygon"})");
    ASSERT_FALSE(no_coordinates.has_value());
    EXPECT_EQ(no_coordinates.error().kind(), ErrorKind::kIllegalState);

    auto trailing = polygons_from_geo_json(std::string(kSquare) + " x");
    ASSERT_FALSE(trailing.has_value());
    EXPECT_EQ(trailing.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(trailing.error().to_string().find("after end of GeoJSON object"), std::string::npos);

    auto empty_polygon = polygons_from_geo_json(R"({"type":"Polygon","coordinates":[]})");
    ASSERT_FALSE(empty_polygon.has_value());
    EXPECT_EQ(empty_polygon.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(empty_polygon.error().to_string().find("polygon array must not be empty"), std::string::npos);

    auto three_elements = polygons_from_geo_json(
            R"({"type":"Polygon","coordinates":[[[0,0,0],[1,0,0],[1,1,0],[0,1,0],[0,0,0]]]})");
    ASSERT_FALSE(three_elements.has_value());
    EXPECT_EQ(three_elements.error().kind(), ErrorKind::kIllegalState);
    EXPECT_NE(three_elements.error().to_string().find("wrong element count"), std::string::npos);

    auto bad_number = polygons_from_geo_json(
            R"({"type":"Polygon","coordinates":[[[0,-],[1,0],[1,1],[0,1],[0,0]]]})");
    ASSERT_FALSE(bad_number.has_value());
    EXPECT_EQ(bad_number.error().kind(), ErrorKind::kIllegalState);

    auto unterminated_string = polygons_from_geo_json(R"({"type":"Polygon)");
    ASSERT_FALSE(unterminated_string.has_value());
    EXPECT_EQ(unterminated_string.error().kind(), ErrorKind::kIllegalState);
}

TEST(GeoJsonPolygonParserTest, RejectsAnUnclosedPolygon) {
    // The shell does not repeat its first point, which `Polygon` rejects.
    auto polygons = polygons_from_geo_json(
            R"({"type":"Polygon","coordinates":[[[0,0],[10,0],[10,10],[0,10]]]})");
    ASSERT_FALSE(polygons.has_value());
    EXPECT_EQ(polygons.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(GeoJsonPolygonParserTest, HandlesEscapesInStrings) {
    // The CRS name goes through the string reader, so the escapes are decoded
    // before it is compared.
    const std::string_view geo_json = R"({"type":"Polygon","crs":{"type":"name",
        "properties":{"name":"urn:ogc:def:crs:OGC\u003a1.3:CRS84"}},
        "coordinates":[[[0,0],[10,0],[10,10],[0,10],[0,0]]]})";
    auto polygons = polygons_from_geo_json(geo_json);
    ASSERT_TRUE(polygons.has_value()) << (polygons ? "" : polygons.error().to_string());

    // An escape the reader does not know is reported rather than ignored.
    auto bad_escape = polygons_from_geo_json(R"({"type":"Polyg\non","coordinates":[]})");
    ASSERT_FALSE(bad_escape.has_value());
    EXPECT_NE(bad_escape.error().to_string().find("unsupported string escape"), std::string::npos);
}

TEST(GeoJsonPolygonParserTest, ExposesTheParserAsAnObject) {
    std::string text(kSquare);
    GeoJsonPolygonParser parser(std::move(text));
    auto polygons = parser.parse();
    ASSERT_TRUE(polygons.has_value()) << (polygons ? "" : polygons.error().to_string());
    EXPECT_EQ(polygons.value().size(), 1U);

    // The parser can only be consumed once: the coordinates are moved out.
    auto again = parser.parse();
    EXPECT_FALSE(again.has_value());
}
