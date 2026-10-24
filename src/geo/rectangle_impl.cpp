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

module lucene_core:geo_rectangle.impl;

import :geo_rectangle;

import std;

import :component_tree;
import :error;
import :geo_component2d;
import :geo_encoding_utils;
import :geo_geometry;
import :geo_utils;
import :rectangle2d;
import :sloppy_math;
import :types;

namespace lucene {

namespace {

/// The quantized end points of the longitude range.
///
/// Encoding +/-180 degrees yields the extremes of `int32`, so these are the values
/// the decoder produces for them; the reference implementation spells them as the
/// product of the extreme integer and the reciprocal scale, and multiplying by it
/// is what the decoder does too.
[[nodiscard]] double min_lon_incl_quantize() {
    return GeoEncodingUtils::decode_longitude(GeoEncodingUtils::min_lon_encoded());
}

[[nodiscard]] double max_lon_incl_quantize() {
    return GeoEncodingUtils::decode_longitude(GeoEncodingUtils::max_lon_encoded());
}

} // namespace

Rectangle::Rectangle(double min_lat, double max_lat, double min_lon, double max_lon)
        : min_lat_(min_lat), max_lat_(max_lat), min_lon_(min_lon), max_lon_(max_lon) {}

Result<Rectangle> Rectangle::create(double min_lat, double max_lat, double min_lon, double max_lon) {
    if (auto checked = GeoUtils::check_latitude(min_lat); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_latitude(max_lat); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(min_lon); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(max_lon); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (max_lat < min_lat) {
        // The reference implementation only asserts this; the port reports it,
        // because an inverted box would silently match nothing.
        return err(LuceneError::illegal_argument("maxLat must not be lower than minLat, got " +
                                                 std::to_string(max_lat) + " < " + std::to_string(min_lat)));
    }
    // NOTE: `max_lon < min_lon` is allowed, the box then crosses the dateline.
    return Rectangle(min_lat, max_lat, min_lon, max_lon);
}

Result<Rectangle> Rectangle::from_point_distance(double center_lat, double center_lon, double radius_meters) {
    if (auto checked = GeoUtils::check_latitude(center_lat); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(center_lon); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }

    constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;
    const double rad_lat = center_lat * kRadiansPerDegree;
    const double rad_lon = center_lon * kRadiansPerDegree;

    // The 7E-2 is Lucene's fudge factor, so the box is never too small.
    const double rad_distance = (radius_meters + 7E-2) / GeoUtils::EARTH_MEAN_RADIUS_METERS;
    double min_lat = rad_lat - rad_distance;
    double max_lat = rad_lat + rad_distance;
    double min_lon = 0.0;
    double max_lon = 0.0;

    if (min_lat > GeoUtils::MIN_LAT_RADIANS && max_lat < GeoUtils::MAX_LAT_RADIANS) {
        const double delta_lon =
                SloppyMath::asin(GeoUtils::sloppy_sin(rad_distance) / SloppyMath::cos(rad_lat));

        double computed_min_lon = rad_lon - delta_lon;
        if (computed_min_lon < GeoUtils::MIN_LON_RADIANS) {
            computed_min_lon += 2.0 * std::numbers::pi;
        }

        double computed_max_lon = rad_lon + delta_lon;
        if (computed_max_lon > GeoUtils::MAX_LON_RADIANS) {
            computed_max_lon -= 2.0 * std::numbers::pi;
        }

        min_lon = computed_min_lon;
        max_lon = computed_max_lon;
    } else {
        // A pole is within the distance: every longitude is in the box.
        min_lat = std::max(min_lat, GeoUtils::MIN_LAT_RADIANS);
        max_lat = std::min(max_lat, GeoUtils::MAX_LAT_RADIANS);
        min_lon = GeoUtils::MIN_LON_RADIANS;
        max_lon = GeoUtils::MAX_LON_RADIANS;
    }

    return Rectangle::create(min_lat / kRadiansPerDegree, max_lat / kRadiansPerDegree,
                             min_lon / kRadiansPerDegree, max_lon / kRadiansPerDegree);
}

double Rectangle::axis_lat(double center_lat, double radius_meters) {
    constexpr double pio2 = std::numbers::pi / 2.0;
    double l1 = center_lat * std::numbers::pi / 180.0;
    const double r = (radius_meters + 7E-2) / GeoUtils::EARTH_MEAN_RADIUS_METERS;

    // A pole is within the distance, so the latitude is the pole itself.
    if (std::abs(l1) + r >= GeoUtils::MAX_LAT_RADIANS) {
        return center_lat >= 0.0 ? GeoUtils::MAX_LAT_INCL : GeoUtils::MIN_LAT_INCL;
    }

    // Measure l1 from the closest pole, so the triangle with the bounding-box
    // meridians is a right triangle and l1 is in (0, pi/2].
    l1 = center_lat >= 0.0 ? pio2 - l1 : l1 + pio2;

    double l2 = std::acos(std::cos(l1) / std::cos(r));
    l2 = center_lat >= 0.0 ? pio2 - l2 : l2 - pio2;
    return l2 * 180.0 / std::numbers::pi;
}

bool Rectangle::contains_point(double lat, double lon, double min_lat, double max_lat, double min_lon,
                               double max_lon) {
    return lat >= min_lat && lat <= max_lat && lon >= min_lon && lon <= max_lon;
}

Result<std::shared_ptr<Component2D>> Rectangle::to_component2d() const {
    // A box that starts at 180 degrees and wraps does not cross the dateline: it
    // starts at -180 instead.
    double min_longitude = min_lon_;
    bool crosses_dateline = min_lon_ > max_lon_;
    if (min_longitude == GeoUtils::MAX_LON_INCL && crosses_dateline) {
        min_longitude = GeoUtils::MIN_LON_INCL;
        crosses_dateline = false;
    }

    auto encoded_min_lat = GeoEncodingUtils::encode_latitude_ceil(min_lat_);
    if (!encoded_min_lat.has_value()) {
        return std::unexpected(std::move(encoded_min_lat.error()));
    }
    auto encoded_max_lat = GeoEncodingUtils::encode_latitude(max_lat_);
    if (!encoded_max_lat.has_value()) {
        return std::unexpected(std::move(encoded_max_lat.error()));
    }
    auto encoded_min_lon = GeoEncodingUtils::encode_longitude_ceil(min_longitude);
    if (!encoded_min_lon.has_value()) {
        return std::unexpected(std::move(encoded_min_lon.error()));
    }
    auto encoded_max_lon = GeoEncodingUtils::encode_longitude(max_lon_);
    if (!encoded_max_lon.has_value()) {
        return std::unexpected(std::move(encoded_max_lon.error()));
    }

    const double q_min_lat = GeoEncodingUtils::decode_latitude(encoded_min_lat.value());
    const double q_max_lat = GeoEncodingUtils::decode_latitude(encoded_max_lat.value());
    const double q_min_lon = GeoEncodingUtils::decode_longitude(encoded_min_lon.value());
    const double q_max_lon = GeoEncodingUtils::decode_longitude(encoded_max_lon.value());

    if (!crosses_dateline) {
        return std::shared_ptr<Component2D>(std::make_shared<Rectangle2D>(q_min_lon, q_max_lon, q_min_lat, q_max_lat));
    }

    // The box spans the dateline: it is the union of the part before 180 and the
    // part after -180.
    std::vector<std::shared_ptr<Component2D>> components;
    components.push_back(std::make_shared<Rectangle2D>(min_lon_incl_quantize(), q_max_lon, q_min_lat, q_max_lat));
    components.push_back(std::make_shared<Rectangle2D>(q_min_lon, max_lon_incl_quantize(), q_min_lat, q_max_lat));
    auto tree = ComponentTree::create(std::move(components));
    if (!tree.has_value()) {
        return std::unexpected(std::move(tree.error()));
    }
    return std::shared_ptr<Component2D>(std::move(tree.value()));
}

std::string Rectangle::to_string() const {
    std::string text = "Rectangle(lat=" + std::format("{}", min_lat_) + " TO " + std::format("{}", max_lat_) +
                       " lon=" + std::format("{}", min_lon_) + " TO " + std::format("{}", max_lon_);
    if (crosses_dateline()) {
        text += " [crosses dateline!]";
    }
    text += ")";
    return text;
}

bool Rectangle::equals(const Rectangle &other) const noexcept {
    return min_lat_ == other.min_lat_ && max_lat_ == other.max_lat_ && min_lon_ == other.min_lon_ &&
           max_lon_ == other.max_lon_;
}

XYRectangle::XYRectangle(float min_x, float max_x, float min_y, float max_y)
        : min_x_(min_x), max_x_(max_x), min_y_(min_y), max_y_(max_y) {}

Result<XYRectangle> XYRectangle::create(float min_x, float max_x, float min_y, float max_y) {
    if (!std::isfinite(min_x) || !std::isfinite(max_x) || !std::isfinite(min_y) || !std::isfinite(max_y)) {
        return err(LuceneError::illegal_argument("the bounds must be finite"));
    }
    if (min_x > max_x) {
        return err(LuceneError::illegal_argument("minX must be lower than maxX, got " + std::to_string(min_x) + " > " +
                                                 std::to_string(max_x)));
    }
    if (min_y > max_y) {
        return err(LuceneError::illegal_argument("minY must be lower than maxY, got " + std::to_string(min_y) + " > " +
                                                 std::to_string(max_y)));
    }
    return XYRectangle(min_x, max_x, min_y, max_y);
}

Result<XYRectangle> XYRectangle::from_point_distance(float x, float y, float radius) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return err(LuceneError::illegal_argument("the center must be finite"));
    }
    if (radius < 0.0F) {
        return err(LuceneError::illegal_argument("radius must be bigger than 0, got " + std::to_string(radius)));
    }
    if (!std::isfinite(radius)) {
        return err(LuceneError::illegal_argument("radius must be finite, got " + std::to_string(radius)));
    }

