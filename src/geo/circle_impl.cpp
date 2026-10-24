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

module lucene_core:geo_circle.impl;

import :geo_circle;

import std;

import :error;
import :geo_component2d;
import :geo_rectangle;
import :geo_utils;
import :point_values;
import :sloppy_math;
import :types;

namespace lucene {

namespace {

/// The latitude and longitude bounds of the circle, in degrees.
///
/// The values come from [`Rectangle::from_point_distance`], which is where the
/// algorithm lives; the circle only needs the four numbers back.
struct LatLonBox {
    double min_lat{0.0};
    double max_lat{0.0};
    double min_lon{0.0};
    double max_lon{0.0};
};

/// Whether the segment `a`-`b` reaches into the circle.
///
/// Port of the free `intersects_line` (`core/geo/circle2d.rs`): project the
/// center onto the segment and ask the calculator about the closest point.
[[nodiscard]] bool segment_intersects(double center_x, double center_y, double a_x, double a_y, double b_x,
                                      double b_y, const DistanceCalculator &calculator) {
    const double vector_apx = center_x - a_x;
    const double vector_apy = center_y - a_y;

    const double vector_abx = b_x - a_x;
    const double vector_aby = b_y - a_y;

    const double magnitude_ab = vector_abx * vector_abx + vector_aby * vector_aby;
    const double dot_product = vector_apx * vector_abx + vector_apy * vector_aby;

    const double distance = dot_product / magnitude_ab;
    if (distance < 0.0 || distance > 1.0) {
        return false;
    }

    const double p_x = a_x + vector_abx * distance;
    const double p_y = a_y + vector_aby * distance;

    const double min_x = std::min(a_x, b_x);
    const double min_y = std::min(a_y, b_y);
    const double max_x = std::max(a_x, b_x);
    const double max_y = std::max(a_y, b_y);

    if (p_x >= min_x && p_x <= max_x && p_y >= min_y && p_y <= max_y) {
        return calculator.contains(p_x, p_y);
    }
    return false;
}

/// The bounding box of a cartesian circle, in floats.
///
/// Port of `XYRectangle::from_point_distance` (`core/geo/xy_rectangle.rs`).
struct XYBox {
    float min_x{0.0F};
    float max_x{0.0F};
    float min_y{0.0F};
    float max_y{0.0F};
};

[[nodiscard]] Result<float> check_finite_float(float value) {
    if (!std::isfinite(value)) {
        return err(LuceneError::illegal_argument("invalid value " + std::to_string(value) + ": must be finite"));
    }
    return value;
}

[[nodiscard]] Result<XYBox> xy_box_from_point_distance(float x, float y, float radius) {
    if (auto checked = check_finite_float(x); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = check_finite_float(y); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (radius < 0.0F || !std::isfinite(radius)) {
        return err(LuceneError::illegal_argument("radius must be finite and not negative, got " +
                                                 std::to_string(radius)));
    }

    // LUCENE-9243: round the box up, so numerical errors cannot shrink it.
    const float distance_box = std::nextafter(radius, std::numeric_limits<float>::infinity());
    constexpr float kFloatMax = std::numeric_limits<float>::max();

    XYBox box;
    box.min_x = std::max(-kFloatMax, x - distance_box);
    box.max_x = std::min(kFloatMax, x + distance_box);
    box.min_y = std::max(-kFloatMax, y - distance_box);
    box.max_y = std::min(kFloatMax, y + distance_box);
    return box;
}

/// A circle in the cartesian plane.
///
/// Port of `CartesianDistance` (`core/geo/circle2d.rs`).
class CartesianDistance final : public DistanceCalculator {
public:
    [[nodiscard]] static Result<std::unique_ptr<CartesianDistance>> create(double center_x, double center_y,
                                                                          double radius) {
        const auto as_float_x = static_cast<float>(center_x);
        const auto as_float_y = static_cast<float>(center_y);
        const auto as_float_radius = static_cast<float>(radius);
        auto box = xy_box_from_point_distance(as_float_x, as_float_y, as_float_radius);
        if (!box.has_value()) {
            return std::unexpected(std::move(box.error()));
        }
        return std::unique_ptr<CartesianDistance>(new CartesianDistance(
                static_cast<double>(as_float_x), static_cast<double>(as_float_y),
                static_cast<double>(as_float_radius) * static_cast<double>(as_float_radius), box.value()));
    }

    [[nodiscard]] bool contains(double x, double y) const override {
        if (component2d_contains_point(x, y, static_cast<double>(box_.min_x), static_cast<double>(box_.max_x),
                                       static_cast<double>(box_.min_y), static_cast<double>(box_.max_y))) {
            const double diff_x = x - center_x_;
            const double diff_y = y - center_y_;
            return diff_x * diff_x + diff_y * diff_y <= radius_squared_;
        }
        return false;
    }

    [[nodiscard]] bool intersects_line(double a_x, double a_y, double b_x, double b_y) const override {
        return segment_intersects(center_x_, center_y_, a_x, a_y, b_x, b_y, *this);
    }

    [[nodiscard]] Result<Relation> relate(double min_x, double max_x, double min_y, double max_y) const override {
        if (component2d_contains_point(center_x_, center_y_, min_x, max_x, min_y, max_y)) {
            if (contains(min_x, min_y) && contains(max_x, min_y) && contains(max_x, max_y) &&
                contains(min_x, max_y)) {
                // Fully enclosed: everything in this cell matches.
                return Relation::kCellInsideQuery;
            }
        } else {
            double sum_of_squared_diffs = 0.0;
            if (center_x_ < min_x) {
                const double diff = min_x - center_x_;
                sum_of_squared_diffs += diff * diff;
            } else if (center_x_ > max_x) {
                const double diff = max_x - center_x_;
                sum_of_squared_diffs += diff * diff;
            }

            if (center_y_ < min_y) {
                const double diff = min_y - center_y_;
                sum_of_squared_diffs += diff * diff;
            } else if (center_y_ > max_y) {
                const double diff = max_y - center_y_;
                sum_of_squared_diffs += diff * diff;
            }

            if (sum_of_squared_diffs > radius_squared_) {
                return Relation::kCellOutsideQuery;
            }
        }
        return Relation::kCellCrossesQuery;
    }

    [[nodiscard]] bool disjoint(double min_x, double max_x, double min_y, double max_y) const override {
        return component2d_disjoint(static_cast<double>(box_.min_x), static_cast<double>(box_.max_x),
                                    static_cast<double>(box_.min_y), static_cast<double>(box_.max_y), min_x, max_x,
                                    min_y, max_y);
    }

    [[nodiscard]] bool within(double min_x, double max_x, double min_y, double max_y) const override {
        return component2d_within(static_cast<double>(box_.min_x), static_cast<double>(box_.max_x),
                                  static_cast<double>(box_.min_y), static_cast<double>(box_.max_y), min_x, max_x,
                                  min_y, max_y);
    }

    [[nodiscard]] double min_x() const override { return static_cast<double>(box_.min_x); }
    [[nodiscard]] double max_x() const override { return static_cast<double>(box_.max_x); }
    [[nodiscard]] double min_y() const override { return static_cast<double>(box_.min_y); }
    [[nodiscard]] double max_y() const override { return static_cast<double>(box_.max_y); }
    [[nodiscard]] double x() const override { return center_x_; }
    [[nodiscard]] double y() const override { return center_y_; }

private:
    CartesianDistance(double center_x, double center_y, double radius_squared, XYBox box)
            : box_(box), center_x_(center_x), center_y_(center_y), radius_squared_(radius_squared) {}

    XYBox box_;
    double center_x_;
    double center_y_;
    double radius_squared_;
};

/// A circle on the earth's surface, measured with the haversine distance.
///
/// Port of `HaversinDistance` (`core/geo/circle2d.rs`).
class HaversinDistance final : public DistanceCalculator {
public:
    [[nodiscard]] static Result<std::unique_ptr<HaversinDistance>> create(double center_lat, double center_lon,
                                                                         double radius) {
        auto box = Rectangle::from_point_distance(center_lat, center_lon, radius);
        if (!box.has_value()) {
            return std::unexpected(std::move(box.error()));
        }
        std::unique_ptr<HaversinDistance> distance(new HaversinDistance());
        distance->center_lat_ = center_lat;
        distance->center_lon_ = center_lon;
        distance->sort_key_ = GeoUtils::distance_query_sort_key(radius);
        distance->axis_lat_ = Rectangle::axis_lat(center_lat, radius);
        distance->box_ = LatLonBox{box.value().min_lat(), box.value().max_lat(), box.value().min_lon(),
                                   box.value().max_lon()};
        distance->crosses_dateline_ = box.value().crosses_dateline();
        return distance;
    }

    [[nodiscard]] bool contains(double x, double y) const override {
        if (crosses_dateline_) {
            if (component2d_contains_point(x, y, box_.min_lon, GeoUtils::MAX_LON_INCL, box_.min_lat, box_.max_lat) ||
                component2d_contains_point(x, y, GeoUtils::MIN_LON_INCL, box_.max_lon, box_.min_lat, box_.max_lat)) {
                return SloppyMath::haversin_sort_key(y, x, center_lat_, center_lon_) <= sort_key_;
            }
        } else if (component2d_contains_point(x, y, box_.min_lon, box_.max_lon, box_.min_lat, box_.max_lat)) {
            return SloppyMath::haversin_sort_key(y, x, center_lat_, center_lon_) <= sort_key_;
        }
        return false;
    }

    [[nodiscard]] bool intersects_line(double a_x, double a_y, double b_x, double b_y) const override {
        if (segment_intersects(center_lon_, center_lat_, a_x, a_y, b_x, b_y, *this)) {
            return true;
        }
        if (crosses_dateline_) {
            // The shape is also the copy of the circle shifted by one turn of the
            // globe, so a segment near the dateline may only hit that copy.
            const double shifted_center_lon = center_lon_ > 0.0 ? center_lon_ - 360.0 : center_lon_ + 360.0;
            return segment_intersects(shifted_center_lon, center_lat_, a_x, a_y, b_x, b_y, *this);
        }
        return false;
    }

    [[nodiscard]] Result<Relation> relate(double min_x, double max_x, double min_y, double max_y) const override {
        return GeoUtils::relate(min_y, max_y, min_x, max_x, center_lat_, center_lon_, sort_key_, axis_lat_);
    }

    [[nodiscard]] bool disjoint(double min_x, double max_x, double min_y, double max_y) const override {
        if (crosses_dateline_) {
            return component2d_disjoint(box_.min_lon, GeoUtils::MAX_LON_INCL, box_.min_lat, box_.max_lat, min_x, max_x,
                                        min_y, max_y) &&
                   component2d_disjoint(GeoUtils::MIN_LON_INCL, box_.max_lon, box_.min_lat, box_.max_lat, min_x,
                                        max_x, min_y, max_y);
        }
        return component2d_disjoint(box_.min_lon, box_.max_lon, box_.min_lat, box_.max_lat, min_x, max_x, min_y,
                                    max_y);
    }

    [[nodiscard]] bool within(double min_x, double max_x, double min_y, double max_y) const override {
        if (crosses_dateline_) {
            return component2d_within(box_.min_lon, GeoUtils::MAX_LON_INCL, box_.min_lat, box_.max_lat, min_x, max_x,
                                      min_y, max_y) ||
                   component2d_within(GeoUtils::MIN_LON_INCL, box_.max_lon, box_.min_lat, box_.max_lat, min_x,
                                      max_x, min_y, max_y);
        }
        return component2d_within(box_.min_lon, box_.max_lon, box_.min_lat, box_.max_lat, min_x, max_x, min_y, max_y);
    }

    [[nodiscard]] double min_x() const override {
        return crosses_dateline_ ? GeoUtils::MIN_LON_INCL : box_.min_lon;
    }

    [[nodiscard]] double max_x() const override {
        return crosses_dateline_ ? GeoUtils::MAX_LON_INCL : box_.max_lon;
    }

    [[nodiscard]] double min_y() const override { return box_.min_lat; }
    [[nodiscard]] double max_y() const override { return box_.max_lat; }
    [[nodiscard]] double x() const override { return center_lon_; }
    [[nodiscard]] double y() const override { return center_lat_; }

private:
    HaversinDistance() = default;

    LatLonBox box_{};
    double center_lat_{0.0};
    double center_lon_{0.0};
    double sort_key_{0.0};
    double axis_lat_{0.0};
    bool crosses_dateline_{false};
};

} // namespace

// ---- Circle ----

Circle::Circle(double lat, double lon, double radius_meters) : lat_(lat), lon_(lon), radius_(radius_meters) {}

Result<Circle> Circle::create(double lat, double lon, double radius_meters) {
    if (auto checked = GeoUtils::check_latitude(lat); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(lon); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (!std::isfinite(radius_meters) || radius_meters < 0.0) {
        return err(LuceneError::illegal_argument("radiusMeters: '" + std::to_string(radius_meters) + "' is invalid"));
    }
    return Circle(lat, lon, radius_meters);
}

Result<std::shared_ptr<Component2D>> Circle::to_component2d() const {
    auto component = Circle2D::from_circle(*this);
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }
    return std::shared_ptr<Component2D>(std::move(component.value()));
}

std::string Circle::to_string() const {
    return "Circle([" + std::format("{}", lat_) + "," + std::format("{}", lon_) + "] radius = " +
           std::format("{}", radius_) + " meters)";
}

bool Circle::equals(const Circle &other) const noexcept {
    return lat_ == other.lat_ && lon_ == other.lon_ && radius_ == other.radius_;
}

// ---- XYCircle ----

XYCircle::XYCircle(float x, float y, float radius) : x_(x), y_(y), radius_(radius) {}

Result<XYCircle> XYCircle::create(float x, float y, float radius) {
    if (radius <= 0.0F) {
        return err(LuceneError::illegal_argument("radius must be bigger than 0, got " + std::to_string(radius)));
    }
    if (!std::isfinite(radius)) {
        return err(LuceneError::illegal_argument("radius must be finite, got " + std::to_string(radius)));
    }
    if (auto checked = check_finite_float(x); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = check_finite_float(y); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return XYCircle(x, y, radius);
}

Result<std::shared_ptr<Component2D>> XYCircle::to_component2d() const {
    auto component = Circle2D::from_xy_circle(*this);
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }
    return std::shared_ptr<Component2D>(std::move(component.value()));
}

std::string XYCircle::to_string() const {
    return "XYCircle([" + std::format("{}", x_) + "," + std::format("{}", y_) + "] radius = " +
           std::format("{}", radius_) + ")";
}

bool XYCircle::equals(const XYCircle &other) const noexcept {
    return x_ == other.x_ && y_ == other.y_ && radius_ == other.radius_;
}

// ---- Circle2D ----

Circle2D::Circle2D(std::unique_ptr<const DistanceCalculator> calculator) : calculator_(std::move(calculator)) {}

Result<std::unique_ptr<Circle2D>> Circle2D::from_circle(const Circle &circle) {
    auto calculator = HaversinDistance::create(circle.lat(), circle.lon(), circle.radius());
    if (!calculator.has_value()) {
        return std::unexpected(std::move(calculator.error()));
    }
    return std::unique_ptr<Circle2D>(
            new Circle2D(std::unique_ptr<const DistanceCalculator>(std::move(calculator.value()))));
}

Result<std::unique_ptr<Circle2D>> Circle2D::from_xy_circle(const XYCircle &circle) {
    auto calculator = CartesianDistance::create(static_cast<double>(circle.x()), static_cast<double>(circle.y()),
                                                static_cast<double>(circle.radius()));
    if (!calculator.has_value()) {
        return std::unexpected(std::move(calculator.error()));
    }
    return std::unique_ptr<Circle2D>(
            new Circle2D(std::unique_ptr<const DistanceCalculator>(std::move(calculator.value()))));
}

double Circle2D::min_x() const { return calculator_->min_x(); }
double Circle2D::max_x() const { return calculator_->max_x(); }
double Circle2D::min_y() const { return calculator_->min_y(); }
double Circle2D::max_y() const { return calculator_->max_y(); }

bool Circle2D::contains(double x, double y) const { return calculator_->contains(x, y); }

Result<Relation> Circle2D::relate(double min_x, double max_x, double min_y, double max_y) const {
    if (calculator_->disjoint(min_x, max_x, min_y, max_y)) {
        return Relation::kCellOutsideQuery;
    }
    if (calculator_->within(min_x, max_x, min_y, max_y)) {
        return Relation::kCellCrossesQuery;
    }
    return calculator_->relate(min_x, max_x, min_y, max_y);
}

bool Circle2D::intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                               double b_x, double b_y) const {
    if (calculator_->disjoint(min_x, max_x, min_y, max_y)) {
        return false;
    }
    return contains(a_x, a_y) || contains(b_x, b_y) || calculator_->intersects_line(a_x, a_y, b_x, b_y);
}

bool Circle2D::intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                   double b_x, double b_y, double c_x, double c_y) const {
    if (calculator_->disjoint(min_x, max_x, min_y, max_y)) {
        return false;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y) || contains(c_x, c_y)) {
        return true;
    }
    if (component2d_point_in_triangle(min_x, max_x, min_y, max_y, calculator_->x(), calculator_->y(), a_x, a_y, b_x,
                                      b_y, c_x, c_y)) {
        return true;
    }
    return calculator_->intersects_line(a_x, a_y, b_x, b_y) || calculator_->intersects_line(b_x, b_y, c_x, c_y) ||
           calculator_->intersects_line(c_x, c_y, a_x, a_y);
}

