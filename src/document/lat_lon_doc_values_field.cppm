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

export module lucene_core:lat_lon_doc_values_field;

import :types;
import std;

import :error;
import :geo_geometry;
import :indexable_field;
import :lat_lon_point_sort_field;
import :query;
import :shape_field;

namespace lucene {

/// A per-document location, stored as a sorted numeric doc value.
///
/// Port of `LatLonDocValuesField` (`core/document/lat_lon_doc_values_field.rs`).
///
/// The packed value is the quantized latitude in the high 32 bits and the
/// quantized longitude in the low 32 bits, which is why a doc-values query can
/// decode a document's location without reading the point index.
///
/// **Deviation:** the reference implementation gives the field its own
/// `FieldType` so that a query can tell it apart from a plain numeric doc values
/// field; the port stores the value with
/// [`SortedNumericDocValuesField`](:doc_values_fields), whose encoding is
/// identical.
export struct LatLonDocValuesField {
    /// One coordinate takes 4 bytes of the packed value.
    static constexpr size_t BYTES = sizeof(int32_t);

    /// The packed value of `(latitude, longitude)`.
    [[nodiscard]] static Result<int64_t> pack(double latitude, double longitude);

    /// The latitude of a packed value.
    [[nodiscard]] static double decode_latitude(int64_t packed);

    /// The longitude of a packed value.
    [[nodiscard]] static double decode_longitude(int64_t packed);

    /// The doc values field for the location `(latitude, longitude)`.
    [[nodiscard]] static Result<Field> create(std::string name, double latitude, double longitude);

    /// The sort field that orders documents by their distance to
    /// `(latitude, longitude)`.
    [[nodiscard]] static Result<LatLonPointSortField> new_distance_sort(std::string field, double latitude,
                                                                       double longitude);

    /// Matches the documents whose location is within `radius_meters` of
    /// `(latitude, longitude)`, read from the doc values.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_slow_distance_query(std::string field, double latitude,
                                                                               double longitude, double radius_meters);

    /// Matches the documents whose location satisfies `query_relation` with any of
    /// `geometries`, read from the doc values.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_slow_geometry_query(
            std::string field, QueryRelation query_relation,
            std::vector<std::shared_ptr<const Geometry>> geometries);

    /// Matches the documents whose location is inside the box, read from the doc
    /// values instead of the point index.
    ///
    /// This is the "slow" box query: it does not need the field to be indexed as
    /// points, but it cannot prune the way the point query does.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_slow_box_query(std::string field, double min_latitude,
                                                                          double max_latitude, double min_longitude,
                                                                          double max_longitude);
};

} // namespace lucene
