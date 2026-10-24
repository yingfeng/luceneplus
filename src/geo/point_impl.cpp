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

module lucene_core:geo_point.impl;

import :geo_point;

import std;

import :error;
import :geo_component2d;
import :geo_encoding_utils;
import :geo_geometry;
import :geo_utils;
import :point2d;
import :types;

namespace lucene {

Point::Point(double lat, double lon) : lat_(lat), lon_(lon) {}

Result<Point> Point::create(double lat, double lon) {
    if (auto checked = GeoUtils::check_latitude(lat); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(lon); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return Point(lat, lon);
}

Result<std::shared_ptr<Component2D>> Point::to_component2d() const {
    // Quantize the way the index stores the coordinates; the extreme values are
    // kept as they are, because rounding them up would leave the valid range.
    double q_lat = lat_;
    if (lat_ != GeoUtils::MAX_LAT_INCL) {
        auto encoded = GeoEncodingUtils::encode_latitude_ceil(lat_);
        if (!encoded.has_value()) {
            return std::unexpected(std::move(encoded.error()));
        }
        q_lat = GeoEncodingUtils::decode_latitude(encoded.value());
    }

    double q_lon = lon_;
    if (lon_ != GeoUtils::MAX_LON_INCL) {
        auto encoded = GeoEncodingUtils::encode_longitude_ceil(lon_);
        if (!encoded.has_value()) {
            return std::unexpected(std::move(encoded.error()));
        }
        q_lon = GeoEncodingUtils::decode_longitude(encoded.value());
    }

    return std::shared_ptr<Component2D>(std::make_shared<Point2D>(q_lon, q_lat));
}

std::string Point::to_string() const {
    return "Point(" + std::format("{}", lon_) + "," + std::format("{}", lat_) + ")";
}

bool Point::equals(const Point &other) const noexcept { return lat_ == other.lat_ && lon_ == other.lon_; }

XYPoint::XYPoint(float x, float y) : x_(x), y_(y) {}

Result<XYPoint> XYPoint::create(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return err(LuceneError::illegal_argument("invalid point: both coordinates must be finite"));
    }
    return XYPoint(x, y);
}

Result<std::shared_ptr<Component2D>> XYPoint::to_component2d() const {
    return std::shared_ptr<Component2D>(
            std::make_shared<Point2D>(static_cast<double>(x_), static_cast<double>(y_)));
}

std::string XYPoint::to_string() const {
    return "XYPoint(" + std::format("{}", x_) + "," + std::format("{}", y_) + ")";
}

bool XYPoint::equals(const XYPoint &other) const noexcept { return x_ == other.x_ && y_ == other.y_; }

} // namespace lucene
