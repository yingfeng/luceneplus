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

module lucene_core:lat_lon_point_sort_field.impl;

import :lat_lon_point_sort_field;

import std;

import :error;
import :field_comparator;
import :geo_encoding_utils;
import :geo_utils;
import :leaf_reader;
import :leaf_reader_context;
import :sloppy_math;
import :sort_field;
import :sorted_numeric_doc_values;
import :types;

namespace lucene {

namespace {

/// Sorts the hits of one leaf by distance.
class LeafDistanceComparator final : public LeafFieldComparator {
public:
    LeafDistanceComparator(double latitude, double longitude, std::vector<double> &values, double &bottom,
                           std::optional<double> &top)
            : latitude_(latitude), longitude_(longitude), values_(values), bottom_(bottom), top_(top) {}

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
            // The latitude is the high 32 bits and the longitude the low 32 bits;
            // decoding them here (rather than through the field module) keeps this
            // module independent of the field, which imports it for the
            // `new_distance_sort` facade.
            const auto packed = static_cast<uint64_t>(value.value());
            const double doc_latitude =
                    GeoEncodingUtils::decode_latitude(static_cast<int32_t>(packed >> 32U));
            const double doc_longitude =
                    GeoEncodingUtils::decode_longitude(static_cast<int32_t>(static_cast<uint32_t>(packed)));
            const double distance = SloppyMath::haversin_meters(latitude_, longitude_, doc_latitude, doc_longitude);
            closest = std::min(closest, distance);
        }
        return closest;
    }

public:
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
    double latitude_;
    double longitude_;
    std::vector<double> &values_;
    double &bottom_;
    std::optional<double> &top_;
    std::unique_ptr<SortedNumericDocValues> values_iterator_;
};

} // namespace

LatLonPointDistanceComparator::LatLonPointDistanceComparator(std::string field, double latitude, double longitude,
                                                             int32_t num_hits)
        : field_(std::move(field)),
          latitude_(latitude),
          longitude_(longitude),
          values_(static_cast<size_t>(std::max(num_hits, 0)), std::numeric_limits<double>::infinity()) {}

int32_t LatLonPointDistanceComparator::compare_distances(double first, double second) const {
    if (first < second) {
        return -1;
    }
    if (first > second) {
        return 1;
    }
    return 0;
}

int32_t LatLonPointDistanceComparator::compare(int32_t slot1, int32_t slot2) const {
    return compare_distances(values_[static_cast<size_t>(slot1)], values_[static_cast<size_t>(slot2)]);
}

int32_t LatLonPointDistanceComparator::compare_bottom(int32_t slot) const {
    return compare_distances(values_[static_cast<size_t>(slot)], bottom_);
}

int32_t LatLonPointDistanceComparator::compare_top(int32_t slot) const {
    if (!top_.has_value()) {
        return 0;
    }
    return compare_distances(values_[static_cast<size_t>(slot)], top_.value());
}

void LatLonPointDistanceComparator::set_bottom(int32_t slot) {
    bottom_ = values_[static_cast<size_t>(slot)];
}

void LatLonPointDistanceComparator::set_top_value(const SortValue &value) {
    if (const auto *as_double = std::get_if<double>(&value); as_double != nullptr) {
        top_ = *as_double;
    }
}

Result<SortValue> LatLonPointDistanceComparator::value(int32_t slot) const {
    return SortValue(values_[static_cast<size_t>(slot)]);
}

Result<std::unique_ptr<LeafFieldComparator>> LatLonPointDistanceComparator::get_leaf_comparator(
        const LeafReaderContext &context) {
    auto leaf = std::make_unique<LeafDistanceComparator>(latitude_, longitude_, values_, bottom_, top_);
    if (auto bound = leaf->bind(context, field_); !bound.has_value()) {
        return std::unexpected(std::move(bound.error()));
    }
    return std::unique_ptr<LeafFieldComparator>(std::move(leaf));
}

Result<int32_t> LatLonPointDistanceComparator::compare_values(const SortValue &first, const SortValue &second) const {
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

LatLonPointSortField::LatLonPointSortField(std::string field, double latitude, double longitude)
        : field_(std::move(field)), latitude_(latitude), longitude_(longitude) {}

Result<LatLonPointSortField> LatLonPointSortField::create(std::string field, double latitude, double longitude) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }
    if (auto checked = GeoUtils::check_latitude(latitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(longitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return LatLonPointSortField(std::move(field), latitude, longitude);
}

SortField LatLonPointSortField::to_sort_field() const {
    const std::string field = field_;
    const double latitude = latitude_;
    const double longitude = longitude_;
    auto factory = [field, latitude, longitude](int32_t num_hits) -> std::unique_ptr<FieldComparator> {
        return std::make_unique<LatLonPointDistanceComparator>(field, latitude, longitude, num_hits);
    };
    auto sort_field = SortField::with_custom(std::string(field), to_string(), std::move(factory));
    // `with_custom` only fails when the factory is empty, which cannot happen here.
    return sort_field.has_value() ? std::move(sort_field.value()) : SortField::doc().value();
}

std::string LatLonPointSortField::to_string() const {
    return "<distance:\"" + field_ + "\" latitude=" + std::format("{}", latitude_) +
           " longitude=" + std::format("{}", longitude_) + ">";
}

} // namespace lucene
