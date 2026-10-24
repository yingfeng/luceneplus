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

module lucene_core:lat_lon_doc_values_field.impl;

import :lat_lon_doc_values_field;

import std;

import :doc_values_fields;
import :error;
import :indexable_field;
import :geo_circle;
import :geo_encoding_utils;
import :geo_geometry;
import :lat_lon_doc_values_box_query;
import :lat_lon_doc_values_query;
import :lat_lon_point_sort_field;
import :query;
import :shape_field;
import :types;

namespace lucene {

Result<int64_t> LatLonDocValuesField::pack(double latitude, double longitude) {
    auto encoded_latitude = GeoEncodingUtils::encode_latitude(latitude);
    if (!encoded_latitude.has_value()) {
        return std::unexpected(std::move(encoded_latitude.error()));
    }
    auto encoded_longitude = GeoEncodingUtils::encode_longitude(longitude);
    if (!encoded_longitude.has_value()) {
        return std::unexpected(std::move(encoded_longitude.error()));
    }
    const auto high = static_cast<uint64_t>(static_cast<int64_t>(encoded_latitude.value())) << 32U;
    const auto low = static_cast<uint64_t>(static_cast<uint32_t>(encoded_longitude.value()));
    return static_cast<int64_t>(high | low);
}

double LatLonDocValuesField::decode_latitude(int64_t packed) {
    return GeoEncodingUtils::decode_latitude(static_cast<int32_t>(static_cast<uint64_t>(packed) >> 32U));
}

double LatLonDocValuesField::decode_longitude(int64_t packed) {
    return GeoEncodingUtils::decode_longitude(static_cast<int32_t>(static_cast<uint32_t>(packed)));
}

Result<Field> LatLonDocValuesField::create(std::string name, double latitude, double longitude) {
    auto packed = pack(latitude, longitude);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    return SortedNumericDocValuesField::create(std::move(name), packed.value());
}

Result<LatLonPointSortField> LatLonDocValuesField::new_distance_sort(std::string field, double latitude,
                                                                   double longitude) {
    return LatLonPointSortField::create(std::move(field), latitude, longitude);
}

Result<std::shared_ptr<Query>> LatLonDocValuesField::new_slow_distance_query(std::string field, double latitude,
                                                                            double longitude, double radius_meters) {
    auto circle = Circle::create(latitude, longitude, radius_meters);
    if (!circle.has_value()) {
        return std::unexpected(std::move(circle.error()));
    }
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<Circle>(std::move(circle.value())));
    return new_slow_geometry_query(std::move(field), QueryRelation::Intersects, std::move(geometries));
}

Result<std::shared_ptr<Query>> LatLonDocValuesField::new_slow_geometry_query(
        std::string field, QueryRelation query_relation, std::vector<std::shared_ptr<const Geometry>> geometries) {
    auto query = LatLonDocValuesQuery::create(std::move(field), query_relation, std::move(geometries));
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::move(query.value()));
}

Result<std::shared_ptr<Query>> LatLonDocValuesField::new_slow_box_query(std::string field, double min_latitude,
                                                                       double max_latitude, double min_longitude,
                                                                       double max_longitude) {
    auto query = LatLonDocValuesBoxQuery::create(std::move(field), min_latitude, max_latitude, min_longitude,
                                                 max_longitude);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::move(query.value()));
}

} // namespace lucene
