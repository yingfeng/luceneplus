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

export module lucene_core:shape_field;

import :types;
import std;

import :error;

namespace lucene {

/// How a query shape has to relate to the indexed shape to match.
///
/// Port of `QueryRelation` (`core/document/shape_field.rs`).
export enum class QueryRelation : uint8_t {
    /// The shapes have to intersect.
    Intersects = 0,
    /// The indexed shape has to lie inside the query shape.
    Within = 1,
    /// The shapes have to be disjoint.
    Disjoint = 2,
    /// The indexed shape has to contain the query shape.
    Contains = 3,
};

/// The name of a [`QueryRelation`].
[[nodiscard]] std::string_view query_relation_to_string(QueryRelation relation) noexcept;

/// How many bytes one coordinate of an encoded triangle takes.
///
/// Port of `ShapeField::BYTES`.
export inline constexpr size_t SHAPE_COORDINATE_BYTES = 4;

/// How many bytes an encoded triangle takes: six coordinates and the bit field.
export inline constexpr size_t SHAPE_TRIANGLE_BYTES = 7 * SHAPE_COORDINATE_BYTES;

/// What the three vertices of a decoded triangle actually are.
///
/// Port of `DecodedTriangleType` (`core/document/shape_field.rs`).
export enum class DecodedTriangleType : uint8_t {
    /// All three vertices are the same point.
    Point = 0,
    /// Two of the vertices are the same, so the triangle is a line.
    Line = 1,
    /// Three distinct vertices.
    Triangle = 2,
};

/// A triangle decoded from a shape field.
///
/// Port of `DecodedTriangle` (`core/document/shape_field.rs`).  The coordinates
/// are the quantized ones the index stores; the `ab`/`bc`/`ca` flags say which
/// edges belong to the original shape rather than to the tessellation.
export struct DecodedTriangle {
    int32_t a_x{0};
    int32_t a_y{0};
    int32_t b_x{0};
    int32_t b_y{0};
    int32_t c_x{0};
    int32_t c_y{0};
    bool ab{true};
    bool bc{true};
    bool ca{true};
    DecodedTriangleType type{DecodedTriangleType::Triangle};

    [[nodiscard]] bool equals(const DecodedTriangle &other) const noexcept;
    [[nodiscard]] size_t hash() const noexcept;
    [[nodiscard]] std::string to_string() const;
};

/// Encodes a triangle into `bytes`, which must hold [`SHAPE_TRIANGLE_BYTES`].
///
/// Port of `encode_triangle` (`core/document/shape_field.rs`).
///
/// Triangles are stored with a counter-clockwise orientation, rotated so that the
/// number of possible reconstructions fits in three bits, and always read back
/// from west to east.
[[nodiscard]] VoidResult encode_triangle(std::span<uint8_t> bytes, int32_t a_y, int32_t a_x, bool ab, int32_t b_y,
                                        int32_t b_x, bool bc, int32_t c_y, int32_t c_x, bool ca);

/// The triangle encoded in `bytes`.
///
/// Port of `decode_triangle` (`core/document/shape_field.rs`).
[[nodiscard]] VoidResult decode_triangle(std::span<const uint8_t> bytes, DecodedTriangle &triangle);

/// Fills in the type of `triangle` (and normalizes the vertices of a line).
///
/// Port of `resolve_triangle_type` (`core/document/shape_field.rs`).
export void resolve_triangle_type(DecodedTriangle &triangle);

} // namespace lucene
