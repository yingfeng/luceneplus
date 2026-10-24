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

module lucene_core:lat_lon_point.impl;

import :lat_lon_point;

import std;

import :error;
import :indexable_field;
import :field_type;
import :geo_component2d;
import :geo_encoding_utils;
import :geo_geometry;
import :geo_predicates;
import :boost_query;
import :lat_lon_point_distance_feature_query;
import :lat_lon_point_distance_query;
import :numeric_utils;
import :point_in_set_query;
import :query;
import :shape_field;
import :spatial_query;
import :types;

namespace lucene {

namespace {

/// A [`SpatialVisitor`] over the quantized lat/lon point index.
///
/// Port of `SpatialVisitorImpl` (`core/document/lat_lon_point_query.rs`): the cell
/// bounds are the encoded latitudes and longitudes, so the query shape is asked
/// about a lat/lon box; the per-point predicates go through the encoded
/// [`Component2DPredicate`].
class LatLonPointSpatialVisitor final : public SpatialVisitor {
public:
    LatLonPointSpatialVisitor(std::shared_ptr<Component2D> component, Component2DPredicate predicate, int32_t min_lat,
                              int32_t max_lat, int32_t min_lon, int32_t max_lon)
            : component_(std::move(component)),
              predicate_(std::move(predicate)),
              min_lat_(min_lat),
              max_lat_(max_lat),
              min_lon_(min_lon),
              max_lon_(max_lon) {}

    [[nodiscard]] Result<Relation> relate(std::span<const uint8_t> min_packed_value,
                                          std::span<const uint8_t> max_packed_value) const override {
        const int32_t lat_lower = NumericUtils::sortable_bytes_to_int(min_packed_value, 0);
        const int32_t lat_upper = NumericUtils::sortable_bytes_to_int(max_packed_value, 0);
        if (lat_lower > max_lat_ || lat_upper < min_lat_) {
            return Relation::kCellOutsideQuery;
        }

        const int32_t lon_lower = NumericUtils::sortable_bytes_to_int(min_packed_value, LatLonPoint::BYTES);
        const int32_t lon_upper = NumericUtils::sortable_bytes_to_int(max_packed_value, LatLonPoint::BYTES);
        if (lon_lower > max_lon_ || lon_upper < min_lon_) {
            return Relation::kCellOutsideQuery;
        }

        const double cell_min_lat = GeoEncodingUtils::decode_latitude(lat_lower);
        const double cell_min_lon = GeoEncodingUtils::decode_longitude(lon_lower);
        const double cell_max_lat = GeoEncodingUtils::decode_latitude(lat_upper);
        const double cell_max_lon = GeoEncodingUtils::decode_longitude(lon_upper);
        return component_->relate(cell_min_lon, cell_max_lon, cell_min_lat, cell_max_lat);
    }

    [[nodiscard]] Result<bool> intersects(std::span<const uint8_t> packed_value) const override {
        return predicate_.test(NumericUtils::sortable_bytes_to_int(packed_value, 0),
                               NumericUtils::sortable_bytes_to_int(packed_value, LatLonPoint::BYTES));
    }

    [[nodiscard]] Result<bool> within(std::span<const uint8_t> packed_value) const override {
        return predicate_.test(NumericUtils::sortable_bytes_to_int(packed_value, 0),
                               NumericUtils::sortable_bytes_to_int(packed_value, LatLonPoint::BYTES));
    }

    [[nodiscard]] Result<WithinRelation> contains(std::span<const uint8_t> packed_value) const override {
        return component_->within_point(
                GeoEncodingUtils::decode_longitude(
                        NumericUtils::sortable_bytes_to_int(packed_value, LatLonPoint::BYTES)),
                GeoEncodingUtils::decode_latitude(NumericUtils::sortable_bytes_to_int(packed_value, 0)));
    }

private:
    std::shared_ptr<Component2D> component_;
    Component2DPredicate predicate_;
    int32_t min_lat_{0};
    int32_t max_lat_{0};
    int32_t min_lon_{0};
    int32_t max_lon_{0};
};

} // namespace

Result<std::vector<uint8_t>> LatLonPoint::pack(double latitude, double longitude) {
    auto encoded_latitude = GeoEncodingUtils::encode_latitude(latitude);
    if (!encoded_latitude.has_value()) {
        return std::unexpected(std::move(encoded_latitude.error()));
    }
    auto encoded_longitude = GeoEncodingUtils::encode_longitude(longitude);
    if (!encoded_longitude.has_value()) {
        return std::unexpected(std::move(encoded_longitude.error()));
    }
    std::vector<uint8_t> packed(DIMENSIONS * BYTES, 0);
    NumericUtils::int_to_sortable_bytes(encoded_latitude.value(), packed, 0);
    NumericUtils::int_to_sortable_bytes(encoded_longitude.value(), packed, BYTES);
    return packed;
}

Result<std::vector<uint8_t>> LatLonPoint::pack_ceil(double latitude, double longitude) {
    auto encoded_latitude = GeoEncodingUtils::encode_latitude_ceil(latitude);
    if (!encoded_latitude.has_value()) {
        return std::unexpected(std::move(encoded_latitude.error()));
    }
    auto encoded_longitude = GeoEncodingUtils::encode_longitude_ceil(longitude);
    if (!encoded_longitude.has_value()) {
        return std::unexpected(std::move(encoded_longitude.error()));
    }
    std::vector<uint8_t> packed(DIMENSIONS * BYTES, 0);
    NumericUtils::int_to_sortable_bytes(encoded_latitude.value(), packed, 0);
    NumericUtils::int_to_sortable_bytes(encoded_longitude.value(), packed, BYTES);
    return packed;
}

Result<Field> LatLonPoint::create(std::string name, double latitude, double longitude) {
    auto packed = pack(latitude, longitude);
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

Result<std::shared_ptr<Query>> LatLonPoint::new_exact_query(std::string field, double latitude, double longitude) {
    auto packed = pack(latitude, longitude);
    if (!packed.has_value()) {
        return std::unexpected(std::move(packed.error()));
    }
    std::vector<std::vector<uint8_t>> points;
    points.push_back(std::move(packed.value()));

    auto query = PointInSetQuery::create(std::move(field), std::move(points), DIMENSIONS);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::make_shared<PointInSetQuery>(std::move(query.value())));
}

Result<std::shared_ptr<Query>> LatLonPoint::new_distance_query(std::string field, double latitude, double longitude,
                                                              double radius_meters) {
    auto query = LatLonPointDistanceQuery::create(std::move(field), latitude, longitude, radius_meters);
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::move(query.value()));
}

