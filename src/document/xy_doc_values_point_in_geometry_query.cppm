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

export module lucene_core:xy_doc_values_point_in_geometry_query;

import :types;
import std;

import :error;
import :geo_component2d;
import :geo_geometry;
import :identity;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :segment_cacheable;
import :weight;

namespace lucene {

/// Matches the documents whose [`XYDocValuesField`](:xy_doc_values_field) location
/// falls inside the given geometries, read from the doc values.
///
/// Port of `XYDocValuesPointInGeometryQuery`
/// (`core/document/xy_doc_values_point_in_geometry_query.rs`).
///
/// Like the point-index variant there is no relation to choose: the shapes are
/// folded into one [`Component2D`], and the doc values iterator proposes every
/// document that has a value while a two-phase check confirms it.
export class XYDocValuesPointInGeometryQuery final : public Query {
public:
    /// A query over `field` for the locations inside `geometries`, which must not
    /// be empty.
    [[nodiscard]] static Result<std::unique_ptr<XYDocValuesPointInGeometryQuery>> create(
            std::string field, std::vector<std::shared_ptr<const Geometry>> geometries);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] const std::vector<std::shared_ptr<const Geometry>> &geometries() const noexcept {
        return geometries_;
    }
    [[nodiscard]] const Identity &identity() const noexcept { return identity_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `XYDocValuesPointInGeometryQuery[: field=x:] [geometries=N]`; the port prints
    /// the number of shapes instead of each one.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    XYDocValuesPointInGeometryQuery(std::string field, std::vector<std::shared_ptr<const Geometry>> geometries,
                                    std::shared_ptr<Component2D> component2d);

    std::string field_;
    std::vector<std::shared_ptr<const Geometry>> geometries_;
    std::shared_ptr<Component2D> component2d_;
    Identity identity_;
};

} // namespace lucene
