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
#include <string>
#include <vector>

module lucene_core:ut.shape_field;

import lucene_core;

using namespace lucene;

namespace {

/// Encodes a triangle and decodes it again.
DecodedTriangle round_trip(int32_t a_y, int32_t a_x, bool ab, int32_t b_y, int32_t b_x, bool bc, int32_t c_y,
                           int32_t c_x, bool ca) {
    std::vector<uint8_t> bytes(SHAPE_TRIANGLE_BYTES, 0);
    EXPECT_TRUE(encode_triangle(bytes, a_y, a_x, ab, b_y, b_x, bc, c_y, c_x, ca).has_value());

    DecodedTriangle triangle;
    EXPECT_TRUE(decode_triangle(bytes, triangle).has_value());
    return triangle;
}

} // namespace

TEST(ShapeFieldTest, NamesTheQueryRelations) {
    EXPECT_EQ(query_relation_to_string(QueryRelation::Intersects), "Intersects");
    EXPECT_EQ(query_relation_to_string(QueryRelation::Within), "Within");
    EXPECT_EQ(query_relation_to_string(QueryRelation::Disjoint), "Disjoint");
    EXPECT_EQ(query_relation_to_string(QueryRelation::Contains), "Contains");
}

TEST(ShapeFieldTest, RoundTripsATriangle) {
    // a = (10, 10) is the westernmost vertex and the triangle is already
    // counter-clockwise, so the encoding keeps the vertices as they are.
    const DecodedTriangle triangle = round_trip(10, 10, true, 10, 20, true, 20, 15, true);

    EXPECT_EQ(triangle.type, DecodedTriangleType::Triangle);
    EXPECT_EQ(triangle.a_x, 10);
    EXPECT_EQ(triangle.a_y, 10);
    EXPECT_EQ(triangle.b_x, 20);
    EXPECT_EQ(triangle.b_y, 10);
    EXPECT_EQ(triangle.c_x, 15);
    EXPECT_EQ(triangle.c_y, 20);

    // The encoded orientation is counter-clockwise.
    EXPECT_GE(GeoUtils::orient(static_cast<double>(triangle.a_x), static_cast<double>(triangle.a_y),
                               static_cast<double>(triangle.b_x), static_cast<double>(triangle.b_y),
                               static_cast<double>(triangle.c_x), static_cast<double>(triangle.c_y)),
              0);
}

TEST(ShapeFieldTest, KeepsTheEdgeFlags) {
    const DecodedTriangle triangle = round_trip(10, 10, false, 10, 20, true, 20, 15, false);
    EXPECT_FALSE(triangle.ab);
    EXPECT_TRUE(triangle.bc);
    EXPECT_FALSE(triangle.ca);
    EXPECT_EQ(triangle.type, DecodedTriangleType::Triangle);
}

TEST(ShapeFieldTest, NormalizesTheRotation) {
    // The same triangle, starting at its easternmost vertex: the encoder rotates
    // it back, so both spellings decode to the same triangle.
    const DecodedTriangle canonical = round_trip(10, 10, true, 10, 20, true, 20, 15, true);
    const DecodedTriangle rotated = round_trip(10, 20, true, 10, 10, true, 20, 15, true);

    EXPECT_TRUE(rotated.equals(canonical)) << rotated.to_string() << " vs " << canonical.to_string();
}

TEST(ShapeFieldTest, EncodesAVerticalTriangle) {
    // All three vertices share the x coordinate, so the encoder starts at the
    // southernmost one.
    const DecodedTriangle triangle = round_trip(10, 15, true, 20, 15, true, 15, 15, true);
    EXPECT_EQ(triangle.type, DecodedTriangleType::Triangle);
    EXPECT_EQ(triangle.a_x, 15);
    EXPECT_EQ(triangle.a_y, 10);
    EXPECT_EQ(triangle.b_y, 20);
    EXPECT_EQ(triangle.c_y, 15);
    EXPECT_GE(GeoUtils::orient(static_cast<double>(triangle.a_x), static_cast<double>(triangle.a_y),
                               static_cast<double>(triangle.b_x), static_cast<double>(triangle.b_y),
                               static_cast<double>(triangle.c_x), static_cast<double>(triangle.c_y)),
              0);
}

TEST(ShapeFieldTest, ResolvesTheDegenerateShapes) {
    // All three vertices equal: a point.
    const DecodedTriangle point = round_trip(5, 5, true, 5, 5, true, 5, 5, true);
    EXPECT_EQ(point.type, DecodedTriangleType::Point);

    // Two vertices equal: a line, with the duplicate folded away.
    const DecodedTriangle line = round_trip(5, 5, false, 5, 5, true, 10, 10, true);
    EXPECT_EQ(line.type, DecodedTriangleType::Line);
    EXPECT_EQ(line.a_x, 5);
    EXPECT_EQ(line.a_y, 5);
    EXPECT_EQ(line.b_x, 10);
    EXPECT_EQ(line.b_y, 10);
    EXPECT_EQ(line.c_x, line.a_x);
    EXPECT_EQ(line.c_y, line.a_y);
    // The edge that survived is the one the original shape had.
    EXPECT_TRUE(line.ab);
}

TEST(ShapeFieldTest, RejectsBadBuffers) {
    std::vector<uint8_t> too_small(SHAPE_TRIANGLE_BYTES - 1, 0);
    auto encoded = encode_triangle(too_small, 0, 0, true, 0, 1, true, 1, 0, true);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().kind(), ErrorKind::kIllegalArgument);

    DecodedTriangle triangle;
    auto decoded = decode_triangle(too_small, triangle);
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().kind(), ErrorKind::kIllegalArgument);

}

TEST(ShapeFieldTest, ComparesAndPrintsDecodedTriangles) {
    DecodedTriangle triangle;
    triangle.a_x = 1;
    triangle.a_y = 2;
    triangle.b_x = 3;
    triangle.b_y = 4;
    triangle.c_x = 5;
    triangle.c_y = 6;
    triangle.ab = true;
    triangle.bc = false;
    triangle.ca = true;

    EXPECT_EQ(triangle.to_string(), "1, 2 3, 4 5, 6 [true,false,true]");

    DecodedTriangle same = triangle;
    EXPECT_TRUE(same.equals(triangle));
    EXPECT_EQ(same.hash(), triangle.hash());

    same.bc = true;
    EXPECT_FALSE(same.equals(triangle));
}
