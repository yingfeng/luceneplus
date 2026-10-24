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

module lucene_core:xy_point_sort_field.impl;

import :xy_point_sort_field;

import std;

import :error;
import :field_comparator;
import :leaf_reader;
import :leaf_reader_context;
import :sort_field;
import :sorted_numeric_doc_values;
import :types;
import :xy_doc_values_field;

namespace lucene {

namespace {

/// Sorts the hits of one leaf by cartesian distance.
class LeafXYDistanceComparator final : public LeafFieldComparator {
public:
    LeafXYDistanceComparator(float x, float y, std::vector<double> &values, double &bottom, std::optional<double> &top)
            : x_(x), y_(y), values_(values), bottom_(bottom), top_(top) {}

    void set_bottom(int32_t slot) override { bottom_ = values_[static_cast<size_t>(slot)]; }

    void set_top_value(const SortValue &value) override {
        if (const auto *as_double = std::get_if<double>(&value); as_double != nullptr) {
            top_ = *as_double;
        }
    }

    [[nodiscard]] Result<int32_t> compare_bottom(int32_t doc) override {
        auto distance = distance_of(doc);
        if (!distance.has_value()) {
            return std::unexpected(std::move(distance.error()));
        }
        return compare_to(distance.value(), bottom_);
    }

    [[nodiscard]] Result<int32_t> compare_top(int32_t doc) override {
        if (!top_.has_value()) {
            return 0;
        }
        auto distance = distance_of(doc);
        if (!distance.has_value()) {
            return std::unexpected(std::move(distance.error()));
        }
        return compare_to(distance.value(), top_.value());
    }

    [[nodiscard]] VoidResult copy(int32_t slot, int32_t doc) override {
        auto distance = distance_of(doc);
        if (!distance.has_value()) {
            return std::unexpected(std::move(distance.error()));
        }
        values_[static_cast<size_t>(slot)] = distance.value();
        return {};
    }

    /// Wires the doc values of `context` into the comparator.
    [[nodiscard]] VoidResult bind(const LeafReaderContext &context, std::string_view field) {
        auto values = context.reader()->get_sorted_numeric_doc_values(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        values_iterator_ = std::move(values.value());
        return {};
    }

private:
    /// `> 0` when `first` sorts after `second`.
    [[nodiscard]] static int32_t compare_to(double first, double second) {
        if (first < second) {
            return -1;
        }
        if (first > second) {
            return 1;
        }
        return 0;
    }

    /// The distance of `doc`, or infinity when it has no value.
    [[nodiscard]] Result<double> distance_of(int32_t doc) {
        if (values_iterator_ == nullptr) {
            return std::numeric_limits<double>::infinity();
        }
        auto advanced = values_iterator_->advance(doc);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (advanced.value() != doc) {
            return std::numeric_limits<double>::infinity();
        }
        auto count = values_iterator_->doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }

        double closest = std::numeric_limits<double>::infinity();
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values_iterator_->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            const double diff_x = static_cast<double>(x_) - static_cast<double>(XYDocValuesField::decode_x(value.value()));
            const double diff_y = static_cast<double>(y_) - static_cast<double>(XYDocValuesField::decode_y(value.value()));
            closest = std::min(closest, std::sqrt(diff_x * diff_x + diff_y * diff_y));
        }
        return closest;
    }

    float x_;
    float y_;
    std::vector<double> &values_;
    double &bottom_;
    std::optional<double> &top_;
    std::unique_ptr<SortedNumericDocValues> values_iterator_;
};

} // namespace

XYPointDistanceComparator::XYPointDistanceComparator(std::string field, float x, float y, int32_t num_hits)
        : field_(std::move(field)),
          x_(x),
          y_(y),
          values_(static_cast<size_t>(std::max(num_hits, 0)), std::numeric_limits<double>::infinity()) {}

int32_t XYPointDistanceComparator::compare_distances(double first, double second) {
    if (first < second) {
        return -1;
    }
    if (first > second) {
        return 1;
    }
    return 0;
}

int32_t XYPointDistanceComparator::compare(int32_t slot1, int32_t slot2) const {
    return compare_distances(values_[static_cast<size_t>(slot1)], values_[static_cast<size_t>(slot2)]);
}

int32_t XYPointDistanceComparator::compare_bottom(int32_t slot) const {
    return compare_distances(values_[static_cast<size_t>(slot)], bottom_);
}

int32_t XYPointDistanceComparator::compare_top(int32_t slot) const {
    if (!top_.has_value()) {
        return 0;
    }
    return compare_distances(values_[static_cast<size_t>(slot)], top_.value());
}

void XYPointDistanceComparator::set_bottom(int32_t slot) { bottom_ = values_[static_cast<size_t>(slot)]; }

void XYPointDistanceComparator::set_top_value(const SortValue &value) {
    if (const auto *as_double = std::get_if<double>(&value); as_double != nullptr) {
        top_ = *as_double;
    }
}

Result<SortValue> XYPointDistanceComparator::value(int32_t slot) const {
    return SortValue(values_[static_cast<size_t>(slot)]);
}

Result<std::unique_ptr<LeafFieldComparator>> XYPointDistanceComparator::get_leaf_comparator(
        const LeafReaderContext &context) {
    auto leaf = std::make_unique<LeafXYDistanceComparator>(x_, y_, values_, bottom_, top_);
    if (auto bound = leaf->bind(context, field_); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
    }
    return std::unique_ptr<LeafFieldComparator>(std::move(leaf));
}

Result<int32_t> XYPointDistanceComparator::compare_values(const SortValue &first, const SortValue &second) const {
    const auto *first_double = std::get_if<double>(&first);
    const auto *second_double = std::get_if<double>(&second);
    if (first_double == nullptr || second_double == nullptr) {
        // A missing value sorts after every real one.
        if (std::holds_alternative<SortValueMissing>(first) && std::holds_alternative<SortValueMissing>(second)) {
            return 0;
        }
        return std::holds_alternative<SortValueMissing>(first) ? 1 : -1;
    }
    return compare_distances(*first_double, *second_double);
}

XYPointSortField::XYPointSortField(std::string field, float x, float y)
        : field_(std::move(field)), x_(x), y_(y) {}

Result<XYPointSortField> XYPointSortField::create(std::string field, float x, float y) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return err(LuceneError::illegal_argument("the origin must be finite, got (" + std::to_string(x) + ", " +
                                                 std::to_string(y) + ")"));
    }
    return XYPointSortField(std::move(field), x, y);
}

SortField XYPointSortField::to_sort_field() const {
    const std::string field = field_;
    const float x = x_;
    const float y = y_;
    auto factory = [field, x, y](int32_t num_hits) -> std::unique_ptr<FieldComparator> {
        return std::make_unique<XYPointDistanceComparator>(field, x, y, num_hits);
    };
    auto sort_field = SortField::with_custom(std::string(field), to_string(), std::move(factory));
    // `with_custom` only fails when the factory is empty, which cannot happen here.
    return sort_field.has_value() ? std::move(sort_field.value()) : SortField::doc().value();
}

std::string XYPointSortField::to_string() const {
    return "<distance:\"" + field_ + "\" x=" + std::format("{}", x_) + " y=" + std::format("{}", y_) + ">";
}

} // namespace lucene
