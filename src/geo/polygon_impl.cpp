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

module lucene_core:geo_polygon.impl;

import :geo_polygon;

import std;

import :error;
import :geo_component2d;
import :geo_geometry;
import :geo_utils;
import :polygon2d;
import :types;

namespace lucene {

Polygon::Polygon(std::vector<double> poly_lats, std::vector<double> poly_lons, std::vector<Polygon> holes,
                 double min_lat, double max_lat, double min_lon, double max_lon, WindingOrder winding_order)
        : poly_lats_(std::move(poly_lats)),
          poly_lons_(std::move(poly_lons)),
          holes_(std::move(holes)),
          min_lat_(min_lat),
          max_lat_(max_lat),
          min_lon_(min_lon),
          max_lon_(max_lon),
          winding_order_(winding_order) {}

Result<Polygon> Polygon::create(std::vector<double> poly_lats, std::vector<double> poly_lons,
                                std::vector<Polygon> holes) {
    if (poly_lats.size() != poly_lons.size()) {
        return err(LuceneError::illegal_argument("polyLats and polyLons must be equal length"));
    }
    if (poly_lats.size() < 4) {
        return err(LuceneError::illegal_argument("at least 4 polygon points required"));
    }
    if (poly_lats.front() != poly_lats.back()) {
        return err(LuceneError::illegal_argument(
                "first and last points of the polygon must be the same (it must close itself): polyLats[0]=" +
                std::format("{}", poly_lats.front()) + " polyLats[" + std::to_string(poly_lats.size() - 1) +
                "]=" + std::format("{}", poly_lats.back())));
    }
    if (poly_lons.front() != poly_lons.back()) {
        return err(LuceneError::illegal_argument(
                "first and last points of the polygon must be the same (it must close itself): polyLons[0]=" +
                std::format("{}", poly_lons.front()) + " polyLons[" + std::to_string(poly_lons.size() - 1) +
                "]=" + std::format("{}", poly_lons.back())));
    }

    for (size_t i = 0; i < poly_lats.size(); ++i) {
        if (auto checked = GeoUtils::check_latitude(poly_lats[i]); !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
        if (auto checked = GeoUtils::check_longitude(poly_lons[i]); !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
    }

    for (const Polygon &inner : holes) {
        if (!inner.holes_.empty()) {
            return err(LuceneError::illegal_argument("holes may not contain holes: polygons may not nest."));
        }
    }

    double min_lat = poly_lats[0];
    double max_lat = poly_lats[0];
    double min_lon = poly_lons[0];
    double max_lon = poly_lons[0];

    double winding_sum = 0.0;
    const size_t num_pts = poly_lats.size() - 1;
    for (size_t i = 1; i < num_pts; ++i) {
        const size_t j = i - 1;
        min_lat = std::min(poly_lats[i], min_lat);
        max_lat = std::max(poly_lats[i], max_lat);
        min_lon = std::min(poly_lons[i], min_lon);
        max_lon = std::max(poly_lons[i], max_lon);
        winding_sum += (poly_lons[j] - poly_lons[num_pts]) * (poly_lats[i] - poly_lats[num_pts]) -
                       (poly_lats[j] - poly_lats[num_pts]) * (poly_lons[i] - poly_lons[num_pts]);
    }

    const WindingOrder winding_order = winding_sum < 0.0 ? WindingOrder::Ccw : WindingOrder::Cw;

    return Polygon(std::move(poly_lats), std::move(poly_lons), std::move(holes), min_lat, max_lat, min_lon, max_lon,
                   winding_order);
}

double Polygon::lat(size_t vertex) const {
    return poly_lats_.at(vertex);
}

double Polygon::lon(size_t vertex) const {
    return poly_lons_.at(vertex);
}

std::string Polygon::vertices_to_geo_json(std::span<const double> lats, std::span<const double> lons) {
    std::string text = "[";
    for (size_t i = 0; i < lats.size(); ++i) {
        text += "[" + std::format("{}", lons[i]) + ", " + std::format("{}", lats[i]) + "]";
        if (i != lats.size() - 1) {
            text += ", ";
        }
    }
    text += "]";
    return text;
}

Result<std::shared_ptr<Component2D>> Polygon::to_component2d() const {
    std::shared_ptr<Component2D> holes_component;
    if (!holes_.empty()) {
        std::vector<const Geometry *> hole_geometries;
        hole_geometries.reserve(holes_.size());
        for (const Polygon &hole : holes_) {
            hole_geometries.push_back(&hole);
        }
        auto built = create_geometry_component(hole_geometries);
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        holes_component = std::move(built.value());
    }

    // The component works in `(x, y)` space, which is `(longitude, latitude)`.
    auto component =
            Polygon2D::create(min_lon_, max_lon_, min_lat_, max_lat_, poly_lons_, poly_lats_, std::move(holes_component));
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }
    return std::make_shared<Polygon2D>(std::move(component.value()));
}

std::string Polygon::to_string() const {
    std::string text = "Polygon";
    for (size_t i = 0; i < poly_lats_.size(); ++i) {
        text += "[" + std::format("{}", poly_lats_[i]) + ", " + std::format("{}", poly_lons_[i]) + "] ";
    }
    if (!holes_.empty()) {
        // The reference implementation prints the holes with `Debug`; the port
        // prints how many there are, which is what its `to_string` callers use.
        text += ", holes=" + std::to_string(holes_.size());
    }
    return text;
}

bool Polygon::equals(const Polygon &other) const {
    if (poly_lats_ != other.poly_lats_ || poly_lons_ != other.poly_lons_ || holes_.size() != other.holes_.size()) {
        return false;
    }
    for (size_t i = 0; i < holes_.size(); ++i) {
        if (!holes_[i].equals(other.holes_[i])) {
            return false;
        }
    }
    return true;
}

XYPolygon::XYPolygon(std::vector<float> xs, std::vector<float> ys, std::vector<XYPolygon> holes, float min_x,
                       float max_x, float min_y, float max_y, WindingOrder winding_order)
        : xs_(std::move(xs)),
          ys_(std::move(ys)),
          holes_(std::move(holes)),
          min_x_(min_x),
          max_x_(max_x),
          min_y_(min_y),
          max_y_(max_y),
          winding_order_(winding_order) {}

Result<XYPolygon> XYPolygon::create(std::vector<float> xs, std::vector<float> ys, std::vector<XYPolygon> holes) {
    if (xs.size() != ys.size()) {
        return err(LuceneError::illegal_argument("x and y must be equal length"));
    }
    if (xs.size() < 4) {
        return err(LuceneError::illegal_argument("at least 4 polygon points required"));
    }
    if (xs.front() != xs.back()) {
        return err(LuceneError::illegal_argument(
                "first and last points of the polygon must be the same (it must close itself): x[0]=" +
                std::format("{}", xs.front()) + " x[" + std::to_string(xs.size() - 1) +
                "]=" + std::format("{}", xs.back())));
    }
    if (ys.front() != ys.back()) {
        return err(LuceneError::illegal_argument(
                "first and last points of the polygon must be the same (it must close itself): y[0]=" +
                std::format("{}", ys.front()) + " y[" + std::to_string(ys.size() - 1) +
                "]=" + std::format("{}", ys.back())));
    }
    for (const XYPolygon &inner : holes) {
        if (!inner.holes_.empty()) {
            return err(LuceneError::illegal_argument("holes may not contain holes: polygons may not nest."));
        }
    }

    for (size_t i = 0; i < xs.size(); ++i) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) {
            return err(LuceneError::illegal_argument("invalid polygon point: the coordinates must be finite"));
        }
    }

