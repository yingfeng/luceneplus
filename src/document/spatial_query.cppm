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

export module lucene_core:spatial_query;

import :types;
import std;

import :error;
import :geo_geometry;
import :geo_component2d;
import :identity;
import :leaf_reader_context;
import :point_values;
import :query;
import :query_visitor;
import :score_mode;
import :segment_cacheable;
import :shape_field;
import :weight;

namespace lucene {

/// The relation of the query read from the indexed shape's point of view.
///
/// Port of `transpose_relation` (`core/document/spatial_query.rs`): asking for
/// "the cell does not match" is the same as asking for "the query matches the
/// complement", so the inside and outside relations swap.
[[nodiscard]] Relation transpose_relation(Relation relation) noexcept;

/// Walks the point tree of a leaf, answering the questions a spatial query asks
/// about each cell and each point.
///
/// Port of the `SpatialVisitor` trait (`core/document/spatial_query.rs`).
///
/// The three predicates are the primitive ones; the two `get_*` helpers turn them
/// into the answer the [`QueryRelation`] asks for, so a concrete visitor only has
/// to implement the geometry work.
export class SpatialVisitor {
public:
    virtual ~SpatialVisitor() = default;

    /// How the cell `[min_packed_value, max_packed_value]` relates to the query.
    [[nodiscard]] virtual Result<Relation> relate(std::span<const uint8_t> min_packed_value,
                                                  std::span<const uint8_t> max_packed_value) const = 0;

    /// Whether the point intersects the query shape.
    [[nodiscard]] virtual Result<bool> intersects(std::span<const uint8_t> packed_value) const = 0;

    /// Whether the point lies inside the query shape.
    [[nodiscard]] virtual Result<bool> within(std::span<const uint8_t> packed_value) const = 0;

    /// Whether the indexed shape contains the query shape at this point.
    [[nodiscard]] virtual Result<WithinRelation> contains(std::span<const uint8_t> packed_value) const = 0;

    /// [`contains`], reduced to "is it a candidate".
    [[nodiscard]] virtual Result<bool> contains_predicate(std::span<const uint8_t> packed_value) const;

    /// [`relate`], with the relation transposed for a disjoint query.
    [[nodiscard]] virtual Result<Relation> get_inner_relation(QueryRelation query_relation,
                                                              std::span<const uint8_t> min_packed_value,
                                                              std::span<const uint8_t> max_packed_value) const;

    /// The predicate that decides whether one point matches, for `query_relation`.
    [[nodiscard]] virtual Result<bool> get_leaf_predicate(QueryRelation query_relation,
                                                          std::span<const uint8_t> packed_value) const;
};

/// Builds the [`SpatialVisitor`] of a concrete spatial query.
///
/// Port of the `SpatialQueryBase` trait (`core/document/spatial_query.rs`).
///
/// **Deviation:** the reference implementation makes this the type parameter of
/// `SpatialQuery`; the port erases the type behind this interface, which is what
/// the query needs anyway.
export class SpatialQueryBase {
public:
    virtual ~SpatialQueryBase() = default;

    [[nodiscard]] virtual Result<std::unique_ptr<SpatialVisitor>> spatial_visitor() const = 0;
};

/// A query that matches documents by the relation of their indexed shapes to a
/// set of query geometries.
///
/// Port of `SpatialQuery` (`core/document/spatial_query.rs`).
///
/// The concrete queries (`LatLonPointQuery`, `XYPointQuery`, ...) only supply the
/// [`SpatialQueryBase`] that knows how to answer the geometry questions; the
/// matching itself (which documents, and how to collect them efficiently) lives
/// here.
///
/// **Deviations:**
///  * `equals` compares the geometries by identity, because the port's
///    [`Geometry`] has no value equality; two queries built from separately
///    constructed but equal shapes are therefore not equal;
///  * `to_string` prints the relation and the number of geometries instead of the
///    reference implementation's type name and shape list, since the port's shapes
///    do not implement `to_string`.
export class SpatialQuery final : public Query {
public:
    [[nodiscard]] static Result<std::unique_ptr<SpatialQuery>> create(
            std::string field, QueryRelation query_relation, std::vector<std::shared_ptr<const Geometry>> geometries,
            std::shared_ptr<const SpatialQueryBase> sub);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] QueryRelation query_relation() const noexcept { return query_relation_; }
    [[nodiscard]] const std::vector<std::shared_ptr<const Geometry>> &geometries() const noexcept {
        return geometries_;
    }
    [[nodiscard]] const Identity &identity() const noexcept;

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    SpatialQuery(std::string field, QueryRelation query_relation,
                 std::vector<std::shared_ptr<const Geometry>> geometries, std::shared_ptr<const SpatialQueryBase> sub);

    std::string field_;
    QueryRelation query_relation_;
    std::vector<std::shared_ptr<const Geometry>> geometries_;
    std::shared_ptr<const SpatialQueryBase> sub_;
    Identity identity_;
};

} // namespace lucene
