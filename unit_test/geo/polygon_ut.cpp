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

#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.polygon;

import lucene_core;

using namespace lucene;

namespace {

/// The ten-by-ten square with a corner at the origin, closed.
std::vector<double> square_lats() { return {0.0, 0.0, 10.0, 10.0, 0.0}; }
std::vector<double> square_lons() { return {0.0, 10.0, 10.0, 0.0, 0.0}; }

/// The two-by-two hole in the middle of the square.
std::vector<double> hole_lats() { return {4.0, 4.0, 6.0, 6.0, 4.0}; }
std::vector<double> hole_lons() { return {4.0, 6.0, 6.0, 4.0, 4.0}; }

} // namespace

TEST(GeoPolygonTest, ValidatesItsVertices) {
    auto uneven = Polygon::create({0.0, 0.0, 10.0, 0.0}, {0.0, 10.0, 10.0});
    ASSERT_FALSE(uneven.has_value());
    EXPECT_EQ(uneven.error().kind(), ErrorKind::kIllegalArgument);

    auto too_few = Polygon::create({0.0, 0.0, 0.0}, {0.0, 10.0, 0.0});
    ASSERT_FALSE(too_few.has_value());
    EXPECT_EQ(too_few.error().kind(), ErrorKind::kIllegalArgument);

    // The last vertex has to repeat the first one.
    auto open = Polygon::create({0.0, 0.0, 10.0, 10.0}, {0.0, 10.0, 10.0, 0.0});
    ASSERT_FALSE(open.has_value());
    EXPECT_EQ(open.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lat = Polygon::create({0.0, 0.0, 91.0, 91.0, 0.0}, {0.0, 10.0, 10.0, 0.0, 0.0});
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = Polygon::create({0.0, 0.0, 10.0, 10.0, 0.0}, {0.0, 181.0, 181.0, 0.0, 0.0});
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(GeoPolygonTest, RejectsNestedHoles) {
    auto hole = Polygon::create(hole_lats(), hole_lons());
    ASSERT_TRUE(hole.has_value());
    auto with_hole = Polygon::create(square_lats(), square_lons(), {hole.value()});
    ASSERT_TRUE(with_hole.has_value()) << (with_hole ? "" : with_hole.error().to_string());

    // A hole that itself has a hole would nest, which the format cannot express.
    auto nested = Polygon::create(square_lats(), square_lons(), {with_hole.value()});
    ASSERT_FALSE(nested.has_value());
    EXPECT_EQ(nested.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(GeoPolygonTest, ComputesTheBoundingBoxAndTheWindingOrder) {
    auto polygon = Polygon::create(square_lats(), square_lons());
    ASSERT_TRUE(polygon.has_value()) << (polygon ? "" : polygon.error().to_string());

    EXPECT_EQ(polygon.value().num_points(), 5U);
    EXPECT_EQ(polygon.value().num_holes(), 0U);
    EXPECT_EQ(polygon.value().min_lat(), 0.0);
    EXPECT_EQ(polygon.value().max_lat(), 10.0);
    EXPECT_EQ(polygon.value().min_lon(), 0.0);
    EXPECT_EQ(polygon.value().max_lon(), 10.0);
    // The vertices (lon, lat) = (0,0), (10,0), (10,10), (0,10) wind clockwise by
    // the sign convention of the reference implementation.
    EXPECT_EQ(polygon.value().winding_order(), WindingOrder::Cw);

    EXPECT_EQ(polygon.value().lat(2), 10.0);
    EXPECT_EQ(polygon.value().lon(2), 10.0);

    // Reversing the shell flips the winding order.
    auto reversed = Polygon::create({0.0, 10.0, 10.0, 0.0, 0.0}, {0.0, 0.0, 10.0, 10.0, 0.0});
    ASSERT_TRUE(reversed.has_value());
    EXPECT_EQ(reversed.value().winding_order(), WindingOrder::Ccw);
}

TEST(GeoPolygonTest, DescribesItselfAndItsVertices) {
    auto polygon = Polygon::create({0.0, 0.0, 10.0, 0.0}, {0.0, 10.0, 10.0, 0.0});
    ASSERT_TRUE(polygon.has_value());
    // The holes are counted rather than printed (see the deviation).
    EXPECT_EQ(polygon.value().to_string(), "Polygon[0, 0] [0, 10] [10, 10] [0, 0] ");

    const std::vector<double> json_lats{0.0, 1.0};
    const std::vector<double> json_lons{2.0, 3.0};
    EXPECT_EQ(Polygon::vertices_to_geo_json(json_lats, json_lons), "[[2, 0], [3, 1]]");

    auto same = Polygon::create({0.0, 0.0, 10.0, 0.0}, {0.0, 10.0, 10.0, 0.0});
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(polygon.value().equals(same.value()));

    auto different = Polygon::create(square_lats(), square_lons());
    ASSERT_TRUE(different.has_value());
    EXPECT_FALSE(polygon.value().equals(different.value()));
}

TEST(GeoPolygonTest, ConvertsToAPolygonComponent) {
    auto polygon = Polygon::create(square_lats(), square_lons());
    ASSERT_TRUE(polygon.has_value());
    auto component = polygon.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());
    ASSERT_NE(component.value(), nullptr);

    // `(x, y)` is `(longitude, latitude)` here.
    EXPECT_EQ(component.value()->min_x(), 0.0);
    EXPECT_EQ(component.value()->max_x(), 10.0);
    EXPECT_TRUE(component.value()->contains(5.0, 5.0));
    EXPECT_TRUE(component.value()->contains(0.5, 9.5));
    EXPECT_FALSE(component.value()->contains(20.0, 5.0));
}

TEST(GeoPolygonTest, RelatesCellsToThePolygon) {
    auto polygon = Polygon::create(square_lats(), square_lons());
    ASSERT_TRUE(polygon.has_value());
    auto component = polygon.value().to_component2d();
    ASSERT_TRUE(component.has_value());

    // A tiny cell inside the shell.
    auto inside = component.value()->relate(4.0, 4.1, 4.0, 4.1);
    ASSERT_TRUE(inside.has_value());
    EXPECT_EQ(inside.value(), Relation::kCellInsideQuery);

    // A cell that contains the whole polygon reports a crossing.
    auto containing = component.value()->relate(-1.0, 11.0, -1.0, 11.0);
    ASSERT_TRUE(containing.has_value());
    EXPECT_EQ(containing.value(), Relation::kCellCrossesQuery);

    // A cell that straddles one of the edges crosses the polygon.
    auto straddling = component.value()->relate(-1.0, 5.0, 4.0, 6.0);
    ASSERT_TRUE(straddling.has_value());
    EXPECT_EQ(straddling.value(), Relation::kCellCrossesQuery);

    // A cell far outside does not touch it.
    auto outside = component.value()->relate(20.0, 21.0, 20.0, 21.0);
    ASSERT_TRUE(outside.has_value());
    EXPECT_EQ(outside.value(), Relation::kCellOutsideQuery);
}

TEST(GeoPolygonTest, IntersectsAndContainsLinesAndTriangles) {
    auto polygon = Polygon::create(square_lats(), square_lons());
    ASSERT_TRUE(polygon.has_value());
    auto component = polygon.value().to_component2d();
    ASSERT_TRUE(component.has_value());

    // A line through the polygon intersects it; a line outside does not.
    EXPECT_TRUE(component.value()->intersects_line_values(-5.0, 5.0, 15.0, 5.0));
    EXPECT_FALSE(component.value()->intersects_line_values(20.0, 20.0, 21.0, 21.0));

    // A short line inside the shell is contained; one leaving it is not.
    EXPECT_TRUE(component.value()->contains_line_values(4.0, 4.0, 6.0, 6.0));
    EXPECT_FALSE(component.value()->contains_line_values(4.0, 4.0, 15.0, 6.0));

    // A small triangle inside the shell is contained; a big one is not.
    EXPECT_TRUE(component.value()->contains_triangle_values(4.0, 4.0, 6.0, 4.0, 5.0, 6.0));
    EXPECT_FALSE(component.value()->contains_triangle_values(-1.0, -1.0, 11.0, -1.0, 5.0, 11.0));
    EXPECT_TRUE(component.value()->intersects_triangle_values(-1.0, -1.0, 11.0, -1.0, 5.0, 11.0));
}

TEST(GeoPolygonTest, ReportsTheWithinRelation) {
    auto polygon = Polygon::create(square_lats(), square_lons());
    ASSERT_TRUE(polygon.has_value());
    auto component = polygon.value().to_component2d();
    ASSERT_TRUE(component.has_value());

    auto inside = component.value()->within_point(5.0, 5.0);
    ASSERT_TRUE(inside.has_value());
    EXPECT_EQ(inside.value(), WithinRelation::NotWithin);

    auto outside = component.value()->within_point(20.0, 20.0);
    ASSERT_TRUE(outside.has_value());
    EXPECT_EQ(outside.value(), WithinRelation::Disjoint);

    auto line = component.value()->within_line_values(5.0, 5.0, true, 6.0, 6.0);
    ASSERT_TRUE(line.has_value());
    EXPECT_EQ(line.value(), WithinRelation::NotWithin);

    // An edge of the shell that does not belong to the indexed shape turns the
    // triangle into a within candidate.
    auto triangle = component.value()->within_triangle_values(-1.0, -1.0, false, 11.0, -1.0, false, 5.0, 11.0, false);
    ASSERT_TRUE(triangle.has_value());
    EXPECT_EQ(triangle.value(), WithinRelation::Candidate);
}

TEST(GeoPolygonTest, ExcludesTheHoles) {
    auto hole = Polygon::create(hole_lats(), hole_lons());
    ASSERT_TRUE(hole.has_value());
    auto polygon = Polygon::create(square_lats(), square_lons(), {hole.value()});
    ASSERT_TRUE(polygon.has_value()) << (polygon ? "" : polygon.error().to_string());
    EXPECT_EQ(polygon.value().num_holes(), 1U);

    auto component = polygon.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    // Inside the shell but outside the hole.
    EXPECT_TRUE(component.value()->contains(2.0, 2.0));
    // Inside the hole: not part of the polygon.
    EXPECT_FALSE(component.value()->contains(5.0, 5.0));
    EXPECT_FALSE(component.value()->contains(4.5, 4.5));
    // Outside the shell.
    EXPECT_FALSE(component.value()->contains(20.0, 5.0));

    // A cell entirely inside the hole is outside the polygon.
    auto hole_cell = component.value()->relate(4.5, 5.5, 4.5, 5.5);
    ASSERT_TRUE(hole_cell.has_value());
    EXPECT_EQ(hole_cell.value(), Relation::kCellOutsideQuery);

    // A cell inside the shell that does not touch the hole is inside.
    auto shell_cell = component.value()->relate(1.0, 1.1, 1.0, 1.1);
    ASSERT_TRUE(shell_cell.has_value());
    EXPECT_EQ(shell_cell.value(), Relation::kCellInsideQuery);
}

namespace {

std::vector<float> xy_square_x() { return {0.0F, 10.0F, 10.0F, 0.0F, 0.0F}; }
std::vector<float> xy_square_y() { return {0.0F, 0.0F, 10.0F, 10.0F, 0.0F}; }

std::vector<float> xy_hole_x() { return {4.0F, 6.0F, 6.0F, 4.0F, 4.0F}; }
std::vector<float> xy_hole_y() { return {4.0F, 4.0F, 6.0F, 6.0F, 4.0F}; }

} // namespace

TEST(XYPolygonTest, ValidatesItsVertices) {
    auto uneven = XYPolygon::create({0.0F, 10.0F, 10.0F, 0.0F}, {0.0F, 0.0F, 10.0F});
    ASSERT_FALSE(uneven.has_value());
    EXPECT_EQ(uneven.error().kind(), ErrorKind::kIllegalArgument);

    auto too_few = XYPolygon::create({0.0F, 10.0F, 0.0F}, {0.0F, 0.0F, 0.0F});
    ASSERT_FALSE(too_few.has_value());
    EXPECT_EQ(too_few.error().kind(), ErrorKind::kIllegalArgument);

    auto open = XYPolygon::create({0.0F, 10.0F, 10.0F, 1.0F}, {0.0F, 0.0F, 10.0F, 10.0F});
    ASSERT_FALSE(open.has_value());
    EXPECT_EQ(open.error().kind(), ErrorKind::kIllegalArgument);

    auto infinite = XYPolygon::create({0.0F, 10.0F, 10.0F, std::numeric_limits<float>::infinity()},
                                      {0.0F, 0.0F, 10.0F, 10.0F});
    ASSERT_FALSE(infinite.has_value());
    EXPECT_EQ(infinite.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(XYPolygonTest, ComputesTheBoundingBoxAndDescribesItself) {
    auto polygon = XYPolygon::create(xy_square_x(), xy_square_y());
    ASSERT_TRUE(polygon.has_value()) << (polygon ? "" : polygon.error().to_string());

    EXPECT_EQ(polygon.value().num_points(), 5U);
    EXPECT_EQ(polygon.value().num_holes(), 0U);
    EXPECT_EQ(polygon.value().min_x(), 0.0F);
    EXPECT_EQ(polygon.value().max_x(), 10.0F);
    EXPECT_EQ(polygon.value().min_y(), 0.0F);
    EXPECT_EQ(polygon.value().max_y(), 10.0F);
    EXPECT_EQ(polygon.value().x(1), 10.0F);
    EXPECT_EQ(polygon.value().y(2), 10.0F);
    EXPECT_EQ(polygon.value().winding_order(), WindingOrder::Cw);

    EXPECT_EQ(polygon.value().to_string(), "XYPolygon[0, 0] [10, 0] [10, 10] [0, 10] [0, 0] ");

    auto same = XYPolygon::create(xy_square_x(), xy_square_y());
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(polygon.value().equals(same.value()));
}

TEST(XYPolygonTest, ConvertsToAPolygonComponent) {
    auto polygon = XYPolygon::create(xy_square_x(), xy_square_y());
    ASSERT_TRUE(polygon.has_value());
    auto component = polygon.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());

    EXPECT_EQ(component.value()->min_x(), 0.0);
    EXPECT_EQ(component.value()->max_y(), 10.0);
    EXPECT_TRUE(component.value()->contains(5.0, 5.0));
    EXPECT_FALSE(component.value()->contains(20.0, 5.0));
}

TEST(XYPolygonTest, ExcludesTheHoles) {
    auto hole = XYPolygon::create(xy_hole_x(), xy_hole_y());
    ASSERT_TRUE(hole.has_value());
    auto polygon = XYPolygon::create(xy_square_x(), xy_square_y(), {hole.value()});
    ASSERT_TRUE(polygon.has_value()) << (polygon ? "" : polygon.error().to_string());
    EXPECT_EQ(polygon.value().num_holes(), 1U);
    EXPECT_EQ(polygon.value().to_string(),
              "XYPolygon[0, 0] [10, 0] [10, 10] [0, 10] [0, 0] , holes=[XYPolygon[4, 4] [6, 4] [6, 6] [4, 6] [4, 4] ]");

    auto component = polygon.value().to_component2d();
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());
    EXPECT_TRUE(component.value()->contains(2.0, 2.0));
    EXPECT_FALSE(component.value()->contains(5.0, 5.0));
    EXPECT_FALSE(component.value()->contains(20.0, 5.0));

    // A hole that has a hole would nest, which is rejected.
    auto nested = XYPolygon::create(xy_square_x(), xy_square_y(), {polygon.value()});
    ASSERT_FALSE(nested.has_value());
    EXPECT_EQ(nested.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(XYPolygonTest, CombinesIntoAComponentTree) {
    auto first = XYPolygon::create(xy_square_x(), xy_square_y());
    ASSERT_TRUE(first.has_value());
    auto second = XYPolygon::create({20.0F, 21.0F, 21.0F, 20.0F, 20.0F}, {20.0F, 20.0F, 21.0F, 21.0F, 20.0F});
    ASSERT_TRUE(second.has_value());

    std::vector<const Geometry *> shapes = {&first.value(), &second.value()};
    auto component = create_geometry_component(shapes);
    ASSERT_TRUE(component.has_value()) << (component ? "" : component.error().to_string());
    EXPECT_TRUE(component.value()->contains(5.0, 5.0));
    EXPECT_TRUE(component.value()->contains(20.5, 20.5));
    EXPECT_FALSE(component.value()->contains(15.0, 15.0));
}
