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

module lucene_core:polygon2d.impl;

import :polygon2d;

import std;

import :edge_tree;
import :error;
import :geo_component2d;
import :point_values;
import :types;

namespace lucene {

Polygon2D::Polygon2D(double min_x, double max_x, double min_y, double max_y, EdgeTree tree,
                     std::shared_ptr<const Component2D> holes)
        : min_x_(min_x),
          max_x_(max_x),
          min_y_(min_y),
          max_y_(max_y),
          tree_(std::move(tree)),
          holes_(std::move(holes)) {}

Result<Polygon2D> Polygon2D::create(double min_x, double max_x, double min_y, double max_y,
                                    std::span<const double> xs, std::span<const double> ys,
                                    std::shared_ptr<const Component2D> holes) {
    auto tree = EdgeTree::create(xs, ys);
    if (!tree.has_value()) {
        return std::unexpected(std::move(tree.error()));
    }
    return Polygon2D(min_x, max_x, min_y, max_y, std::move(tree.value()), std::move(holes));
}

int32_t Polygon2D::number_of_corners(double min_x, double max_x, double min_y, double max_y) const {
    int32_t contains_count = 0;
    if (contains(min_x, min_y)) {
        contains_count += 1;
    }
    if (contains(max_x, min_y)) {
        contains_count += 1;
    }
    if (contains_count == 1) {
        return contains_count;
    }
    if (contains(max_x, max_y)) {
        contains_count += 1;
    }
    if (contains_count == 2) {
        return contains_count;
    }
    if (contains(min_x, max_y)) {
        contains_count += 1;
    }
    return contains_count;
}

bool Polygon2D::contains(double x, double y) const {
    if (component2d_contains_point(x, y, min_x_, max_x_, min_y_, max_y_) && tree_.contains(x, y)) {
        return holes_ == nullptr || !holes_->contains(x, y);
    }
    return false;
}

Result<Relation> Polygon2D::relate(double min_x, double max_x, double min_y, double max_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return Relation::kCellOutsideQuery;
    }
    if (component2d_within(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return Relation::kCellCrossesQuery;
    }

    if (holes_ != nullptr) {
        auto hole_relation = holes_->relate(min_x, max_x, min_y, max_y);
        if (!hole_relation.has_value()) {
            return std::unexpected(std::move(hole_relation.error()));
        }
        if (hole_relation.value() == Relation::kCellCrossesQuery) {
            return Relation::kCellCrossesQuery;
        }
        if (hole_relation.value() == Relation::kCellInsideQuery) {
            return Relation::kCellOutsideQuery;
        }
    }

    // Ask the corners first: if some are inside and some are not, the cell crosses
    // the polygon and the edge tree does not have to be walked at all.
    const int32_t num_corners = number_of_corners(min_x, max_x, min_y, max_y);
    if (num_corners == 4) {
        if (tree_.crosses_box(min_x, max_x, min_y, max_y, true)) {
            return Relation::kCellCrossesQuery;
        }
        return Relation::kCellInsideQuery;
    }
    if (num_corners == 0) {
        if (component2d_contains_point(tree_.x1(), tree_.y1(), min_x, max_x, min_y, max_y)) {
            return Relation::kCellCrossesQuery;
        }
        if (tree_.crosses_box(min_x, max_x, min_y, max_y, true)) {
            return Relation::kCellCrossesQuery;
        }
        return Relation::kCellOutsideQuery;
    }
    return Relation::kCellCrossesQuery;
}

bool Polygon2D::intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                double b_x, double b_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return false;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y) || tree_.crosses_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, true)) {
        return holes_ == nullptr || !holes_->contains_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y);
    }
    return false;
}

bool Polygon2D::intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                    double b_x, double b_y, double c_x, double c_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return false;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y) || contains(c_x, c_y) ||
        component2d_point_in_triangle(min_x, max_x, min_y, max_y, tree_.x1(), tree_.y1(), a_x, a_y, b_x, b_y, c_x,
                                      c_y) ||
        tree_.crosses_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y, true)) {
        return holes_ == nullptr || !holes_->contains_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y);
    }
    return false;
}

bool Polygon2D::contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                              double b_x, double b_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return false;
    }
    if (contains(a_x, a_y) && contains(b_x, b_y) &&
        !tree_.crosses_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, false)) {
        return holes_ == nullptr || !holes_->intersects_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y);
    }
    return false;
}

bool Polygon2D::contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                  double b_x, double b_y, double c_x, double c_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return false;
    }
    if (contains(a_x, a_y) && contains(b_x, b_y) && contains(c_x, c_y) &&
        !tree_.crosses_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y, false)) {
        return holes_ == nullptr || !holes_->intersects_triangle(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, c_x, c_y);
    }
    return false;
}

Result<WithinRelation> Polygon2D::within_point(double x, double y) const {
    return contains(x, y) ? WithinRelation::NotWithin : WithinRelation::Disjoint;
}

Result<WithinRelation> Polygon2D::within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                              double a_y, bool ab, double b_x, double b_y) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return WithinRelation::Disjoint;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y)) {
        return WithinRelation::NotWithin;
    }
    if (ab && tree_.crosses_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, true)) {
        return WithinRelation::NotWithin;
    }
    return WithinRelation::Disjoint;
}

Result<WithinRelation> Polygon2D::within_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                  double a_y, bool ab, double b_x, double b_y, bool bc, double c_x,
                                                  double c_y, bool ca) const {
    if (component2d_disjoint(min_x_, max_x_, min_y_, max_y_, min_x, max_x, min_y, max_y)) {
        return WithinRelation::Disjoint;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y) || contains(c_x, c_y)) {
        return WithinRelation::NotWithin;
    }

    WithinRelation relation = WithinRelation::Disjoint;

    if (tree_.crosses_line(min_x, max_x, min_y, max_y, a_x, a_y, b_x, b_y, true)) {
        if (ab) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }
    if (tree_.crosses_line(min_x, max_x, min_y, max_y, b_x, b_y, c_x, c_y, true)) {
        if (bc) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }
    if (tree_.crosses_line(min_x, max_x, min_y, max_y, c_x, c_y, a_x, a_y, true)) {
        if (ca) {
            return WithinRelation::NotWithin;
        }
        relation = WithinRelation::Candidate;
    }

    if (relation == WithinRelation::Candidate) {
        return WithinRelation::Candidate;
    }
    if (component2d_point_in_triangle(min_x, max_x, min_y, max_y, tree_.x1(), tree_.y1(), a_x, a_y, b_x, b_y, c_x,
                                      c_y)) {
        return WithinRelation::Candidate;
    }
    return relation;
}

} // namespace lucene
