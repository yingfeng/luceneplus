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

export module lucene_core:geo_line;

import :types;
import std;

import :error;
import :geo_component2d;
import :geo_geometry;

namespace lucene {

/// Represents a polyline on the earth's surface, in decimal degrees.
///
/// Port of `Line` (`core/geo/line.rs`).
///
/// The shape keeps the bounding box it computed, which is what the point-index
/// machinery prunes with before it looks at the individual edges.
export class Line final : public LatLonGeometry {
public:
    /// A line through the given vertices; the two arrays must have the same
    /// length and at least two points are required.
    [[nodiscard]] static Result<Line> create(std::vector<double> lats, std::vector<double> lons);

    /// Number of vertices.
    [[nodiscard]] size_t num_points() const noexcept { return lats_.size(); }
    [[nodiscard]] double lat(size_t vertex) const;
    [[nodiscard]] double lon(size_t vertex) const;
    [[nodiscard]] const std::vector<double> &lats() const noexcept { return lats_; }
    [[nodiscard]] const std::vector<double> &lons() const noexcept { return lons_; }

    [[nodiscard]] double min_lat() const noexcept { return min_lat_; }
    [[nodiscard]] double max_lat() const noexcept { return max_lat_; }
    [[nodiscard]] double min_lon() const noexcept { return min_lon_; }
    [[nodiscard]] double max_lon() const noexcept { return max_lon_; }

    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const Line &other) const noexcept;

private:
    Line(std::vector<double> lats, std::vector<double> lons, double min_lat, double max_lat, double min_lon,
         double max_lon);

    std::vector<double> lats_;
    std::vector<double> lons_;
    double min_lat_;
    double max_lat_;
    double min_lon_;
    double max_lon_;
};

/// Represents a polyline in the cartesian plane; the vertices are floats.
///
/// Port of `XYLine` (`core/geo/xy_line.rs`).
export class XYLine final : public XYGeometry {
public:
    /// A line through the given vertices; the two arrays must have the same
    /// length and at least two points are required.
    [[nodiscard]] static Result<XYLine> create(std::vector<float> xs, std::vector<float> ys);

    [[nodiscard]] size_t num_points() const noexcept { return xs_.size(); }
    [[nodiscard]] float x(size_t vertex) const;
    [[nodiscard]] float y(size_t vertex) const;
    [[nodiscard]] const std::vector<float> &xs() const noexcept { return xs_; }
    [[nodiscard]] const std::vector<float> &ys() const noexcept { return ys_; }

    [[nodiscard]] float min_x() const noexcept { return min_x_; }
    [[nodiscard]] float max_x() const noexcept { return max_x_; }
    [[nodiscard]] float min_y() const noexcept { return min_y_; }
    [[nodiscard]] float max_y() const noexcept { return max_y_; }

    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const XYLine &other) const noexcept;

private:
    XYLine(std::vector<float> xs, std::vector<float> ys, float min_x, float max_x, float min_y, float max_y);

    std::vector<float> xs_;
    std::vector<float> ys_;
    float min_x_;
    float max_x_;
    float min_y_;
    float max_y_;
};

} // namespace lucene