bool Circle2D::contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                             double b_x, double b_y) const {
    if (calculator_->disjoint(min_x, max_x, min_y, max_y)) {
        return false;
    }
    return contains(a_x, a_y) && contains(b_x, b_y);
}

bool Circle2D::contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                 double b_x, double b_y, double c_x, double c_y) const {
    if (calculator_->disjoint(min_x, max_x, min_y, max_y)) {
        return false;
    }
    return contains(a_x, a_y) && contains(b_x, b_y) && contains(c_x, c_y);
}

Result<WithinRelation> Circle2D::within_point(double x, double y) const {
    return contains(x, y) ? WithinRelation::NotWithin : WithinRelation::Disjoint;
}

Result<WithinRelation> Circle2D::within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                             double a_y, bool ab, double b_x, double b_y) const {
    if (calculator_->disjoint(min_x, max_x, min_y, max_y)) {
        return WithinRelation::Disjoint;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y)) {
        return WithinRelation::NotWithin;
    }
    if (ab && calculator_->intersects_line(a_x, a_y, b_x, b_y)) {
        return WithinRelation::NotWithin;
    }
    return WithinRelation::Disjoint;
}

Result<WithinRelation> Circle2D::within_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                 double a_y, bool ab, double b_x, double b_y, bool bc, double c_x,
                                                 double c_y, bool ca) const {
    if (calculator_->disjoint(min_x, max_x, min_y, max_y)) {
        return WithinRelation::Disjoint;
    }
    if (contains(a_x, a_y) || contains(b_x, b_y) || contains(c_x, c_y)) {
        return WithinRelation::NotWithin;
    }
    if (ab && calculator_->intersects_line(a_x, a_y, b_x, b_y)) {
        return WithinRelation::NotWithin;
    }
    if (bc && calculator_->intersects_line(b_x, b_y, c_x, c_y)) {
        return WithinRelation::NotWithin;
    }
    if (ca && calculator_->intersects_line(c_x, c_y, a_x, a_y)) {
        return WithinRelation::NotWithin;
    }
    if (component2d_point_in_triangle(min_x, max_x, min_y, max_y, calculator_->x(), calculator_->y(), a_x, a_y, b_x,
                                      b_y, c_x, c_y)) {
        return WithinRelation::Candidate;
    }
    return WithinRelation::Disjoint;
}

} // namespace lucene
