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

module lucene_core:shape_field.impl;

import :shape_field;

import std;

import :error;
import :geo_utils;
import :numeric_utils;
import :types;

namespace lucene {

namespace {

/// The eight ways a triangle's six distinct values can be laid out, from west to
/// east: which of the six slots holds which coordinate.
///
/// Port of the `MINY_MINX_...` constants of `core/document/shape_field.rs`.
constexpr int32_t kMinyMinxMaxyMaxxYX = 0;
constexpr int32_t kMinyMinxYXMaxyMaxx = 1;
constexpr int32_t kMaxyMinxYXMinyMaxx = 2;
constexpr int32_t kMaxyMinxMinyMaxxYX = 3;
constexpr int32_t kYMinyMinxXMaxyMaxx = 4;
constexpr int32_t kYMinyMinxMaxxMaxyX = 5;
constexpr int32_t kMaxyMinxMinyXYMaxx = 6;
constexpr int32_t kMinyMinxYMaxxMaxyX = 7;

[[nodiscard]] int32_t read_coordinate(std::span<const uint8_t> bytes, size_t slot) {
    return NumericUtils::sortable_bytes_to_int(bytes, slot * SHAPE_COORDINATE_BYTES);
}

} // namespace

std::string_view query_relation_to_string(QueryRelation relation) noexcept {
    switch (relation) {
        case QueryRelation::Intersects:
            return "Intersects";
        case QueryRelation::Within:
            return "Within";
        case QueryRelation::Disjoint:
            return "Disjoint";
        case QueryRelation::Contains:
            return "Contains";
    }
    return "Intersects";
}

bool DecodedTriangle::equals(const DecodedTriangle &other) const noexcept {
    return a_x == other.a_x && b_x == other.b_x && c_x == other.c_x && a_y == other.a_y && b_y == other.b_y &&
           c_y == other.c_y && ab == other.ab && bc == other.bc && ca == other.ca;
}

size_t DecodedTriangle::hash() const noexcept {
    size_t result = static_cast<size_t>(a_x);
    for (const int32_t value : {a_y, b_x, b_y, c_x, c_y}) {
        result = result * 31 + static_cast<size_t>(value);
    }
    result = result * 31 + static_cast<size_t>(ab);
    result = result * 31 + static_cast<size_t>(bc);
    result = result * 31 + static_cast<size_t>(ca);
    return result;
}

std::string DecodedTriangle::to_string() const {
    return std::format("{}, {} {}, {} {}, {} [{},{},{}]", a_x, a_y, b_x, b_y, c_x, c_y, ab, bc, ca);
}

VoidResult encode_triangle(std::span<uint8_t> bytes, int32_t a_y, int32_t a_x, bool ab, int32_t b_y, int32_t b_x,
                           bool bc, int32_t c_y, int32_t c_x, bool ca) {
    if (bytes.size() != SHAPE_TRIANGLE_BYTES) {
        return err(LuceneError::illegal_argument("the buffer must hold " + std::to_string(SHAPE_TRIANGLE_BYTES) +
                                                 " bytes, got " + std::to_string(bytes.size())));
    }

    // Rotate the vertices so that `a` is the westernmost one.
    if (b_x < a_x || c_x < a_x) {
        const int32_t temp_x = a_x;
        const int32_t temp_y = a_y;
        const bool temp_edge = ab;
        if (b_x < c_x) {
            a_x = b_x;
            a_y = b_y;
            ab = bc;
            b_x = c_x;
            b_y = c_y;
            bc = ca;
            c_x = temp_x;
            c_y = temp_y;
            ca = temp_edge;
        } else {
            a_x = c_x;
            a_y = c_y;
            ab = ca;
            c_x = b_x;
            c_y = b_y;
            ca = bc;
            b_x = temp_x;
            b_y = temp_y;
            bc = temp_edge;
        }
    } else if (a_x == b_x && a_x == c_x && (b_y < a_y || c_y < a_y)) {
        // A vertical triangle: put the southernmost vertex first.
        const int32_t temp_x = a_x;
        const int32_t temp_y = a_y;
        const bool temp_edge = ab;
        if (b_y < c_y) {
            a_x = b_x;
            a_y = b_y;
            ab = bc;
            b_x = c_x;
            b_y = c_y;
            bc = ca;
            c_x = temp_x;
            c_y = temp_y;
            ca = temp_edge;
        } else {
            a_x = c_x;
            a_y = c_y;
            ab = ca;
            c_x = b_x;
            c_y = b_y;
            ca = bc;
            b_x = temp_x;
            b_y = temp_y;
            bc = temp_edge;
        }
    }

    // Triangles are stored counter-clockwise.
    if (GeoUtils::orient(static_cast<double>(a_x), static_cast<double>(a_y), static_cast<double>(b_x),
                         static_cast<double>(b_y), static_cast<double>(c_x), static_cast<double>(c_y)) == -1) {
        const int32_t temp_x = b_x;
        const int32_t temp_y = b_y;
        const bool temp_edge = ab;
        ab = bc;
        b_x = c_x;
        b_y = c_y;
        c_x = temp_x;
        c_y = temp_y;
        ca = temp_edge;
    }

    const int32_t min_x = a_x;
    const int32_t min_y = std::min(a_y, std::min(b_y, c_y));
    const int32_t max_x = std::max(a_x, std::max(b_x, c_x));
    const int32_t max_y = std::max(a_y, std::max(b_y, c_y));

    int32_t bits = 0;
    int32_t x = 0;
    int32_t y = 0;
    if (min_y == a_y) {
        if (max_y == b_y && max_x == b_x) {
            y = c_y;
            x = c_x;
            bits = kMinyMinxMaxyMaxxYX;
        } else if (max_y == c_y && max_x == c_x) {
            y = b_y;
            x = b_x;
            bits = kMinyMinxYXMaxyMaxx;
        } else {
            y = b_y;
            x = c_x;
            bits = kMinyMinxYMaxxMaxyX;
        }
    } else if (max_y == a_y) {
        if (min_y == b_y && max_x == b_x) {
            y = c_y;
            x = c_x;
            bits = kMaxyMinxMinyMaxxYX;
        } else if (min_y == c_y && max_x == c_x) {
            y = b_y;
            x = b_x;
            bits = kMaxyMinxYXMinyMaxx;
        } else {
            y = c_y;
            x = b_x;
            bits = kMaxyMinxMinyXYMaxx;
        }
    } else if (max_x == b_x && min_y == b_y) {
        y = a_y;
        x = c_x;
        bits = kYMinyMinxMaxxMaxyX;
    } else if (max_x == c_x && max_y == c_y) {
        y = a_y;
        x = b_x;
        bits = kYMinyMinxXMaxyMaxx;
    } else {
        return err(LuceneError::illegal_argument("Could not encode the provided triangle"));
    }

    if (ab) {
        bits |= 1 << 3;
    }
    if (bc) {
        bits |= 1 << 4;
    }
    if (ca) {
        bits |= 1 << 5;
    }

    NumericUtils::int_to_sortable_bytes(min_y, bytes, 0);
    NumericUtils::int_to_sortable_bytes(min_x, bytes, SHAPE_COORDINATE_BYTES);
    NumericUtils::int_to_sortable_bytes(max_y, bytes, 2 * SHAPE_COORDINATE_BYTES);
    NumericUtils::int_to_sortable_bytes(max_x, bytes, 3 * SHAPE_COORDINATE_BYTES);
    NumericUtils::int_to_sortable_bytes(y, bytes, 4 * SHAPE_COORDINATE_BYTES);
    NumericUtils::int_to_sortable_bytes(x, bytes, 5 * SHAPE_COORDINATE_BYTES);
    NumericUtils::int_to_sortable_bytes(bits, bytes, 6 * SHAPE_COORDINATE_BYTES);
    return {};
}

VoidResult decode_triangle(std::span<const uint8_t> bytes, DecodedTriangle &triangle) {
    if (bytes.size() != SHAPE_TRIANGLE_BYTES) {
        return err(LuceneError::illegal_argument("the buffer must hold " + std::to_string(SHAPE_TRIANGLE_BYTES) +
                                                 " bytes, got " + std::to_string(bytes.size())));
    }

    const int32_t bits = read_coordinate(bytes, 6);
    const int32_t code = ((1 << 3) - 1) & bits;

    int32_t a_x = 0;
    int32_t a_y = 0;
    int32_t b_x = 0;
    int32_t b_y = 0;
    int32_t c_x = 0;
    int32_t c_y = 0;
    switch (code) {
        case kMinyMinxMaxyMaxxYX:
            a_x = read_coordinate(bytes, 1);
            a_y = read_coordinate(bytes, 0);
            b_x = read_coordinate(bytes, 3);
            b_y = read_coordinate(bytes, 2);
            c_x = read_coordinate(bytes, 5);
            c_y = read_coordinate(bytes, 4);
            break;
        case kMinyMinxYXMaxyMaxx:
            a_x = read_coordinate(bytes, 1);
            a_y = read_coordinate(bytes, 0);
            b_x = read_coordinate(bytes, 5);
            b_y = read_coordinate(bytes, 4);
            c_x = read_coordinate(bytes, 3);
            c_y = read_coordinate(bytes, 2);
            break;
        case kMaxyMinxYXMinyMaxx:
            a_x = read_coordinate(bytes, 1);
            a_y = read_coordinate(bytes, 2);
            b_x = read_coordinate(bytes, 5);
            b_y = read_coordinate(bytes, 4);
            c_x = read_coordinate(bytes, 3);
            c_y = read_coordinate(bytes, 0);
            break;
        case kMaxyMinxMinyMaxxYX:
            a_x = read_coordinate(bytes, 1);
            a_y = read_coordinate(bytes, 2);
            b_x = read_coordinate(bytes, 3);
            b_y = read_coordinate(bytes, 0);
            c_x = read_coordinate(bytes, 5);
            c_y = read_coordinate(bytes, 4);
            break;
        case kYMinyMinxXMaxyMaxx:
            a_x = read_coordinate(bytes, 1);
            a_y = read_coordinate(bytes, 4);
            b_x = read_coordinate(bytes, 5);
            b_y = read_coordinate(bytes, 0);
            c_x = read_coordinate(bytes, 3);
            c_y = read_coordinate(bytes, 2);
            break;
        case kYMinyMinxMaxxMaxyX:
            a_x = read_coordinate(bytes, 1);
            a_y = read_coordinate(bytes, 4);
            b_x = read_coordinate(bytes, 3);
            b_y = read_coordinate(bytes, 0);
            c_x = read_coordinate(bytes, 5);
            c_y = read_coordinate(bytes, 2);
            break;
        case kMaxyMinxMinyXYMaxx:
            a_x = read_coordinate(bytes, 1);
            a_y = read_coordinate(bytes, 2);
            b_x = read_coordinate(bytes, 5);
            b_y = read_coordinate(bytes, 0);
            c_x = read_coordinate(bytes, 3);
            c_y = read_coordinate(bytes, 4);
            break;
        case kMinyMinxYMaxxMaxyX:
            a_x = read_coordinate(bytes, 1);
            a_y = read_coordinate(bytes, 0);
            b_x = read_coordinate(bytes, 3);
            b_y = read_coordinate(bytes, 4);
            c_x = read_coordinate(bytes, 5);
            c_y = read_coordinate(bytes, 2);
            break;
        default:
            return err(LuceneError::illegal_argument("Could not decode the provided triangle"));
    }

    const bool ab = (bits & (1 << 3)) == (1 << 3);
    const bool bc = (bits & (1 << 4)) == (1 << 4);
    const bool ca = (bits & (1 << 5)) == (1 << 5);

    triangle.a_x = a_x;
    triangle.a_y = a_y;
    triangle.ab = ab;
    triangle.b_x = b_x;
    triangle.b_y = b_y;
    triangle.bc = bc;
    triangle.c_x = c_x;
    triangle.c_y = c_y;
    triangle.ca = ca;
    resolve_triangle_type(triangle);
    return {};
}

void resolve_triangle_type(DecodedTriangle &triangle) {
    if (triangle.a_x == triangle.b_x && triangle.a_y == triangle.b_y) {
        if (triangle.a_x == triangle.c_x && triangle.a_y == triangle.c_y) {
            triangle.type = DecodedTriangleType::Point;
        } else {
            triangle.ab = triangle.bc || triangle.ca;
            triangle.b_x = triangle.c_x;
            triangle.b_y = triangle.c_y;
            triangle.c_x = triangle.a_x;
            triangle.c_y = triangle.a_y;
            triangle.type = DecodedTriangleType::Line;
        }
    } else if (triangle.a_x == triangle.c_x && triangle.a_y == triangle.c_y) {
        triangle.ab = triangle.ab || triangle.bc;
        triangle.type = DecodedTriangleType::Line;
    } else if (triangle.b_x == triangle.c_x && triangle.b_y == triangle.c_y) {
        triangle.ab = triangle.ab || triangle.ca;
        triangle.c_x = triangle.a_x;
        triangle.c_y = triangle.a_y;
        triangle.type = DecodedTriangleType::Line;
    } else {
        triangle.type = DecodedTriangleType::Triangle;
    }
}

} // namespace lucene