    // LUCENE-9243: round the box up, so numerical errors cannot shrink it.
    const float distance_box = std::nextafter(radius, std::numeric_limits<float>::infinity());
    constexpr float kFloatMax = std::numeric_limits<float>::max();
    return XYRectangle::create(std::max(-kFloatMax, x - distance_box), std::min(kFloatMax, x + distance_box),
                               std::max(-kFloatMax, y - distance_box), std::min(kFloatMax, y + distance_box));
}

Result<std::shared_ptr<Component2D>> XYRectangle::to_component2d() const {
    return std::shared_ptr<Component2D>(std::make_shared<Rectangle2D>(
            static_cast<double>(min_x_), static_cast<double>(max_x_), static_cast<double>(min_y_),
            static_cast<double>(max_y_)));
}

std::string XYRectangle::to_string() const {
    return "XYRectangle(x=" + std::format("{}", min_x_) + " TO " + std::format("{}", max_x_) + " y=" +
           std::format("{}", min_y_) + " TO " + std::format("{}", max_y_) + ")";
}

bool XYRectangle::equals(const XYRectangle &other) const noexcept {
    return min_x_ == other.min_x_ && max_x_ == other.max_x_ && min_y_ == other.min_y_ && max_y_ == other.max_y_;
}

} // namespace lucene
