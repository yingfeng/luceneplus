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

export module lucene_core:polygon2d;

import :types;
import std;

import :edge_tree;
import :error;
import :geo_component2d;
import :point_values;

namespace lucene {

/// A polygon, as a query shape: the edges of the shell live in an
/// [`EdgeTree`], the holes in a component of their own.
///
/// Port of `Polygon2D` (`core/geo/polygon2d.rs`).  Loosely based on the
/// algorithm described in <http://www-ma2.upc.es/geoc/Schirra-pointPolygon.pdf>:
/// the cell corners decide whether the cell is inside or outside, and only the
/// ambiguous cases walk the edge tree.
///
/// **Deviation:** the reference implementation keeps the holes in a
/// `HolesEnum` that selects the lat/lon or the XY flavour; the port stores them as
/// a [`Component2D`], which is what both flavours degenerate to.
export class Polygon2D final : public Component2D {
public:
    /// A polygon over the vertices `(xs[i], ys[i])`; `holes` may be null when the
    /// polygon has none.  At least four vertices are required, the last one
    /// repeating the first.
    [[nodiscard]] static Result<Polygon2D> create(double min_x, double max_x, double min_y, double max_y,
                                                  std::span<const double> xs, std::span<const double> ys,
                                                  std::shared_ptr<const Component2D> holes);

    [[nodiscard]] const EdgeTree &tree() const noexcept { return tree_; }
    [[nodiscard]] const std::shared_ptr<const Component2D> &holes() const noexcept { return holes_; }

    // ---- Component2D ----

    [[nodiscard]] double min_x() const override { return min_x_; }
    [[nodiscard]] double max_x() const override { return max_x_; }
    [[nodiscard]] double min_y() const override { return min_y_; }
    [[nodiscard]] double max_y() const override { return max_y_; }

    [[nodiscard]] bool contains(double x, double y) const override;

    [[nodiscard]] Result<Relation> relate(double min_x, double max_x, double min_y, double max_y) const override;

    [[nodiscard]] bool intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                       double b_x, double b_y) const override;

    [[nodiscard]] bool intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                           double a_y, double b_x, double b_y, double c_x, double c_y) const override;

    [[nodiscard]] bool contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                     double b_x, double b_y) const override;

    [[nodiscard]] bool contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                         double a_y, double b_x, double b_y, double c_x, double c_y) const override;

    [[nodiscard]] Result<WithinRelation> within_point(double x, double y) const override;

    [[nodiscard]] Result<WithinRelation> within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                     double a_y, bool ab, double b_x, double b_y) const override;

    [[nodiscard]] Result<WithinRelation> within_triangle(double min_x, double max_x, double min_y, double max_y,
                                                         double a_x, double a_y, bool ab, double b_x, double b_y,
                                                         bool bc, double c_x, double c_y, bool ca) const override;

private:
    Polygon2D(double min_x, double max_x, double min_y, double max_y, EdgeTree tree,
              std::shared_ptr<const Component2D> holes);

    /// How many corners of the cell lie inside the shell (stops early, so it is
    /// either `0`, `1`, `2`, or `4`).
    [[nodiscard]] int32_t number_of_corners(double min_x, double max_x, double min_y, double max_y) const;

    double min_x_;
    double max_x_;
    double min_y_;
    double max_y_;
    EdgeTree tree_;
    std::shared_ptr<const Component2D> holes_;
};

} // namespace lucene
