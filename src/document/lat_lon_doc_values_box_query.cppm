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

export module lucene_core:lat_lon_doc_values_box_query;

import :types;
import std;

import :error;
import :identity;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :segment_cacheable;
import :weight;

namespace lucene {

/// Matches the documents whose [`LatLonDocValuesField`](:lat_lon_doc_values_field)
/// location falls inside a bounding box, read from the doc values.
///
/// Port of `LatLonDocValuesBoxQuery`
/// (`core/document/lat_lon_doc_values_box_query.rs`).
///
/// The doc values iterator proposes every document that has a value, and a
/// two-phase check decodes the packed location to confirm it.  A box whose
/// minimum longitude is larger than its maximum crosses the dateline, so the
/// confirm step accepts the longitudes on either side of it.
export class LatLonDocValuesBoxQuery final : public Query {
public:
    /// A query for the locations inside the box, after validating the bounds.
    [[nodiscard]] static Result<std::unique_ptr<LatLonDocValuesBoxQuery>> create(
            std::string field, double min_latitude, double max_latitude, double min_longitude, double max_longitude);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    /// The quantized bounds; the confirm step compares against them.
    [[nodiscard]] int32_t min_latitude() const noexcept { return min_latitude_; }
    [[nodiscard]] int32_t max_latitude() const noexcept { return max_latitude_; }
    [[nodiscard]] int32_t min_longitude() const noexcept { return min_longitude_; }
    [[nodiscard]] int32_t max_longitude() const noexcept { return max_longitude_; }
    [[nodiscard]] bool crosses_dateline() const noexcept { return crosses_dateline_; }
    [[nodiscard]] const Identity &identity() const noexcept { return identity_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `field:box(minLat=..., maxLat=..., minLon=..., maxLon=...)`, with the
    /// decoded (not the quantized) bounds.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    LatLonDocValuesBoxQuery(std::string field, int32_t min_latitude, int32_t max_latitude, int32_t min_longitude,
                            int32_t max_longitude, bool crosses_dateline);

    std::string field_;
    int32_t min_latitude_;
    int32_t max_latitude_;
    int32_t min_longitude_;
    int32_t max_longitude_;
    bool crosses_dateline_;
    Identity identity_;
};

} // namespace lucene
