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

export module lucene_core:geo_polygon;

import :types;
import std;

import :error;
import :geo_component2d;
import :geo_geometry;
import :geo_utils;

namespace lucene {

/// Represents a closed polygon on the earth's surface, in decimal degrees.
///
/// Port of `Polygon` (`core/geo/polygon.rs`).
///
/// Notes that the shape relies on:
///
///  1. the shell must be in clockwise order, the holes counter-clockwise;
///  2. the polygon must be closed (the last vertex repeats the first);
///  3. it must not cross itself, nor the 180th meridian (use two polygons, one on
///     each side, instead);
///  4. holes may not nest.
///
/// **Deviation:** `from_geo_json` is a free function in `:geo_json_polygon`
/// (`polygons_from_geo_json`) rather than a static member here, because the reader
/// depends on this module.
export class Polygon final : public LatLonGeometry {
public:
    /// A polygon over the vertices `(poly_lats[i], poly_lons[i])` with the given
    /// holes, after validating all of them.
    [[nodiscard]] static Result<Polygon> create(std::vector<double> poly_lats, std::vector<double> poly_lons,
                                                std::vector<Polygon> holes = {});

    [[nodiscard]] size_t num_points() const noexcept { return poly_lats_.size(); }
    [[nodiscard]] double lat(size_t vertex) const;
    [[nodiscard]] double lon(size_t vertex) const;
    [[nodiscard]] const std::vector<double> &poly_lats() const noexcept { return poly_lats_; }
    [[nodiscard]] const std::vector<double> &poly_lons() const noexcept { return poly_lons_; }

    [[nodiscard]] size_t num_holes() const noexcept { return holes_.size(); }
    [[nodiscard]] const std::vector<Polygon> &holes() const noexcept { return holes_; }

    [[nodiscard]] double min_lat() const noexcept { return min_lat_; }
    [[nodiscard]] double max_lat() const noexcept { return max_lat_; }
    [[nodiscard]] double min_lon() const noexcept { return min_lon_; }
    [[nodiscard]] double max_lon() const noexcept { return max_lon_; }

    /// The winding order of the shell.
    [[nodiscard]] WindingOrder winding_order() const noexcept { return winding_order_; }

    /// The vertices as a GeoJSON coordinate array: `[[lon, lat], ...]`.
    [[nodiscard]] static std::string vertices_to_geo_json(std::span<const double> lats, std::span<const double> lons);

    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const Polygon &other) const;

private:
    Polygon(std::vector<double> poly_lats, std::vector<double> poly_lons, std::vector<Polygon> holes, double min_lat,
            double max_lat, double min_lon, double max_lon, WindingOrder winding_order);

    std::vector<double> poly_lats_;
    std::vector<double> poly_lons_;
    std::vector<Polygon> holes_;
    double min_lat_;
    double max_lat_;
    double min_lon_;
    double max_lon_;
    WindingOrder winding_order_;
};

/// Represents a polygon in the cartesian plane: the vertices are floats.
///
/// Port of `XYPolygon` (`core/geo/xy_polygon.rs`).
///
/// The same rules apply as for [`Polygon`], only in `x`/`y` space instead of
/// longitude/latitude, and the vertices are floats.
export class XYPolygon final : public XYGeometry {
public:
    /// A polygon over the vertices `(xs[i], ys[i])` with the given holes, after
    /// validating all of them.
    [[nodiscard]] static Result<XYPolygon> create(std::vector<float> xs, std::vector<float> ys,
                                                  std::vector<XYPolygon> holes = {});

    [[nodiscard]] size_t num_points() const noexcept { return xs_.size(); }
    [[nodiscard]] float x(size_t vertex) const;
    [[nodiscard]] float y(size_t vertex) const;
    [[nodiscard]] const std::vector<float> &poly_x() const noexcept { return xs_; }
    [[nodiscard]] const std::vector<float> &poly_y() const noexcept { return ys_; }

    [[nodiscard]] size_t num_holes() const noexcept { return holes_.size(); }
    [[nodiscard]] const std::vector<XYPolygon> &holes() const noexcept { return holes_; }

    [[nodiscard]] float min_x() const noexcept { return min_x_; }
    [[nodiscard]] float max_x() const noexcept { return max_x_; }
    [[nodiscard]] float min_y() const noexcept { return min_y_; }
    [[nodiscard]] float max_y() const noexcept { return max_y_; }

    /// The winding order of the shell.
    [[nodiscard]] WindingOrder winding_order() const noexcept { return winding_order_; }

    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool equals(const XYPolygon &other) const;

private:
    XYPolygon(std::vector<float> xs, std::vector<float> ys, std::vector<XYPolygon> holes, float min_x, float max_x,
              float min_y, float max_y, WindingOrder winding_order);

    std::vector<float> xs_;
    std::vector<float> ys_;
    std::vector<XYPolygon> holes_;
    float min_x_;
    float max_x_;
    float min_y_;
    float max_y_;
    WindingOrder winding_order_;
};

} // namespace lucene
