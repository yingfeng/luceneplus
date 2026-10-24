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

export module lucene_core:xy_encoding_utils;

import :types;
import std;

import :error;

namespace lucene {

/// Quantizes cartesian coordinates into sortable `int32` values and back.
///
/// Port of `XYEncodingUtils` (`core/geo/xy_encoding_utils.rs`).
///
/// The quantization is the float-to-sortable-integer one, so the encoded values
/// order like the coordinates they came from — which is what lets the point index
/// prune on them.
export struct XYEncodingUtils {
    /// The smallest coordinate a point can hold.
    static constexpr double MIN_VAL_INCL = -static_cast<double>(std::numeric_limits<float>::max());
    /// The largest coordinate a point can hold.
    static constexpr double MAX_VAL_INCL = static_cast<double>(std::numeric_limits<float>::max());

    /// Fails unless `value` is finite.
    [[nodiscard]] static Result<float> check_val(float value);

    /// The sortable encoding of `value`.
    [[nodiscard]] static Result<int32_t> encode(float value);

    /// The coordinate [`encode`] put into `encoded`.
    [[nodiscard]] static float decode(int32_t encoded);

    /// The coordinate at `src[offset]`, as [`decode`] reads it.
    [[nodiscard]] static float decode_bytes(std::span<const uint8_t> src, size_t offset);

    /// A copy of `values` widened to `double`.
    [[nodiscard]] static std::vector<double> float_array_to_double_array(std::span<const float> values);
};

} // namespace lucene
