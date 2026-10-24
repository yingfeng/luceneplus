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

export module lucene_core:lat_lon_point_distance_query;

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

/// Matches the documents whose [`LatLonPoint`](:lat_lon_point) lies within a
/// distance of a centre.
///
/// Port of `LatLonPointDistanceQuery`
/// (`core/document/lat_lon_point_distance_query.rs`).
///
/// The query first walks the point tree with the circle's bounding box (two boxes
/// when the circle reaches over the dateline), then falls back to the precomputed
/// distance predicate for the cells the circle crosses.  Every match scores the
/// boost, so the query is a constant score one.
export class LatLonPointDistanceQuery final : public Query {
public:
    /// A query for the points within `radius_meters` of `(latitude, longitude)`.
    [[nodiscard]] static Result<std::unique_ptr<LatLonPointDistanceQuery>> create(std::string field, double latitude,
                                                                                double longitude,
                                                                                double radius_meters);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] double latitude() const noexcept { return latitude_; }
    [[nodiscard]] double longitude() const noexcept { return longitude_; }
    [[nodiscard]] double radius_meters() const noexcept { return radius_meters_; }
    [[nodiscard]] const Identity &identity() const noexcept { return identity_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `field:<lat>,<lon> +/- <radius> meters`, with each number at one decimal.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    LatLonPointDistanceQuery(std::string field, double latitude, double longitude, double radius_meters);

    std::string field_;
    double latitude_;
    double longitude_;
    double radius_meters_;
    Identity identity_;
};

} // namespace lucene
