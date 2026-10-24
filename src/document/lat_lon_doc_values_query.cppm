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

export module lucene_core:lat_lon_doc_values_query;

import :types;
import std;

import :error;
import :geo_geometry;
import :identity;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :segment_cacheable;
import :shape_field;
import :weight;

namespace lucene {

/// Matches the documents whose [`LatLonDocValuesField`](:lat_lon_doc_values_field)
/// location satisfies a relation with the given geometries, read from the doc
/// values.
///
/// Port of `LatLonDocValuesQuery` (`core/document/lat_lon_doc_values_query.rs`).
///
/// This is the doc-values counterpart of the point-index geometry query: it needs
/// no point index, but it cannot prune either, so every document that has a value
/// is proposed and then confirmed by a two-phase check.
///
/// **Deviations:**
///  * the reference implementation rejects line geometries for `Within` and
///    non-point geometries for `Contains`; the port's [`Geometry`] carries no kind
///    tag yet, so those combinations are not rejected;
///  * the reference implementation keeps one iterator class per relation; the port
///    switches on the relation inside a single two-phase iterator, which behaves
///    the same.
export class LatLonDocValuesQuery final : public Query {
public:
    /// A query over `field` for the locations that relate to `geometries` as
    /// `query_relation` says; the shapes must not be empty.
    [[nodiscard]] static Result<std::unique_ptr<LatLonDocValuesQuery>> create(
            std::string field, QueryRelation query_relation,
            std::vector<std::shared_ptr<const Geometry>> geometries);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] QueryRelation query_relation() const noexcept { return query_relation_; }
    [[nodiscard]] const std::vector<std::shared_ptr<const Geometry>> &geometries() const noexcept {
        return geometries_;
    }
    [[nodiscard]] const Identity &identity() const noexcept { return identity_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `field:<relation>:geometries(<n>)`; the port prints how many shapes there
    /// are instead of each one.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    LatLonDocValuesQuery(std::string field, QueryRelation query_relation,
                         std::vector<std::shared_ptr<const Geometry>> geometries);

    std::string field_;
    QueryRelation query_relation_;
    std::vector<std::shared_ptr<const Geometry>> geometries_;
    Identity identity_;
};

} // namespace lucene
