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

export module lucene_core:geo_point;

import :types;
import std;

import :error;
import :geo_component2d;
import :geo_geometry;

namespace lucene {

/// Represents a point on the earth's surface, in decimal degrees.
///
/// Port of `Point` (`core/geo/point.rs`).
///
/// [`to_component2d`] quantizes the coordinates the way the point index stores
/// them, so a query point and an indexed point agree on where they are.
export class Point final : public LatLonGeometry {
public:
    /// A point at `(lat, lon)`, after validating both.
    [[nodiscard]] static Result<Point> create(double lat, double lon);

    [[nodiscard]] double lat() const noexcept { return lat_; }
    [[nodiscard]] double lon() const noexcept { return lon_; }

    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const Point &other) const noexcept;

private:
    Point(double lat, double lon);

    double lat_;
    double lon_;
};

/// Represents a point in the cartesian plane: the coordinates are floats.
///
/// Port of `XYPoint` (`core/geo/xy_point.rs`).
export class XYPoint final : public XYGeometry {
public:
    /// A point at `(x, y)`, after validating both.
    [[nodiscard]] static Result<XYPoint> create(float x, float y);

    [[nodiscard]] float x() const noexcept { return x_; }
    [[nodiscard]] float y() const noexcept { return y_; }

    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const XYPoint &other) const noexcept;

private:
    XYPoint(float x, float y);

    float x_;
    float y_;
};

} // namespace lucene
