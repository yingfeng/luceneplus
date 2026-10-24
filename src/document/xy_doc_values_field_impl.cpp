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

module lucene_core:xy_doc_values_field.impl;

import :xy_doc_values_field;

import std;

import :doc_values_fields;
import :error;
import :geo_circle;
import :geo_geometry;
import :geo_rectangle;
import :indexable_field;
import :query;
import :types;
import :xy_doc_values_point_in_geometry_query;
import :xy_encoding_utils;

namespace lucene {

Result<int64_t> XYDocValuesField::pack(float x, float y) {
    auto encoded_x = XYEncodingUtils::encode(x);
    if (!encoded_x.has_value()) {
        return std::unexpected(std::move(encoded_x.error()));
    }
    auto encoded_y = XYEncodingUtils::encode(y);
    if (!encoded_y.has_value()) {
        return std::unexpected(std::move(encoded_y.error()));
    }
    const auto high = static_cast<uint64_t>(static_cast<int64_t>(encoded_x.value())) << 32U;
    const auto low = static_cast<uint64_t>(static_cast<uint32_t>(encoded_y.value()));
    return static_cast<int64_t>(high | low);
}

float XYDocValuesField::decode_x(int64_t packed) {
    return XYEncodingUtils::decode(static_cast<int32_t>(static_cast<uint64_t>(packed) >> 32U));
}

float XYDocValuesField::decode_y(int64_t packed) {
    return XYEncodingUtils::decode(static_cast<int32_t>(static_cast<uint32_t>(packed)));
}

Result<Field> XYDocValuesField::create(std::string name, float x, float y) {
    auto packed = pack(x, y);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return SortedNumericDocValuesField::create(std::move(name), packed.value());
}

Result<std::shared_ptr<Query>> XYDocValuesField::new_slow_box_query(std::string field, float min_x, float max_x,
                                                                    float min_y, float max_y) {
    auto rectangle = XYRectangle::create(min_x, max_x, min_y, max_y);
    if (!rectangle.has_value()) {
        return std::unexpected(std::move(rectangle.error()));
    }
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<XYRectangle>(std::move(rectangle.value())));
    return new_slow_geometry_query(std::move(field), std::move(geometries));
}

Result<std::shared_ptr<Query>> XYDocValuesField::new_slow_distance_query(std::string field, float x, float y,
                                                                         float radius) {
    auto circle = XYCircle::create(x, y, radius);
    if (!circle.has_value()) {
        return std::unexpected(std::move(circle.error()));
    }
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<XYCircle>(std::move(circle.value())));
    return new_slow_geometry_query(std::move(field), std::move(geometries));
}

Result<std::shared_ptr<Query>> XYDocValuesField::new_slow_geometry_query(
        std::string field, std::vector<std::shared_ptr<const Geometry>> geometries) {
    auto query = XYDocValuesPointInGeometryQuery::create(std::move(field), std::move(geometries));
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::move(query.value()));
}

} // namespace lucene