Result<std::shared_ptr<Query>> LatLonPoint::new_distance_feature_query(std::string field, float weight,
                                                                      double origin_lat, double origin_lon,
                                                                      double pivot_distance_meters) {
    auto created = LatLonPointDistanceFeatureQuery::create(std::move(field), origin_lat, origin_lon,
                                                           pivot_distance_meters);
    if (!created.has_value()) {
        return std::unexpected(std::move(created.error()));
    }
    std::shared_ptr<Query> query(std::move(created.value()));
    if (weight != 1.0F) {
        auto boosted = BoostQuery::create(std::move(query), weight);
        if (!boosted.has_value()) {
            return std::unexpected(std::move(boosted.error()));
        }
        return std::shared_ptr<Query>(std::make_shared<BoostQuery>(std::move(boosted.value())));
    }
    return query;
}

Result<std::shared_ptr<Query>> LatLonPoint::new_geometry_query(
        std::string field, QueryRelation query_relation, std::vector<std::shared_ptr<const Geometry>> geometries) {
    auto spatial = LatLonPointSpatial::create(geometries);
    if (!spatial.has_value()) {
        return std::unexpected(std::move(spatial.error()));
    }
    std::shared_ptr<const SpatialQueryBase> sub(std::move(spatial.value()));

    auto query = SpatialQuery::create(std::move(field), query_relation, std::move(geometries), std::move(sub));
    if (!query.has_value()) {
        return std::unexpected(std::move(query.error()));
    }
    return std::shared_ptr<Query>(std::move(query.value()));
}

LatLonPointSpatial::LatLonPointSpatial(std::shared_ptr<Component2D> component) : component_(std::move(component)) {}

Result<std::unique_ptr<LatLonPointSpatial>> LatLonPointSpatial::create(
        std::vector<std::shared_ptr<const Geometry>> geometries) {
    if (geometries.empty()) {
        return err(LuceneError::illegal_argument("geometries must not be empty"));
    }
    std::vector<const Geometry *> shapes;
    shapes.reserve(geometries.size());
    for (const auto &geometry : geometries) {
        if (geometry == nullptr) {
            return err(LuceneError::illegal_argument("the geometries must not be null"));
        }
        shapes.push_back(geometry.get());
    }
    auto component = create_geometry_component(shapes);
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }
    return std::unique_ptr<LatLonPointSpatial>(new LatLonPointSpatial(std::move(component.value())));
}

Result<std::unique_ptr<SpatialVisitor>> LatLonPointSpatial::spatial_visitor() const {
    auto predicate = create_component_predicate(component_);
    if (!predicate.has_value()) {
        return std::unexpected(std::move(predicate.error()));
    }

    // The bounding box of all geometries: it lets the tree walk prune whole cells
    // before the shape is consulted.
    auto min_lat = GeoEncodingUtils::encode_latitude(component_->min_y());
    if (!min_lat.has_value()) {
        return std::unexpected(std::move(min_lat.error()));
    }
    auto max_lat = GeoEncodingUtils::encode_latitude(component_->max_y());
    if (!max_lat.has_value()) {
        return std::unexpected(std::move(max_lat.error()));
    }
    auto min_lon = GeoEncodingUtils::encode_longitude(component_->min_x());
    if (!min_lon.has_value()) {
        return std::unexpected(std::move(min_lon.error()));
    }
    auto max_lon = GeoEncodingUtils::encode_longitude(component_->max_x());
    if (!max_lon.has_value()) {
        return std::unexpected(std::move(max_lon.error()));
    }

    return std::unique_ptr<SpatialVisitor>(std::make_unique<LatLonPointSpatialVisitor>(
            component_, std::move(predicate.value()), min_lat.value(), max_lat.value(), min_lon.value(),
            max_lon.value()));
}

} // namespace lucene