    float min_x = xs[0];
    float max_x = xs[0];
    float min_y = ys[0];
    float max_y = ys[0];

    double winding_sum = 0.0;
    const size_t num_pts = xs.size() - 1;
    for (size_t i = 1; i < num_pts; ++i) {
        const size_t j = i - 1;
        min_x = std::min(xs[i], min_x);
        max_x = std::max(xs[i], max_x);
        min_y = std::min(ys[i], min_y);
        max_y = std::max(ys[i], max_y);
        winding_sum += static_cast<double>((xs[j] - xs[num_pts]) * (ys[i] - ys[num_pts]) -
                                           (ys[j] - ys[num_pts]) * (xs[i] - xs[num_pts]));
    }

    const WindingOrder winding_order = winding_sum < 0.0 ? WindingOrder::Ccw : WindingOrder::Cw;

    return XYPolygon(std::move(xs), std::move(ys), std::move(holes), min_x, max_x, min_y, max_y, winding_order);
}

float XYPolygon::x(size_t vertex) const {
    return xs_.at(vertex);
}

float XYPolygon::y(size_t vertex) const {
    return ys_.at(vertex);
}

Result<std::shared_ptr<Component2D>> XYPolygon::to_component2d() const {
    std::shared_ptr<Component2D> holes_component;
    if (!holes_.empty()) {
        std::vector<const Geometry *> hole_geometries;
        hole_geometries.reserve(holes_.size());
        for (const XYPolygon &hole : holes_) {
            hole_geometries.push_back(&hole);
        }
        auto built = create_geometry_component(hole_geometries);
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        holes_component = std::move(built.value());
    }

    std::vector<double> xs;
    xs.reserve(xs_.size());
    for (const float value : xs_) {
        xs.push_back(static_cast<double>(value));
    }
    std::vector<double> ys;
    ys.reserve(ys_.size());
    for (const float value : ys_) {
        ys.push_back(static_cast<double>(value));
    }

    auto component = Polygon2D::create(static_cast<double>(min_x_), static_cast<double>(max_x_),
                                       static_cast<double>(min_y_), static_cast<double>(max_y_), xs, ys,
                                       std::move(holes_component));
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }
    return std::make_shared<Polygon2D>(std::move(component.value()));
}

std::string XYPolygon::to_string() const {
    std::string text = "XYPolygon";
    for (size_t i = 0; i < xs_.size(); ++i) {
        text += "[" + std::format("{}", xs_[i]) + ", " + std::format("{}", ys_[i]) + "] ";
    }
    if (!holes_.empty()) {
        text += ", holes=[";
        for (size_t i = 0; i < holes_.size(); ++i) {
            if (i > 0) {
                text += ", ";
            }
            text += holes_[i].to_string();
        }
        text += "]";
    }
    return text;
}

bool XYPolygon::equals(const XYPolygon &other) const {
    if (xs_ != other.xs_ || ys_ != other.ys_ || holes_.size() != other.holes_.size()) {
        return false;
    }
    for (size_t i = 0; i < holes_.size(); ++i) {
        if (!holes_[i].equals(other.holes_[i])) {
            return false;
        }
    }
    return true;
}

} // namespace lucene
