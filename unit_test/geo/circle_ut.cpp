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

module lucene_core:ut.circle;

import lucene_core;

using namespace lucene;

namespace {

/// 100 km in degrees at the equator.
constexpr double kHundredKmDegrees = 0.89932;

} // namespace

TEST(CircleTest, ValidatesTheCenterAndTheRadius) {
    EXPECT_TRUE(Circle::create(0.0, 0.0, 1000.0).has_value());

    auto bad_lat = Circle::create(90.5, 0.0, 1000.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = Circle::create(0.0, -180.5, 1000.0);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);

    auto negative_radius = Circle::create(0.0, 0.0, -1.0);
    ASSERT_FALSE(negative_radius.has_value());
    EXPECT_EQ(negative_radius.error().kind(), ErrorKind::kIllegalArgument);

    auto infinite_radius = Circle::create(0.0, 0.0, std::numeric_limits<double>::infinity());
    ASSERT_FALSE(infinite_radius.has_value());
    EXPECT_EQ(infinite_radius.error().kind(), ErrorKind::kIllegalArgument);

    // A zero radius is a point, which is allowed.
    EXPECT_TRUE(Circle::create(12.5, -34.5, 0.0).has_value());
}

TEST(CircleTest, DescribesItself) {
    auto circle = Circle::create(1.0, 2.0, 3.0);
    ASSERT_TRUE(circle.has_value());
    EXPECT_EQ(circle.value().lat(), 1.0);
    EXPECT_EQ(circle.value().lon(), 2.0);
    EXPECT_EQ(circle.value().radius(), 3.0);
    EXPECT_EQ(circle.value().to_string(), "Circle([1,2] radius = 3 meters)");

    auto same = Circle::create(1.0, 2.0, 3.0);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(circle.value().equals(same.value()));

    auto different = Circle::create(1.0, 2.0, 4.0);
    ASSERT_TRUE(different.has_value());
    EXPECT_FALSE(circle.value().equals(different.value()));
}

