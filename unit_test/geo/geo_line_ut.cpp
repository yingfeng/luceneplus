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

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.geo_line;

import lucene_core;

using namespace lucene;

TEST(GeoLineTest, ValidatesItsVertices) {
    auto uneven = Line::create({0.0, 1.0}, {0.0});
    ASSERT_FALSE(uneven.has_value());
    EXPECT_EQ(uneven.error().kind(), ErrorKind::kIllegalArgument);

    auto single = Line::create({0.0}, {0.0});
    ASSERT_FALSE(single.has_value());
    EXPECT_EQ(single.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lat = Line::create({0.0, 91.0}, {0.0, 1.0});
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = Line::create({0.0, 1.0}, {0.0, -181.0});
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(GeoLineTest, ComputesTheBoundingBoxAndDescribesItself) {
    auto line = Line::create({-1.0, 2.0, 5.0}, {10.0, -20.0, 0.0});
    ASSERT_TRUE(line.has_value()) << (line ? "" : line.error().to_string());

    EXPECT_EQ(line.value().num_points(), 3U);
    EXPECT_EQ(line.value().lat(0), -1.0);
    EXPECT_EQ(line.value().lon(1), -20.0);
    EXPECT_EQ(line.value().min_lat(), -1.0);
    EXPECT_EQ(line.value().max_lat(), 5.0);
    EXPECT_EQ(line.value().min_lon(), -20.0);
    EXPECT_EQ(line.value().max_lon(), 10.0);

    EXPECT_EQ(line.value().to_string(), "Line([10, -1][-20, 2][0, 5])");

    auto same = Line::create({-1.0, 2.0, 5.0}, {10.0, -20.0, 0.0});
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(line.value().equals(same.value()));

    auto different = Line::create({-1.0, 2.0}, {10.0, -20.0});
    ASSERT_TRUE(different.has_value());
    EXPECT_FALSE(line.value().equals(different.value()));
}

TEST(GeoLineTest, ConvertsToALineComponent) {
    // A diagonal from (lon 0, lat 0) to (lon 10, lat 10).
    auto line = Line::create({0.0, 10.0}, {0.0, 10.0});
    ASSERT_TRUE(line.has_value());
    auto component = line.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());
    ASSERT_NE(component.value(), nullptr);

    EXPECT_EQ(component.value()->min_x(), 0.0);
    EXPECT_EQ(component.value()->max_x(), 10.0);
    EXPECT_EQ(component.value()->min_y(), 0.0);
    EXPECT_EQ(component.value()->max_y(), 10.0);

    // `(x, y)` is `(longitude, latitude)` here.
    EXPECT_TRUE(component.value()->contains(5.0, 5.0));
    EXPECT_FALSE(component.value()->contains(5.0, 6.0));

    // A segment crossing the diagonal intersects it; one outside the box does not.
    EXPECT_TRUE(component.value()->intersects_line_values(-1.0, 5.0, 11.0, 5.0));
    EXPECT_FALSE(component.value()->intersects_line_values(20.0, 20.0, 21.0, 21.0));
}

TEST(GeoLineTest, KeepsALineWithThreeVertices) {
    auto line = Line::create({0.0, 5.0, 10.0}, {0.0, 5.0, 0.0});
    ASSERT_TRUE(line.has_value());
    auto component = line.value().to_component2d();
    ASSERT_TRUE(component.has_value());

    // The vertices are (lon, lat) = (0,0), (5,5), (0,10).
    EXPECT_TRUE(component.value()->contains(2.5, 2.5));
    EXPECT_TRUE(component.value()->contains(2.5, 7.5));
    EXPECT_FALSE(component.value()->contains(7.5, 2.5));
    EXPECT_FALSE(component.value()->contains(5.0, 0.0));
}

TEST(XYLineTest, ValidatesItsVertices) {
    auto uneven = XYLine::create({0.0F, 1.0F}, {0.0F});
    ASSERT_FALSE(uneven.has_value());
    EXPECT_EQ(uneven.error().kind(), ErrorKind::kIllegalArgument);

    auto single = XYLine::create({0.0F}, {0.0F});
    ASSERT_FALSE(single.has_value());
    EXPECT_EQ(single.error().kind(), ErrorKind::kIllegalArgument);

    auto nan = XYLine::create({0.0F, std::numeric_limits<float>::quiet_NaN()}, {0.0F, 1.0F});
    ASSERT_FALSE(nan.has_value());
    EXPECT_EQ(nan.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(XYLineTest, ComputesTheBoundingBoxAndDescribesItself) {
    auto line = XYLine::create({1.5F, -2.5F, 3.5F}, {-1.0F, 2.0F, 0.5F});
    ASSERT_TRUE(line.has_value()) << (line ? "" : line.error().to_string());

    EXPECT_EQ(line.value().num_points(), 3U);
    EXPECT_EQ(line.value().x(0), 1.5F);
    EXPECT_EQ(line.value().y(1), 2.0F);
    EXPECT_EQ(line.value().min_x(), -2.5F);
    EXPECT_EQ(line.value().max_x(), 3.5F);
    EXPECT_EQ(line.value().min_y(), -1.0F);
    EXPECT_EQ(line.value().max_y(), 2.0F);

    EXPECT_EQ(line.value().to_string(), "XYLine([1.5, -1][-2.5, 2][3.5, 0.5])");

    auto same = XYLine::create({1.5F, -2.5F, 3.5F}, {-1.0F, 2.0F, 0.5F});
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(line.value().equals(same.value()));
}

TEST(XYLineTest, ConvertsToALineComponent) {
    auto line = XYLine::create({0.0F, 1.0F}, {0.0F, 1.0F});
    ASSERT_TRUE(line.has_value());
    auto component = line.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    EXPECT_EQ(component.value()->min_x(), 0.0);
    EXPECT_EQ(component.value()->max_y(), 1.0);
    EXPECT_TRUE(component.value()->contains(0.5, 0.5));
    EXPECT_FALSE(component.value()->contains(0.5, 0.0));
    EXPECT_TRUE(component.value()->intersects_line_values(-1.0, 0.5, 2.0, 0.5));
}
