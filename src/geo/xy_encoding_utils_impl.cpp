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

module lucene_core:xy_encoding_utils.impl;

import :xy_encoding_utils;

import std;

import :error;
import :numeric_utils;
import :types;

namespace lucene {

Result<float> XYEncodingUtils::check_val(float value) {
    if (!std::isfinite(value)) {
        return err(LuceneError::illegal_argument("invalid value " + std::to_string(value) + "; must be between " +
                                                 std::to_string(MIN_VAL_INCL) + " and " +
                                                 std::to_string(MAX_VAL_INCL)));
    }
    return value;
}

Result<int32_t> XYEncodingUtils::encode(float value) {
    auto checked = check_val(value);
    if (!checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return NumericUtils::float_to_sortable_int(checked.value());
}

float XYEncodingUtils::decode(int32_t encoded) { return NumericUtils::sortable_int_to_float(encoded); }

float XYEncodingUtils::decode_bytes(std::span<const uint8_t> src, size_t offset) {
    return decode(NumericUtils::sortable_bytes_to_int(src, offset));
}

std::vector<double> XYEncodingUtils::float_array_to_double_array(std::span<const float> values) {
    std::vector<double> widened;
    widened.reserve(values.size());
    for (const float value : values) {
        widened.push_back(static_cast<double>(value));
    }
    return widened;
}

} // namespace lucene
