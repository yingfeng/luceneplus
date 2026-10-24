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

module lucene_core:xy_point_field.impl;

import :xy_point_field;

import std;

import :error;
import :field_type;
import :geo_circle;
import :geo_geometry;
import :geo_rectangle;
import :indexable_field;
import :numeric_utils;
import :query;
import :types;
import :xy_encoding_utils;
import :xy_point_in_geometry_query;

namespace lucene {

Result<std::vector<uint8_t>> XYPointField::pack(float x, float y) {
    auto encoded_x = XYEncodingUtils::encode(x);
    if (!encoded_x.has_value()) {
        return std::unexpected(std::move(encoded_x.error()));
    }
    auto encoded_y = XYEncodingUtils::encode(y);
    if (!encoded_y.has_value()) {
        return std::unexpected(std::move(encoded_y.error()));
    }

    std::vector<uint8_t> packed(DIMENSIONS * BYTES, 0);
    NumericUtils::int_to_sortable_bytes(encoded_x.value(), packed, 0);
    NumericUtils::int_to_sortable_bytes(encoded_y.value(), packed, BYTES);
    return packed;
}

float XYPointField::decode_x(std::span<const uint8_t> packed) { return XYEncodingUtils::decode_bytes(packed, 0); }

float XYPointField::decode_y(std::span<const uint8_t> packed) {
    return XYEncodingUtils::decode_bytes(packed, BYTES);
}

Result<Field> XYPointField::create(std::string name, float x, float y) {
    auto packed = pack(x, y);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    FieldType type;
    if (auto set = type.set_dimensions(DIMENSIONS, BYTES); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }
    type.freeze();
    return Field::from_bytes(std::move(name), std::move(packed.value()), std::move(type));
}

Result<std::shared_ptr<Query>> XYPointField::new_box_query(std::string field, float min_x, float max_x, float min_y,
                                                           float max_y) {
    // The rectangle validates the bounds, so the query does not have to.
    auto rectangle = XYRectangle::create(min_x, max_x, min_y, max_y);
    if (!rectangle.has_value()) {
        return std::unexpected(std::move(rectangle.error()));
    }
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<XYRectangle>(std::move(rectangle.value())));
    return new_geometry_query(std::move(field), std::move(geometries));
}

Result<std::shared_ptr<Query>> XYPointField::new_distance_query(std::string field, float x, float y, float radius) {
    auto circle = XYCircle::create(x, y, radius);
    if (!circle.has_value()) {
        return std::unexpected(std::move(circle.error()));
    }
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<XYCircle>(std::move(circle.value())));
    return new_geometry_query(std::move(field), std::move(geometries));
}

Result<std::shared_ptr<Query>> XYPointField::new_geometry_query(
        std::string field, std::vector<std::shared_ptr<const Geometry>> geometries) {
    auto query = XYPointInGeometryQuery::create(std::move(field), std::move(geometries));
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::move(query.value()));
}

} // namespace lucene
