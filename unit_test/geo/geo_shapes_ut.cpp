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

module lucene_core:ut.geo_shapes;

import lucene_core;

using namespace lucene;

TEST(GeoRectangleTest, ValidatesItsCorners) {
    EXPECT_TRUE(Rectangle::create(-10.0, 10.0, -20.0, 20.0).has_value());

    auto bad_lat = Rectangle::create(-91.0, 10.0, -20.0, 20.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = Rectangle::create(-10.0, 10.0, -20.0, 181.0);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);

    // An inverted box would silently match nothing.
    auto inverted = Rectangle::create(10.0, -10.0, -20.0, 20.0);
    ASSERT_FALSE(inverted.has_value());
    EXPECT_EQ(inverted.error().kind(), ErrorKind::kIllegalArgument);

    // A crossing box is not inverted: `max_lon < min_lon` is allowed.
    EXPECT_TRUE(Rectangle::create(-10.0, 10.0, 170.0, -170.0).has_value());
}

TEST(GeoRectangleTest, DescribesItself) {
    auto box = Rectangle::create(-10.0, 10.0, -20.0, 20.0);
    ASSERT_TRUE(box.has_value());
    EXPECT_EQ(box.value().to_string(), "Rectangle(lat=-10 TO 10 lon=-20 TO 20)");
    EXPECT_FALSE(box.value().crosses_dateline());
    EXPECT_EQ(box.value().min_lat(), -10.0);
    EXPECT_EQ(box.value().max_lon(), 20.0);

    auto same = Rectangle::create(-10.0, 10.0, -20.0, 20.0);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(box.value().equals(same.value()));

    auto crossing = Rectangle::create(-10.0, 10.0, 170.0, -170.0);
    ASSERT_TRUE(crossing.has_value());
    EXPECT_TRUE(crossing.value().crosses_dateline());
    EXPECT_EQ(crossing.value().to_string(), "Rectangle(lat=-10 TO 10 lon=170 TO -170 [crosses dateline!])");
}

TEST(GeoRectangleTest, ConvertsToARectangleComponent) {
    auto box = Rectangle::create(-10.0, 10.0, -20.0, 20.0);
    ASSERT_TRUE(box.has_value());
    auto component = box.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());
    ASSERT_NE(component.value(), nullptr);

    // `contains(x, y)` takes the longitude first.
    EXPECT_TRUE(component.value()->contains(0.0, 0.0));
    EXPECT_TRUE(component.value()->contains(19.0, -9.0));
    EXPECT_FALSE(component.value()->contains(0.0, 25.0));
    EXPECT_FALSE(component.value()->contains(25.0, 0.0));

    // The box is quantized inwards, so it is never larger than the request.
    EXPECT_GE(component.value()->min_x(), -20.0);
    EXPECT_LE(component.value()->max_x(), 20.0);
    EXPECT_GE(component.value()->min_y(), -10.0);
    EXPECT_LE(component.value()->max_y(), 10.0);
}

TEST(GeoRectangleTest, SplitsABoxThatCrossesTheDateline) {
    auto box = Rectangle::create(-10.0, 10.0, 170.0, -170.0);
    ASSERT_TRUE(box.has_value());
    auto component = box.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    // Both sides of the dateline are inside, the rest of the world is not.
    EXPECT_TRUE(component.value()->contains(175.0, 0.0));
    EXPECT_TRUE(component.value()->contains(-175.0, 0.0));
    EXPECT_FALSE(component.value()->contains(0.0, 0.0));
    EXPECT_FALSE(component.value()->contains(160.0, 0.0));
}

TEST(GeoRectangleTest, TreatsABoxStartingAt180AsNotCrossing) {
    // 180 to -170 wraps, but the reference implementation starts it at -180
    // instead, which is the same box without a dateline crossing.
    auto box = Rectangle::create(-10.0, 10.0, 180.0, -170.0);
    ASSERT_TRUE(box.has_value());
    auto component = box.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    EXPECT_TRUE(component.value()->contains(-175.0, 0.0));
    EXPECT_FALSE(component.value()->contains(175.0, 0.0));
}

TEST(XYRectangleTest, ValidatesItsBounds) {
    EXPECT_TRUE(XYRectangle::create(-1.0F, 1.0F, -1.0F, 1.0F).has_value());

    auto inverted_x = XYRectangle::create(1.0F, -1.0F, -1.0F, 1.0F);
    ASSERT_FALSE(inverted_x.has_value());
    EXPECT_EQ(inverted_x.error().kind(), ErrorKind::kIllegalArgument);

    auto inverted_y = XYRectangle::create(-1.0F, 1.0F, 1.0F, -1.0F);
    ASSERT_FALSE(inverted_y.has_value());
    EXPECT_EQ(inverted_y.error().kind(), ErrorKind::kIllegalArgument);

    auto nan = XYRectangle::create(std::numeric_limits<float>::quiet_NaN(), 1.0F, -1.0F, 1.0F);
    ASSERT_FALSE(nan.has_value());
    EXPECT_EQ(nan.error().kind(), ErrorKind::kIllegalArgument);

    auto box = XYRectangle::create(-1.5F, 2.5F, -3.5F, 4.5F);
    ASSERT_TRUE(box.has_value());
    EXPECT_EQ(box.value().to_string(), "XYRectangle(x=-1.5 TO 2.5 y=-3.5 TO 4.5)");
    auto same = XYRectangle::create(-1.5F, 2.5F, -3.5F, 4.5F);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(box.value().equals(same.value()));
}

