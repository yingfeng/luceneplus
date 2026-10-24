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

export module lucene_core:lat_lon_point;

import :types;
import std;

import :error;
import :indexable_field;
import :geo_component2d;
import :geo_geometry;
import :lat_lon_point_distance_feature_query;
import :lat_lon_point_distance_query;
import :query;
import :shape_field;
import :spatial_query;

namespace lucene {

/// An indexed location field: a latitude/longitude pair, stored as a
/// two-dimensional point.
///
/// Port of `LatLonPoint` (`core/document/lat_lon_point.rs`).
///
/// Values are quantized to `int32` per dimension (about 4.2E-8 degrees for the
/// latitude and 8.4E-8 for the longitude), so a query and an indexed point agree on
/// where they are.  A document may have several `LatLonPoint` fields with the same
/// name.
///
/// **Deviations:** the reference implementation wraps a mutable parent field and
/// offers `set_location_value`; the port returns the finished [`Field`] from
/// [`create`].  `new_box_query` is not ported yet.
export struct LatLonPoint {
    /// [`LatLonPoint`] is encoded as integer values, so a dimension takes 4 bytes.
    static constexpr size_t BYTES = sizeof(int32_t);
    /// How many dimensions a point has.
    static constexpr size_t DIMENSIONS = 2;

    /// The indexed field for the point at `(latitude, longitude)`.
    [[nodiscard]] static Result<Field> create(std::string name, double latitude, double longitude);

    /// The packed form the point index stores: the latitude, then the longitude,
    /// both as sortable `int32`.
    [[nodiscard]] static Result<std::vector<uint8_t>> pack(double latitude, double longitude);

    /// [`pack`], rounding both coordinates up.
    [[nodiscard]] static Result<std::vector<uint8_t>> pack_ceil(double latitude, double longitude);

    /// Matches the documents whose point is exactly `(latitude, longitude)`.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_exact_query(std::string field, double latitude,
                                                                        double longitude);

    /// Matches the documents whose point is within `radius_meters` of
    /// `(latitude, longitude)`.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_distance_query(std::string field, double latitude,
                                                                          double longitude, double radius_meters);

    /// Boosts the documents near `(origin_lat, origin_lon)` by
    /// `weight * pivot / (pivot + distance)`.
    ///
    /// A weight other than `1` wraps the query in a
    /// [`BoostQuery`](:boost_query), like the reference implementation does.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_distance_feature_query(std::string field, float weight,
                                                                                   double origin_lat, double origin_lon,
                                                                                   double pivot_distance_meters);

    /// Matches the documents whose point satisfies `query_relation` with at least
    /// one of `geometries`.
    ///
    /// **Deviation:** the reference implementation rejects line geometries for
    /// `Within` and non-point geometries for `Contains`; the port's [`Geometry`] has
    /// no kind tag yet, so those combinations are not rejected.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_geometry_query(
            std::string field, QueryRelation query_relation,
            std::vector<std::shared_ptr<const Geometry>> geometries);
};

/// The [`SpatialQueryBase`] of a query over a [`LatLonPoint`] field: it answers the
/// geometry questions with the lat/lon predicates of the query shapes.
///
/// Port of `LatLonPointSpatial` / `SpatialVisitorImpl`
/// (`core/document/lat_lon_point_query.rs`).
export class LatLonPointSpatial final : public SpatialQueryBase {
public:
    /// Builds the component of `geometries`, which may be several shapes.
    [[nodiscard]] static Result<std::unique_ptr<LatLonPointSpatial>> create(
            std::vector<std::shared_ptr<const Geometry>> geometries);

    [[nodiscard]] Result<std::unique_ptr<SpatialVisitor>> spatial_visitor() const override;

private:
    explicit LatLonPointSpatial(std::shared_ptr<Component2D> component);

    std::shared_ptr<Component2D> component_;
};

} // namespace lucene
