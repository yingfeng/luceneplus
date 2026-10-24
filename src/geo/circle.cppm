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

export module lucene_core:geo_circle;

import :types;
import std;

import :error;
import :geo_component2d;
import :geo_geometry;
import :point_values;

namespace lucene {

/// The distance a [`Circle2D`] measures: the cartesian one for x/y shapes and the
/// haversine one for lat/lon shapes.
///
/// Port of the `DistanceCalculator` trait (`core/geo/circle2d.rs`).
export class DistanceCalculator {
public:
    virtual ~DistanceCalculator() = default;

    /// Whether the point is within the circle.
    [[nodiscard]] virtual bool contains(double x, double y) const = 0;

    /// Whether the segment `a`-`b` comes closer to the center than the radius.
    [[nodiscard]] virtual bool intersects_line(double a_x, double a_y, double b_x, double b_y) const = 0;

    /// How the cell relates to the circle.
    [[nodiscard]] virtual Result<Relation> relate(double min_x, double max_x, double min_y,
                                                 double max_y) const = 0;

    /// Whether the cell is outside the circle's bounding box.
    [[nodiscard]] virtual bool disjoint(double min_x, double max_x, double min_y, double max_y) const = 0;

    /// Whether the cell contains the circle's bounding box.
    [[nodiscard]] virtual bool within(double min_x, double max_x, double min_y, double max_y) const = 0;

    [[nodiscard]] virtual double min_x() const = 0;
    [[nodiscard]] virtual double max_x() const = 0;
    [[nodiscard]] virtual double min_y() const = 0;
    [[nodiscard]] virtual double max_y() const = 0;
    [[nodiscard]] virtual double x() const = 0;
    [[nodiscard]] virtual double y() const = 0;
};

/// Represents a circle on the earth's surface.
///
/// Port of `Circle` (`core/geo/circle.rs`).
///
/// Latitude/longitude are decimal degrees, the radius is meters.  The shape is
/// turned into a [`Circle2D`] with haversine distance by
/// [`Circle2D::from_circle`].
export class Circle final : public LatLonGeometry {
public:
    /// A circle with the given center and radius, after validating all three.
    [[nodiscard]] static Result<Circle> create(double lat, double lon, double radius_meters);

    [[nodiscard]] double lat() const noexcept { return lat_; }
    [[nodiscard]] double lon() const noexcept { return lon_; }
    [[nodiscard]] double radius() const noexcept { return radius_; }

    /// This shape as a [`Component2D`], for the point-index machinery.
    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const Circle &other) const noexcept;

private:
    Circle(double lat, double lon, double radius_meters);

    double lat_;
    double lon_;
    double radius_;
};

/// Represents a circle in a cartesian plane (`x`/`y` and the radius are floats).
///
/// Port of `XYCircle` (`core/geo/xy_circle.rs`).
export class XYCircle final : public XYGeometry {
public:
    /// A circle with the given center and radius, after validating all three.
    [[nodiscard]] static Result<XYCircle> create(float x, float y, float radius);

    [[nodiscard]] float x() const noexcept { return x_; }
    [[nodiscard]] float y() const noexcept { return y_; }
    [[nodiscard]] float radius() const noexcept { return radius_; }

    /// This shape as a [`Component2D`], for the point-index machinery.
    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const XYCircle &other) const noexcept;

private:
    XYCircle(float x, float y, float radius);

    float x_;
    float y_;
    float radius_;
};

/// A circle as a [`Component2D`].
///
/// Port of `Circle2D<T>` (`core/geo/circle2d.rs`).  The reference implementation
/// is generic over the distance calculator; the port stores the calculator behind
/// a pointer, so one concrete type covers both the cartesian and the lat/lon
/// circle.
///
/// The lat/lon flavour is the interesting one: the circle is not convex in lat/lon
/// space, so the implementation first checks the circle's bounding box and then
/// compares the haversine distance.  A circle that swallows one of the poles has a
/// bounding box that spans every longitude, and a circle that touches the dateline
/// has one whose minimum longitude is larger than its maximum.
export class Circle2D final : public Component2D {
public:
    /// A circle over lat/lon, measured with the haversine distance.
    [[nodiscard]] static Result<std::unique_ptr<Circle2D>> from_circle(const Circle &circle);

    /// A circle in the cartesian plane, measured with the euclidean distance.
    [[nodiscard]] static Result<std::unique_ptr<Circle2D>> from_xy_circle(const XYCircle &circle);

    // ---- Component2D ----

    [[nodiscard]] double min_x() const override;
    [[nodiscard]] double max_x() const override;
    [[nodiscard]] double min_y() const override;
    [[nodiscard]] double max_y() const override;

    [[nodiscard]] bool contains(double x, double y) const override;

    [[nodiscard]] Result<Relation> relate(double min_x, double max_x, double min_y, double max_y) const override;

    [[nodiscard]] bool intersects_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                       double a_y, double b_x, double b_y) const override;

    [[nodiscard]] bool intersects_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                           double a_y, double b_x, double b_y, double c_x,
                                           double c_y) const override;

    [[nodiscard]] bool contains_line(double min_x, double max_x, double min_y, double max_y, double a_x, double a_y,
                                     double b_x, double b_y) const override;

    [[nodiscard]] bool contains_triangle(double min_x, double max_x, double min_y, double max_y, double a_x,
                                         double a_y, double b_x, double b_y, double c_x,
                                         double c_y) const override;

    [[nodiscard]] Result<WithinRelation> within_point(double x, double y) const override;

    [[nodiscard]] Result<WithinRelation> within_line(double min_x, double max_x, double min_y, double max_y, double a_x,
                                                     double a_y, bool ab, double b_x, double b_y) const override;

    [[nodiscard]] Result<WithinRelation> within_triangle(double min_x, double max_x, double min_y, double max_y,
                                                         double a_x, double a_y, bool ab, double b_x, double b_y,
                                                         bool bc, double c_x, double c_y, bool ca) const override;

private:
    explicit Circle2D(std::unique_ptr<const DistanceCalculator> calculator);

    std::unique_ptr<const DistanceCalculator> calculator_;
};

} // namespace lucene
