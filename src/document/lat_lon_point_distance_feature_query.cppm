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

export module lucene_core:lat_lon_point_distance_feature_query;

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

/// Boosts the documents that are close to an origin, without filtering any of
/// them out.
///
/// Port of `LatLonPointDistanceFeatureQuery`
/// (`core/document/lat_lon_point_distance_feature_query.rs`).
///
/// The score of a document is
/// `boost * pivotDistance / (pivotDistance + distance)`, so a document at the
/// origin scores `boost` and the score halves once the document is
/// `pivotDistance` away.  A document without a location does not match.
///
/// The locations come from the doc values of the field, which is why the field
/// has to be indexed as both points and [`LatLonDocValuesField`](:lat_lon_doc_values_field);
/// the point values are only needed to tell whether the field exists in a leaf.
export class LatLonPointDistanceFeatureQuery final : public Query {
public:
    /// A query boosting the documents near `(origin_lat, origin_lon)`;
    /// `pivot_distance` must be positive.
    [[nodiscard]] static Result<std::unique_ptr<LatLonPointDistanceFeatureQuery>> create(
            std::string field, double origin_lat, double origin_lon, double pivot_distance);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] double origin_lat() const noexcept { return origin_lat_; }
    [[nodiscard]] double origin_lon() const noexcept { return origin_lon_; }
    [[nodiscard]] double pivot_distance() const noexcept { return pivot_distance_; }
    [[nodiscard]] const Identity &identity() const noexcept { return identity_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `LatLonPointDistanceFeatureQuery(field=...,originLat=...,originLon=...,pivotDistance=...)`.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    LatLonPointDistanceFeatureQuery(std::string field, double origin_lat, double origin_lon, double pivot_distance);

    std::string field_;
    double origin_lat_;
    double origin_lon_;
    double pivot_distance_;
    Identity identity_;
};

} // namespace lucene
