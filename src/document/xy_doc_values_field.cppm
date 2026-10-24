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

export module lucene_core:xy_doc_values_field;

import :types;
import std;

import :error;
import :indexable_field;
import :geo_geometry;
import :query;

namespace lucene {

/// A per-document cartesian location, stored as a sorted numeric doc value.
///
/// Port of `XYDocValuesField` (`core/document/xy_doc_values_field.rs`).
///
/// The packed value is the encoded `x` in the high 32 bits and the encoded `y` in
/// the low 32 bits, so a doc-values query can decode a document's location
/// without reading the point index.
///
/// **Deviation:** like [`LatLonDocValuesField`](:lat_lon_doc_values_field) the
/// reference implementation gives this field its own `FieldType`; the port stores
/// the value as a sorted numeric doc value, whose encoding is identical.
export struct XYDocValuesField {
    /// One coordinate takes 4 bytes of the packed value.
    static constexpr size_t BYTES = sizeof(int32_t);

    /// The packed value of `(x, y)`.
    [[nodiscard]] static Result<int64_t> pack(float x, float y);

    /// The `x` of a packed value.
    [[nodiscard]] static float decode_x(int64_t packed);

    /// The `y` of a packed value.
    [[nodiscard]] static float decode_y(int64_t packed);

    /// The doc values field for the location `(x, y)`.
    [[nodiscard]] static Result<Field> create(std::string name, float x, float y);

    /// Matches the locations inside the box `[min_x, max_x] x [min_y, max_y]`,
    /// read from the doc values.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_slow_box_query(std::string field, float min_x, float max_x,
                                                                          float min_y, float max_y);

    /// Matches the locations within `radius` of `(x, y)`, in cartesian units.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_slow_distance_query(std::string field, float x, float y,
                                                                               float radius);

    /// Matches the locations inside any of `geometries`, which must not be empty.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_slow_geometry_query(
            std::string field, std::vector<std::shared_ptr<const Geometry>> geometries);
};

} // namespace lucene