TEST(XYRectangleTest, GrowsABoxAroundACircle) {
    // The box is rounded up, so it is never smaller than the circle.
    auto box = XYRectangle::from_point_distance(1.0F, 2.0F, 0.5F);
    ASSERT_TRUE(box.has_value()) << (box ? "" : box.error().to_string());
    EXPECT_LE(box.value().min_x(), 0.5F);
    EXPECT_GE(box.value().max_x(), 1.5F);
    EXPECT_LE(box.value().min_y(), 1.5F);
    EXPECT_GE(box.value().max_y(), 2.5F);

    auto negative = XYRectangle::from_point_distance(0.0F, 0.0F, -1.0F);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);

    auto infinite = XYRectangle::from_point_distance(0.0F, 0.0F, std::numeric_limits<float>::infinity());
    ASSERT_FALSE(infinite.has_value());
    EXPECT_EQ(infinite.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(XYRectangleTest, ConvertsToARectangleComponent) {
    auto box = XYRectangle::create(-1.0F, 1.0F, -2.0F, 2.0F);
    ASSERT_TRUE(box.has_value());
    auto component = box.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    EXPECT_TRUE(component.value()->contains(0.0, 0.0));
    EXPECT_FALSE(component.value()->contains(0.0, 2.5));
    // The cartesian box is not quantized.
    EXPECT_EQ(component.value()->min_x(), -1.0);
    EXPECT_EQ(component.value()->max_y(), 2.0);
}

TEST(GeoPointTest, ValidatesAndQuantizesItsCoordinates) {
    auto point = Point::create(12.345, 67.89);
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(point.value().lat(), 12.345);
    EXPECT_EQ(point.value().lon(), 67.89);
    EXPECT_EQ(point.value().to_string(), "Point(67.89,12.345)");

    auto bad_lat = Point::create(-90.5, 0.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);
    auto bad_lon = Point::create(0.0, 180.5);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);

    auto component = point.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());
    // A point is a zero-area box at the quantized coordinate.
    EXPECT_EQ(component.value()->min_x(), component.value()->max_x());
    EXPECT_EQ(component.value()->min_y(), component.value()->max_y());
    EXPECT_TRUE(component.value()->contains(component.value()->min_x(), component.value()->min_y()));
    // The quantization moves the coordinate by less than a milli-degree.
    EXPECT_NEAR(component.value()->min_x(), 67.89, 1e-3);
    EXPECT_NEAR(component.value()->min_y(), 12.345, 1e-3);
}

TEST(GeoPointTest, KeepsTheExtremeCoordinates) {
    // Rounding the maximum latitude up would leave the valid range, so it is kept.
    auto point = Point::create(GeoUtils::MAX_LAT_INCL, GeoUtils::MAX_LON_INCL);
    ASSERT_TRUE(point.has_value());
    auto component = point.value().to_component2d();
    ASSERT_TRUE(component.has_value());
    EXPECT_EQ(component.value()->min_y(), GeoUtils::MAX_LAT_INCL);
    EXPECT_EQ(component.value()->min_x(), GeoUtils::MAX_LON_INCL);
}

TEST(XYPointTest, ValidatesAndConvertsItsCoordinates) {
    auto point = XYPoint::create(1.5F, -2.5F);
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(point.value().to_string(), "XYPoint(1.5,-2.5)");
    auto same = XYPoint::create(1.5F, -2.5F);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(point.value().equals(same.value()));

    auto nan = XYPoint::create(std::numeric_limits<float>::quiet_NaN(), 0.0F);
    ASSERT_FALSE(nan.has_value());
    EXPECT_EQ(nan.error().kind(), ErrorKind::kIllegalArgument);

    auto component = point.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());
    EXPECT_EQ(component.value()->min_x(), 1.5);
    EXPECT_EQ(component.value()->min_y(), -2.5);
    EXPECT_TRUE(component.value()->contains(1.5, -2.5));
}

TEST(GeoGeometryTest, BuildsOneComponentFromSeveralShapes) {
    auto first = Rectangle::create(-10.0, 10.0, -20.0, 20.0);
    ASSERT_TRUE(first.has_value());
    auto second = Rectangle::create(40.0, 50.0, 60.0, 70.0);
    ASSERT_TRUE(second.has_value());

    // An empty list is an error.
    auto empty = create_geometry_component({});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind(), ErrorKind::kIllegalArgument);

    // A single shape is used as it is.
    std::vector<const Geometry *> single = {&first.value()};
    auto one = create_geometry_component(single);
    ASSERT_TRUE(one.has_value()) << (one ? "" : one.error().to_string());
    EXPECT_TRUE(one.value()->contains(0.0, 0.0));
    EXPECT_FALSE(one.value()->contains(65.0, 45.0));

    // Several shapes become a component tree that matches the union.
    std::vector<const Geometry *> both = {&first.value(), &second.value()};
    auto union_component = create_geometry_component(both);
    ASSERT_TRUE(union_component.has_value()) << (union_component ? "" : union_component.error().to_string());
    EXPECT_TRUE(union_component.value()->contains(0.0, 0.0));
    EXPECT_TRUE(union_component.value()->contains(65.0, 45.0));
    EXPECT_FALSE(union_component.value()->contains(0.0, 65.0));

    // A null shape is rejected.
    std::vector<const Geometry *> with_null = {nullptr};
    auto bad = create_geometry_component(with_null);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind(), ErrorKind::kIllegalArgument);
}
