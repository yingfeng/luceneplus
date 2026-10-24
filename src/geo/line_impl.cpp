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

module lucene_core:geo_line.impl;

import :geo_line;

import std;

import :error;
import :geo_component2d;
import :geo_geometry;
import :geo_utils;
import :line2d;
import :types;

namespace lucene {

Line::Line(std::vector<double> lats, std::vector<double> lons, double min_lat, double max_lat, double min_lon,
           double max_lon)
        : lats_(std::move(lats)),
          lons_(std::move(lons)),
          min_lat_(min_lat),
          max_lat_(max_lat),
          min_lon_(min_lon),
          max_lon_(max_lon) {}

Result<Line> Line::create(std::vector<double> lats, std::vector<double> lons) {
    if (lats.size() != lons.size()) {
        return err(LuceneError::illegal_argument("lats and lons must be equal length"));
    }
    if (lats.size() < 2) {
        return err(LuceneError::illegal_argument("at least 2 line points required"));
    }

    double min_lat = lats[0];
    double min_lon = lons[0];
    double max_lat = lats[0];
    double max_lon = lons[0];
    for (size_t i = 0; i < lats.size(); ++i) {
        if (auto checked = GeoUtils::check_latitude(lats[i]); !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
        if (auto checked = GeoUtils::check_longitude(lons[i]); !checked.has_value()) {
            return std::unexpected(std::move(checked.error()));
        }
        min_lat = std::min(lats[i], min_lat);
        min_lon = std::min(lons[i], min_lon);
        max_lat = std::max(lats[i], max_lat);
        max_lon = std::max(lons[i], max_lon);
    }

    return Line(std::move(lats), std::move(lons), min_lat, max_lat, min_lon, max_lon);
}

double Line::lat(size_t vertex) const {
    return lats_.at(vertex);
}

double Line::lon(size_t vertex) const {
    return lons_.at(vertex);
}

Result<std::shared_ptr<Component2D>> Line::to_component2d() const {
    // The component works in `(x, y)` space, which is `(longitude, latitude)`.
    auto component = Line2D::create(lons_, lats_);
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }
    return std::make_shared<Line2D>(std::move(component.value()));
}

std::string Line::to_string() const {
    std::string text = "Line(";
    for (size_t i = 0; i < lats_.size(); ++i) {
        text += "[" + std::format("{}", lons_[i]) + ", " + std::format("{}", lats_[i]) + "]";
    }
    text += ")";
    return text;
}

bool Line::equals(const Line &other) const noexcept { return lats_ == other.lats_ && lons_ == other.lons_; }

XYLine::XYLine(std::vector<float> xs, std::vector<float> ys, float min_x, float max_x, float min_y, float max_y)
        : xs_(std::move(xs)), ys_(std::move(ys)), min_x_(min_x), max_x_(max_x), min_y_(min_y), max_y_(max_y) {}

Result<XYLine> XYLine::create(std::vector<float> xs, std::vector<float> ys) {
    if (xs.size() != ys.size()) {
        return err(LuceneError::illegal_argument("x and y must be equal length"));
    }
    if (xs.size() < 2) {
        return err(LuceneError::illegal_argument("at least 2 line points required"));
    }

    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = -std::numeric_limits<float>::max();
    float max_y = -std::numeric_limits<float>::max();
    for (size_t i = 0; i < xs.size(); ++i) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) {
            return err(LuceneError::illegal_argument("invalid line point: the coordinates must be finite"));
        }
        min_x = std::min(xs[i], min_x);
        min_y = std::min(ys[i], min_y);
        max_x = std::max(xs[i], max_x);
        max_y = std::max(ys[i], max_y);
    }

    return XYLine(std::move(xs), std::move(ys), min_x, max_x, min_y, max_y);
}

float XYLine::x(size_t vertex) const {
    return xs_.at(vertex);
}

float XYLine::y(size_t vertex) const {
    return ys_.at(vertex);
}

Result<std::shared_ptr<Component2D>> XYLine::to_component2d() const {
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

    auto component = Line2D::create(xs, ys);
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }
    return std::make_shared<Line2D>(std::move(component.value()));
}

std::string XYLine::to_string() const {
    std::string text = "XYLine(";
    for (size_t i = 0; i < xs_.size(); ++i) {
        text += "[" + std::format("{}", xs_[i]) + ", " + std::format("{}", ys_[i]) + "]";
    }
    text += ")";
    return text;
}

bool XYLine::equals(const XYLine &other) const noexcept { return xs_ == other.xs_ && ys_ == other.ys_; }

} // namespace lucene
