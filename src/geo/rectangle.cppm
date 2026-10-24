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

export module lucene_core:geo_rectangle;

import :types;
import std;

import :error;
import :geo_component2d;
import :geo_geometry;

namespace lucene {

/// Represents a lat/lon rectangle: a box bounded by two latitudes and two
/// longitudes, in degrees.
///
/// Port of `Rectangle` (`core/geo/rectangle.rs`).
///
/// The box may cross the dateline, in which case `min_lon` is larger than
/// `max_lon`; [`to_component2d`] then splits it into two [`Rectangle2D`]s (one for
/// each side of the dateline) wrapped in a component tree.
///
/// **Deviation:** `from_polygon` is not ported, because the port has no polygon
/// shape yet.
export class Rectangle final : public LatLonGeometry {
public:
    /// A box with the given corners, after validating them.
    [[nodiscard]] static Result<Rectangle> create(double min_lat, double max_lat, double min_lon, double max_lon);

    [[nodiscard]] double min_lat() const noexcept { return min_lat_; }
    [[nodiscard]] double max_lat() const noexcept { return max_lat_; }
    [[nodiscard]] double min_lon() const noexcept { return min_lon_; }
    [[nodiscard]] double max_lon() const noexcept { return max_lon_; }

    /// `true` when the box spans the dateline (`max_lon < min_lon`).
    [[nodiscard]] bool crosses_dateline() const noexcept { return max_lon_ < min_lon_; }

    /// The bounding box of the circle with the given center and radius.
    ///
    /// Port of `Rectangle::from_point_distance` (`core/geo/rectangle.rs`).  A
    /// circle that swallows a pole gets a box that spans every longitude; one that
    /// reaches over the dateline gets a box whose minimum longitude is larger than
    /// its maximum.
    [[nodiscard]] static Result<Rectangle> from_point_distance(double center_lat, double center_lon,
                                                               double radius_meters);

    /// The latitude at which a circle meets the meridians of its bounding box.
    ///
    /// Port of `Rectangle::axis_lat` (`core/geo/rectangle.rs`).
    [[nodiscard]] static double axis_lat(double center_lat, double radius_meters);

    /// Whether the box contains the point `(lat, lon)`.
    [[nodiscard]] static bool contains_point(double lat, double lon, double min_lat, double max_lat, double min_lon,
                                             double max_lon);

    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const Rectangle &other) const noexcept;

private:
    Rectangle(double min_lat, double max_lat, double min_lon, double max_lon);

    double min_lat_;
    double max_lat_;
    double min_lon_;
    double max_lon_;
};

/// Represents a rectangle in the cartesian plane: the bounds are floats.
///
/// Port of `XYRectangle` (`core/geo/xy_rectangle.rs`).
export class XYRectangle final : public XYGeometry {
public:
    /// A box with the given corners, after validating them.
    [[nodiscard]] static Result<XYRectangle> create(float min_x, float max_x, float min_y, float max_y);

    /// The bounding box of the circle with the given center and radius.
    ///
    /// Port of `XYRectangle::from_point_distance`: the box is rounded up, so
    /// numerical errors cannot shrink it (LUCENE-9243).
    [[nodiscard]] static Result<XYRectangle> from_point_distance(float x, float y, float radius);

    [[nodiscard]] float min_x() const noexcept { return min_x_; }
    [[nodiscard]] float max_x() const noexcept { return max_x_; }
    [[nodiscard]] float min_y() const noexcept { return min_y_; }
    [[nodiscard]] float max_y() const noexcept { return max_y_; }

    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const XYRectangle &other) const noexcept;

private:
    XYRectangle(float min_x, float max_x, float min_y, float max_y);

    float min_x_;
    float max_x_;
    float min_y_;
    float max_y_;
};

} // namespace lucene
