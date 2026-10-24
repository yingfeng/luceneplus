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

export module lucene_core:point2d;

import :types;
import std;

import :error;
import :geo_component2d;
import :point_values;

namespace lucene {

/// A single point, as a query shape.
///
/// Port of `Point2D` (`core/geo/point2d.rs`).  The point is a degenerate
/// component: a zero-area bounding box that only ever relates to the cell that
/// contains it.
///
/// **Deviation:** the reference implementation also has `create_from_point` /
/// `create_from_xy_point`, which need the lat/lon and XY point *geometries*; the
/// port keeps those conversions in `:geo_point` (the shape module) so this module
/// has no dependency on them.
export class Point2D final : public Component2D {
public:
    Point2D(double x, double y) : x_(x), y_(y) {}

    [[nodiscard]] double x() const noexcept { return x_; }
    [[nodiscard]] double y() const noexcept { return y_; }

    // ---- Component2D ----

    [[nodiscard]] double min_x() const override { return x_; }
    [[nodiscard]] double max_x() const override { return x_; }
    [[nodiscard]] double min_y() const override { return y_; }
    [[nodiscard]] double max_y() const override { return y_; }

    [[nodiscard]] bool contains(double x, double y) const override { return x == x_ && y == y_; }

    [[nodiscard]] Result<Relation> relate(double min_x, double max_x, double min_y, double max_y) const override;

    [[nodiscard]] bool intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                       double b_x, double b_y) const override;

    [[nodiscard]] bool intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                           double a_y, double b_x, double b_y, double c_x, double c_y) const override;

    [[nodiscard]] bool contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                     double b_x, double b_y) const override;

    [[nodiscard]] bool contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                         double b_x, double b_y, double c_x, double c_y) const override;

    [[nodiscard]] Result<WithinRelation> within_point(double x, double y) const override;

    [[nodiscard]] Result<WithinRelation> within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                     double a_y, bool ab, double b_x, double b_y) const override;

    [[nodiscard]] Result<WithinRelation> within_triangle(double min_x, double max_x, double min_y, double max_y,
                                                         double a_x, double a_y, bool ab, double b_x, double b_y,
                                                         bool bc, double c_x, double c_y, bool ca) const override;

private:
    double x_;
    double y_;
};

} // namespace lucene