TEST(Circle2DTest, ComputesTheBoundingBoxOfACircle) {
    auto circle = Circle::create(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(circle.has_value());
    auto component = Circle2D::from_circle(circle.value());
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    EXPECT_NEAR(component.value()->min_x(), -kHundredKmDegrees, 1e-3);
    EXPECT_NEAR(component.value()->max_x(), kHundredKmDegrees, 1e-3);
    EXPECT_NEAR(component.value()->min_y(), -kHundredKmDegrees, 1e-3);
    EXPECT_NEAR(component.value()->max_y(), kHundredKmDegrees, 1e-3);
}

TEST(Circle2DTest, ContainsThePointsWithinTheRadius) {
    auto circle = Circle::create(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(circle.has_value());
    auto component = Circle2D::from_circle(circle.value());
    ASSERT_TRUE(component.has_value());

    // 0.5 degrees of longitude at the equator is about 55.6 km.
    EXPECT_TRUE(component.value()->contains(0.5, 0.0));
    EXPECT_TRUE(component.value()->contains(0.0, 0.5));
    EXPECT_TRUE(component.value()->contains(0.0, 0.0));
    // One degree is about 111 km, so it is outside.
    EXPECT_FALSE(component.value()->contains(1.0, 0.0));
    EXPECT_FALSE(component.value()->contains(0.0, 1.0));
    // Diagonal distances grow faster than the box.
    EXPECT_FALSE(component.value()->contains(0.7, 0.7));
}

TEST(Circle2DTest, WrapsACircleOverTheDateline) {
    // The circle reaches past 180 degrees, so its box wraps around.
    auto circle = Circle::create(0.0, 179.8, 50'000.0);
    ASSERT_TRUE(circle.has_value());
    auto component = Circle2D::from_circle(circle.value());
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    // The bounding box covers every longitude of the world.
    EXPECT_EQ(component.value()->min_x(), GeoUtils::MIN_LON_INCL);
    EXPECT_EQ(component.value()->max_x(), GeoUtils::MAX_LON_INCL);
    EXPECT_NEAR(component.value()->min_y(), -0.4497, 1e-3);
    EXPECT_NEAR(component.value()->max_y(), 0.4497, 1e-3);

    // Both sides of the dateline are inside, the rest of the world is not.
    EXPECT_TRUE(component.value()->contains(179.9, 0.0));
    EXPECT_TRUE(component.value()->contains(-179.9, 0.0));
    EXPECT_FALSE(component.value()->contains(0.0, 0.0));
    EXPECT_FALSE(component.value()->contains(-170.0, 0.0));
}

TEST(Circle2DTest, SpansEveryLongitudeWhenItReachesAPole) {
    auto circle = Circle::create(89.9, 0.0, 50'000.0);
    ASSERT_TRUE(circle.has_value());
    auto component = Circle2D::from_circle(circle.value());
    ASSERT_TRUE(component.has_value());

    EXPECT_EQ(component.value()->min_x(), GeoUtils::MIN_LON_INCL);
    EXPECT_EQ(component.value()->max_x(), GeoUtils::MAX_LON_INCL);
    EXPECT_NEAR(component.value()->max_y(), GeoUtils::MAX_LAT_INCL, 1e-9);

    EXPECT_TRUE(component.value()->contains(123.0, 89.95));
    EXPECT_TRUE(component.value()->contains(-45.0, 89.99));
    // 0.9 degrees of latitude is about 100 km, further than the radius.
    EXPECT_FALSE(component.value()->contains(0.0, 89.0));
}

TEST(Circle2DTest, RelatesCellsToTheCircle) {
    auto circle = Circle::create(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(circle.has_value());
    auto component = Circle2D::from_circle(circle.value());
    ASSERT_TRUE(component.has_value());

    // A tiny cell around the center is completely inside.
    auto inside = component.value()->relate(-0.0001, 0.0001, -0.0001, 0.0001);
    ASSERT_TRUE(inside.has_value());
    EXPECT_EQ(inside.value(), Relation::kCellInsideQuery);

    // The bounding box cell crosses the circle.
    auto crossing = component.value()->relate(-0.9, 0.9, -0.9, 0.9);
    ASSERT_TRUE(crossing.has_value());
    EXPECT_EQ(crossing.value(), Relation::kCellCrossesQuery);

    // A cell far away is outside.
    auto outside = component.value()->relate(10.0, 11.0, 10.0, 11.0);
    ASSERT_TRUE(outside.has_value());
    EXPECT_EQ(outside.value(), Relation::kCellOutsideQuery);
}

TEST(Circle2DTest, IntersectsLinesAndTriangles) {
    auto circle = Circle::create(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(circle.has_value());
    auto component = Circle2D::from_circle(circle.value());
    ASSERT_TRUE(component.has_value());

    // A line crossing the center intersects the circle.
    EXPECT_TRUE(component.value()->intersects_line_values(-1.0, 0.0, 1.0, 0.0));
    // A line outside the bounding box does not.
    EXPECT_FALSE(component.value()->intersects_line_values(10.0, 10.0, 11.0, 11.0));
    // A line that passes close by, inside the box but outside the radius, does
    // not: at 0.85 degrees of latitude the closest point is about 94.5 km away,
    // which is inside the box but not inside the circle of 100 km... so the
    // segment along y = 0.85 still crosses the circle.
    EXPECT_TRUE(component.value()->intersects_line_values(-1.0, 0.85, 1.0, 0.85));
    // A short segment entirely outside the radius does not.
    EXPECT_FALSE(component.value()->intersects_line_values(0.5, 0.85, 0.6, 0.85));

    // A triangle around the center intersects; one far away does not.
    EXPECT_TRUE(component.value()->intersects_triangle_values(-0.5, -0.5, 0.5, -0.5, 0.0, 0.5));
    EXPECT_FALSE(component.value()->intersects_triangle_values(10.0, 10.0, 11.0, 10.0, 10.0, 11.0));

    // A small line and triangle inside the circle are contained; a crossing one
    // is not.
    EXPECT_TRUE(component.value()->contains_line_values(-0.1, 0.0, 0.1, 0.0));
    EXPECT_FALSE(component.value()->contains_line_values(-1.0, 0.0, 1.0, 0.0));
    EXPECT_TRUE(component.value()->contains_triangle_values(-0.1, -0.1, 0.1, -0.1, 0.0, 0.1));
    EXPECT_FALSE(component.value()->contains_triangle_values(-1.0, -1.0, 1.0, -1.0, 0.0, 1.0));
}

TEST(Circle2DTest, ReportsTheWithinRelation) {
    auto circle = Circle::create(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(circle.has_value());
    auto component = Circle2D::from_circle(circle.value());
    ASSERT_TRUE(component.has_value());

    // A point inside the circle is not "within" it.
    auto inside = component.value()->within_point(0.0, 0.0);
    ASSERT_TRUE(inside.has_value());
    EXPECT_EQ(inside.value(), WithinRelation::NotWithin);

    // A point outside is disjoint.
    auto outside = component.value()->within_point(10.0, 10.0);
    ASSERT_TRUE(outside.has_value());
    EXPECT_EQ(outside.value(), WithinRelation::Disjoint);

    // A line whose end point is inside is not within; a line entirely outside the
    // bounding box is disjoint.
    auto line_not_within = component.value()->within_line_values(0.0, 0.0, true, 0.1, 0.1);
    ASSERT_TRUE(line_not_within.has_value());
    EXPECT_EQ(line_not_within.value(), WithinRelation::NotWithin);
    auto line_disjoint = component.value()->within_line_values(10.0, 10.0, true, 11.0, 11.0);
    ASSERT_TRUE(line_disjoint.has_value());
    EXPECT_EQ(line_disjoint.value(), WithinRelation::Disjoint);

    // A triangle that contains the center, with every edge further away than the
    // radius, is a within candidate.
    auto triangle = component.value()->within_triangle_values(-3.0, -3.0, false, 3.0, -3.0, false, 0.0, 3.0, false);
    ASSERT_TRUE(triangle.has_value());
    EXPECT_EQ(triangle.value(), WithinRelation::Candidate);
}

TEST(Circle2DTest, MeasuresCartesianCircles) {
    auto circle = XYCircle::create(0.0F, 0.0F, 1.0F);
    ASSERT_TRUE(circle.has_value());
    auto component = Circle2D::from_xy_circle(circle.value());
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    // The box is rounded up, so it is not smaller than the circle.
    EXPECT_LE(component.value()->min_x(), -1.0);
    EXPECT_GE(component.value()->max_x(), 1.0);
    EXPECT_LE(component.value()->min_y(), -1.0);
    EXPECT_GE(component.value()->max_y(), 1.0);

    EXPECT_TRUE(component.value()->contains(0.5, 0.5));
    // The corner of the unit square is further than one from the center.
    EXPECT_FALSE(component.value()->contains(0.8, 0.8));

    auto inside = component.value()->relate(-0.01, 0.01, -0.01, 0.01);
    ASSERT_TRUE(inside.has_value());
    EXPECT_EQ(inside.value(), Relation::kCellInsideQuery);

    auto outside = component.value()->relate(10.0, 11.0, 10.0, 11.0);
    ASSERT_TRUE(outside.has_value());
    EXPECT_EQ(outside.value(), Relation::kCellOutsideQuery);

    EXPECT_TRUE(component.value()->intersects_line_values(-2.0, 0.0, 2.0, 0.0));
    EXPECT_FALSE(component.value()->intersects_line_values(2.0, 2.0, 3.0, 2.0));
}

TEST(XYCircleTest, ValidatesItsArguments) {
    EXPECT_TRUE(XYCircle::create(0.0F, 0.0F, 1.0F).has_value());

    auto zero_radius = XYCircle::create(0.0F, 0.0F, 0.0F);
    ASSERT_FALSE(zero_radius.has_value());
    EXPECT_EQ(zero_radius.error().kind(), ErrorKind::kIllegalArgument);

    auto infinite_radius = XYCircle::create(0.0F, 0.0F, std::numeric_limits<float>::infinity());
    ASSERT_FALSE(infinite_radius.has_value());
    EXPECT_EQ(infinite_radius.error().kind(), ErrorKind::kIllegalArgument);

    auto nan_center = XYCircle::create(std::numeric_limits<float>::quiet_NaN(), 0.0F, 1.0F);
    ASSERT_FALSE(nan_center.has_value());
    EXPECT_EQ(nan_center.error().kind(), ErrorKind::kIllegalArgument);

    auto circle = XYCircle::create(1.5F, -2.5F, 3.5F);
    ASSERT_TRUE(circle.has_value());
    EXPECT_EQ(circle.value().to_string(), "XYCircle([1.5,-2.5] radius = 3.5)");
    auto same = XYCircle::create(1.5F, -2.5F, 3.5F);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(circle.value().equals(same.value()));
}

TEST(CircleTest, ConvertsToAComponent2D) {
    auto circle = Circle::create(0.0, 0.0, 100'000.0);
    ASSERT_TRUE(circle.has_value());
    auto component = circle.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());
    ASSERT_NE(component.value(), nullptr);
    EXPECT_TRUE(component.value()->contains(0.0, 0.0));

    auto xy = XYCircle::create(0.0F, 0.0F, 1.0F);
    ASSERT_TRUE(xy.has_value());
    auto xy_component = xy.value().to_component2d();
    ASSERT_TRUE(xy_component.has_value());
    ASSERT_NE(xy_component.value(), nullptr);
    EXPECT_TRUE(xy_component.value()->contains(0.5, 0.5));
}
