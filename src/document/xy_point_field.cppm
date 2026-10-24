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

export module lucene_core:xy_point_field;

import :types;
import std;

import :error;
import :indexable_field;
import :geo_geometry;
import :query;

namespace lucene {

/// An indexed cartesian point field: an `x`/`y` pair, stored as a
/// two-dimensional point.
///
/// Port of `XYPointField` (`core/document/xy_point_field.rs`).
///
/// The coordinates are quantized to `int32` through [`XYEncodingUtils`]
/// (`:xy_encoding_utils`), so a query and an indexed point agree on where they
/// are.
///
/// **Deviations:** the reference implementation wraps a mutable parent field and
/// offers `set_location_value`; the port returns the finished [`Field`] from
/// [`create`].  The geometry factories take plain [`Geometry`] pointers instead
/// of a dedicated `XYGeometry` enum, because the port has no such enum.
export struct XYPointField {
    /// A coordinate takes 4 bytes.
    static constexpr size_t BYTES = sizeof(int32_t);
    /// How many dimensions a point has.
    static constexpr size_t DIMENSIONS = 2;

    /// The indexed field for the point at `(x, y)`.
    [[nodiscard]] static Result<Field> create(std::string name, float x, float y);

    /// The packed form the point index stores: `x`, then `y`, both as sortable
    /// `int32`.
    [[nodiscard]] static Result<std::vector<uint8_t>> pack(float x, float y);

    /// The `x` of a packed point.
    [[nodiscard]] static float decode_x(std::span<const uint8_t> packed);

    /// The `y` of a packed point.
    [[nodiscard]] static float decode_y(std::span<const uint8_t> packed);

    /// Matches the points inside the box `[min_x, max_x] x [min_y, max_y]`.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_box_query(std::string field, float min_x, float max_x,
                                                                     float min_y, float max_y);

    /// Matches the points within `radius` of `(x, y)`, in cartesian units.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_distance_query(std::string field, float x, float y,
                                                                          float radius);

    /// Matches the points inside any of `geometries`, which must not be empty.
    [[nodiscard]] static Result<std::shared_ptr<Query>> new_geometry_query(
            std::string field, std::vector<std::shared_ptr<const Geometry>> geometries);
};

} // namespace lucene
