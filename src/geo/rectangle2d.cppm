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

export module lucene_core:rectangle2d;

import :types;
import std;

import :error;
import :geo_component2d;
import :point_values;

namespace lucene {

/// An axis-aligned rectangle, as a query shape.
///
/// Port of `Rectangle2D` (`core/geo/rectangle2d.rs`).
///
/// **Deviation:** the reference implementation's `create_from_rectangle` /
/// `create_from_xy_rectangle` envelope the quantized shape in a component tree
/// when the box crosses the dateline; that bridging lives in `:geo_rectangle`
/// (the shape module), so this module stays free of the shape classes.
export class Rectangle2D final : public Component2D {
public:
    Rectangle2D(double min_x, double max_x, double min_y, double max_y)
            : min_x_(min_x), max_x_(max_x), min_y_(min_y), max_y_(max_y) {}

    // ---- Component2D ----

    [[nodiscard]] double min_x() const override { return min_x_; }
    [[nodiscard]] double max_x() const override { return max_x_; }
    [[nodiscard]] double min_y() const override { return min_y_; }
    [[nodiscard]] double max_y() const override { return max_y_; }

    [[nodiscard]] bool contains(double x, double y) const override {
        return component2d_contains_point(x, y, min_x_, max_x_, min_y_, max_y_);
    }

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

    [[nodiscard]] bool equals(const Rectangle2D &other) const noexcept {
        return min_x_ == other.min_x_ && max_x_ == other.max_x_ && min_y_ == other.min_y_ && max_y_ == other.max_y_;
    }

    [[nodiscard]] std::string to_string() const;

private:
    /// Whether the edge `a`-`b` crosses one of the four rectangle edges.
    [[nodiscard]] bool edges_intersect(double a_x, double a_y, double b_x, double b_y) const;

    double min_x_{0};
    double max_x_{0};
    double min_y_{0};
    double max_y_{0};
};

} // namespace lucene
