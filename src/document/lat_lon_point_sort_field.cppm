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

export module lucene_core:lat_lon_point_sort_field;

import :types;
import std;

import :error;
import :field_comparator;
import :sort_field;

namespace lucene {

/// Sorts the hits of one leaf by their distance to a centre.
///
/// Port of `LatLonPointDistanceComparator`
/// (`core/document/lat_lon_point_distance_comparator.rs`).
///
/// The distance comes from the [`LatLonDocValuesField`](:lat_lon_doc_values_field)
/// values of the document: a document with several values sorts by the closest
/// one, and a document without a value sorts last.
export class LatLonPointDistanceComparator final : public FieldComparator {
public:
    /// A comparator over `num_hits` slots for the values of `field`.
    LatLonPointDistanceComparator(std::string field, double latitude, double longitude, int32_t num_hits);

    [[nodiscard]] int32_t compare(int32_t slot1, int32_t slot2) const override;
    [[nodiscard]] int32_t compare_bottom(int32_t slot) const override;
    [[nodiscard]] int32_t compare_top(int32_t slot) const override;
    void set_bottom(int32_t slot) override;
    void set_top_value(const SortValue &value) override;
    [[nodiscard]] Result<SortValue> value(int32_t slot) const override;
    [[nodiscard]] Result<std::unique_ptr<LeafFieldComparator>>
    get_leaf_comparator(const LeafReaderContext &context) override;
    [[nodiscard]] Result<int32_t> compare_values(const SortValue &first, const SortValue &second) const override;

private:
    [[nodiscard]] int32_t compare_distances(double first, double second) const;

    std::string field_;
    double latitude_;
    double longitude_;
    std::vector<double> values_;
    double bottom_{std::numeric_limits<double>::infinity()};
    std::optional<double> top_;
};

/// Sorts by distance from an origin location.
///
/// Port of `LatLonPointSortField`
/// (`core/document/lat_lon_point_sort_field.rs`).
///
/// **Deviation:** the reference implementation is its own sort field type; the
/// port produces a [`SortField`] of type [`kCustom`](SortFieldType::kCustom) that
/// carries the distance comparator, so the search layer needs no new type.
export class LatLonPointSortField final {
public:
    /// A distance sort around `(latitude, longitude)`, after validating the
    /// coordinates.
    [[nodiscard]] static Result<LatLonPointSortField> create(std::string field, double latitude, double longitude);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] double latitude() const noexcept { return latitude_; }
    [[nodiscard]] double longitude() const noexcept { return longitude_; }

    /// The sort field the search layer consumes.
    [[nodiscard]] SortField to_sort_field() const;

    /// `<distance:"field" latitude=... longitude=...>`, the form the reference
    /// implementation prints.
    [[nodiscard]] std::string to_string() const;

private:
    LatLonPointSortField(std::string field, double latitude, double longitude);

    std::string field_;
    double latitude_;
    double longitude_;
};

} // namespace lucene
