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

module lucene_core:sort_field_provider.impl;

import :sort_field_provider;

import std;

import :data_input;
import :data_output;
import :error;
import :sort_field;

namespace lucene {

namespace {

/// The name of `type` in the segment metadata.
///
/// Port of `AsRef<str> for SortFieldType` (`core/search/sort_field.rs`).
[[nodiscard]] std::string_view type_name(SortFieldType type) noexcept {
    switch (type) {
        case SortFieldType::kScore:
            return "SCORE";
        case SortFieldType::kDoc:
            return "DOC";
        case SortFieldType::kString:
            return "STRING";
        case SortFieldType::kInt:
            return "INT";
        case SortFieldType::kFloat:
            return "FLOAT";
        case SortFieldType::kLong:
            return "LONG";
        case SortFieldType::kDouble:
            return "DOUBLE";
        case SortFieldType::kSortedNumeric:
            return "LONG";
        case SortFieldType::kSortedNumericDouble:
            return "DOUBLE";
        case SortFieldType::kSortedSet:
            return "STRING";
        case SortFieldType::kCustom:
            // A custom sort field computes its values per search, so it has no
            // name in the segment metadata.
            return "CUSTOM";
    }
    return "SCORE";
}

/// Parses a type name, the inverse of [`type_name`].
///
/// Port of `SortFieldType::value_of` (`core/search/sort_field.rs`).
[[nodiscard]] Result<SortFieldType> parse_type(std::string_view name) {
    if (name == "SCORE") {
        return SortFieldType::kScore;
    }
    if (name == "DOC") {
        return SortFieldType::kDoc;
    }
    if (name == "STRING") {
        return SortFieldType::kString;
    }
    if (name == "INT") {
        return SortFieldType::kInt;
    }
    if (name == "FLOAT") {
        return SortFieldType::kFloat;
    }
    if (name == "LONG") {
        return SortFieldType::kLong;
    }
    if (name == "DOUBLE") {
        return SortFieldType::kDouble;
    }
    return err(LuceneError::illegal_argument("Can't deserialize SortField - unknown type " + std::string(name)));
}

/// Consumes the missing value the metadata may carry after the "has missing
/// value" flag.  The port's [`SortField`] has no missing value, so the payload is
/// read and dropped.
[[nodiscard]] VoidResult skip_missing_value(SortFieldType type, DataInput &input) {
    switch (type) {
        case SortFieldType::kCustom:
            return err(LuceneError::illegal_argument("a custom sort field has no serialized form"));
        case SortFieldType::kString:
        case SortFieldType::kInt:
        case SortFieldType::kFloat: {
            if (auto read = input.read_int(); !read.has_value()) {
                return std::unexpected(std::move(read.error()));
            }
            return {};
        }
        case SortFieldType::kLong:
        case SortFieldType::kDouble: {
            if (auto read = input.read_long(); !read.has_value()) {
                return std::unexpected(std::move(read.error()));
            }
            return {};
        }
        case SortFieldType::kScore:
        case SortFieldType::kDoc:
        case SortFieldType::kSortedNumeric:
        case SortFieldType::kSortedNumericDouble:
        case SortFieldType::kSortedSet:
            break;
    }
    return err(LuceneError::illegal_argument("Cannot deserialize sort of type " + std::string(type_name(type))));
}

[[nodiscard]] Result<std::string> read_field_name(DataInput &input) {
    auto name = input.read_string();
    if (!name.has_value()) {
        return std::unexpected(std::move(name.error()));
    }
    return name;
}

[[nodiscard]] Result<bool> read_flag(DataInput &input) {
    auto flag = input.read_int();
    if (!flag.has_value()) {
        return std::unexpected(std::move(flag.error()));
    }
    return flag.value() == 1;
}

[[nodiscard]] Result<SortFieldSelector> read_selector(DataInput &input) {
    auto selector = input.read_int();
    if (!selector.has_value()) {
        return std::unexpected(std::move(selector.error()));
    }
    if (selector.value() == 0) {
        return SortFieldSelector::kMin;
    }
    if (selector.value() == 1) {
        return SortFieldSelector::kMax;
    }
    return err(LuceneError::illegal_argument("unknown sort field selector: " + std::to_string(selector.value())));
}

} // namespace

Result<std::string_view> sort_field_provider_name(const SortField &field) {
    switch (field.type()) {
        case SortFieldType::kCustom:
            return err(LuceneError::illegal_argument("a custom sort field cannot be written to the segment metadata"));
        case SortFieldType::kSortedNumeric:
        case SortFieldType::kSortedNumericDouble:
            return SORTED_NUMERIC_SORT_FIELD_PROVIDER;
        case SortFieldType::kSortedSet:
            return SORTED_SET_SORT_FIELD_PROVIDER;
        case SortFieldType::kScore:
        case SortFieldType::kDoc:
        case SortFieldType::kString:
        case SortFieldType::kInt:
        case SortFieldType::kFloat:
        case SortFieldType::kLong:
        case SortFieldType::kDouble:
            return SORT_FIELD_PROVIDER;
    }
    return SORT_FIELD_PROVIDER;
}

VoidResult write_sort_field(const SortField &field, DataOutput &output) {
    const SortFieldType type = field.type();
    if (type == SortFieldType::kScore || type == SortFieldType::kDoc || !field.field().has_value()) {
        auto text = field.to_string();
        return err(LuceneError::illegal_argument(
                "Cannot serialize sort field " + (text.has_value() ? text.value() : std::string("?")) +
                ": it has no sortable field"));
    }

    if (auto written = output.write_string(field.field().value()); !written.has_value()) {
        return written;
    }

    if (type == SortFieldType::kSortedSet) {
        // `SortedSetSortField` has no type name: its values are always strings.
        if (auto written = output.write_int(field.reverse() ? 1 : 0); !written.has_value()) {
            return written;
        }
        if (auto written = output.write_int(static_cast<int32_t>(field.selector())); !written.has_value()) {
            return written;
        }
        // 0 = no missing value.
        return output.write_int(0);
    }

    if (auto written = output.write_string(type_name(type)); !written.has_value()) {
        return written;
    }
    if (auto written = output.write_int(field.reverse() ? 1 : 0); !written.has_value()) {
        return written;
    }
    if (type == SortFieldType::kSortedNumeric || type == SortFieldType::kSortedNumericDouble) {
        if (auto written = output.write_int(static_cast<int32_t>(field.selector())); !written.has_value()) {
            return written;
        }
    }
    // 0 = no missing value.
    return output.write_int(0);
}

Result<SortField> read_sort_field(std::string_view provider_name, DataInput &input) {
    auto field_name = read_field_name(input);
    if (!field_name.has_value()) {
        return std::unexpected(std::move(field_name.error()));
    }

    if (provider_name == SORTED_SET_SORT_FIELD_PROVIDER) {
        auto reverse = read_flag(input);
        if (!reverse.has_value()) {
            return std::unexpected(std::move(reverse.error()));
        }
        auto selector = read_selector(input);
        if (!selector.has_value()) {
            return std::unexpected(std::move(selector.error()));
        }
        // 0 = no missing value, 1 = first, 2 = last; the port has no missing
        // values, so the code only has to be consumed.
        auto missing = input.read_int();
        if (!missing.has_value()) {
            return std::unexpected(std::move(missing.error()));
        }
        return SortField::with_reverse_selector(std::move(field_name.value()), SortFieldType::kSortedSet,
                                               selector.value(), reverse.value());
    }

    auto type_name_value = read_field_name(input);
    if (!type_name_value.has_value()) {
        return std::unexpected(std::move(type_name_value.error()));
    }
    auto type = parse_type(type_name_value.value());
    if (!type.has_value()) {
        return std::unexpected(std::move(type.error()));
    }
    auto reverse = read_flag(input);
    if (!reverse.has_value()) {
        return std::unexpected(std::move(reverse.error()));
    }

    if (provider_name == SORTED_NUMERIC_SORT_FIELD_PROVIDER) {
        // `SortedNumericSortField` names the type of its values; the port
        // collapses them into "integral" and "floating point" sorts.
        SortFieldType multi_type = SortFieldType::kSortedNumeric;
        switch (type.value()) {
            case SortFieldType::kCustom:
                return err(LuceneError::illegal_argument("a custom sort field has no serialized form"));
            case SortFieldType::kInt:
            case SortFieldType::kLong:
                multi_type = SortFieldType::kSortedNumeric;
                break;
            case SortFieldType::kFloat:
            case SortFieldType::kDouble:
                multi_type = SortFieldType::kSortedNumericDouble;
                break;
            case SortFieldType::kString:
            case SortFieldType::kScore:
            case SortFieldType::kDoc:
            case SortFieldType::kSortedNumeric:
            case SortFieldType::kSortedNumericDouble:
            case SortFieldType::kSortedSet:
                return err(LuceneError::illegal_argument(
                        "Cannot deserialize sorted numeric sort of type " + std::string(type_name(type.value()))));
        }
        auto selector = read_selector(input);
        if (!selector.has_value()) {
            return std::unexpected(std::move(selector.error()));
        }
        auto has_missing = read_flag(input);
        if (!has_missing.has_value()) {
            return std::unexpected(std::move(has_missing.error()));
        }
        if (has_missing.value()) {
            if (auto skipped = skip_missing_value(type.value(), input); !skipped.has_value()) {
                return std::unexpected(std::move(skipped.error()));
            }
        }
        return SortField::with_reverse_selector(std::move(field_name.value()), multi_type, selector.value(),
                                               reverse.value());
    }

    if (provider_name != SORT_FIELD_PROVIDER) {
        return err(LuceneError::illegal_argument("A SortFieldProvider with name '" + std::string(provider_name) +
                                                 "' does not exist"));
    }

    auto has_missing = read_flag(input);
    if (!has_missing.has_value()) {
        return std::unexpected(std::move(has_missing.error()));
    }
    if (has_missing.value()) {
        if (auto skipped = skip_missing_value(type.value(), input); !skipped.has_value()) {
            return std::unexpected(std::move(skipped.error()));
        }
    }
    return SortField::with_reverse(std::move(field_name.value()), type.value(), reverse.value());
}

} // namespace lucene
