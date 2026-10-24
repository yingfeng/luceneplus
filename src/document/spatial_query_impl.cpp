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

module lucene_core:spatial_query.impl;

import :spatial_query;

import std;

import :bulk_scorer;
import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :field_infos;
import :fixed_bit_set;
import :geo_component2d;
import :identity;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :point_values;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :segment_cacheable;
import :shape_field;
import :types;
import :weight;

namespace lucene {

namespace {

/// Iterates the set bits of a [`FixedBitSet`].
///
/// A port of `BitSetIterator` (`util/bits.rs`), which the port does not have; it
/// is enough for the spatial collector, which only ever iterates its own bitset.
class FixedBitSetIterator final : public DocIdSetIterator {
public:
    FixedBitSetIterator(const FixedBitSet &bits, int64_t cost) : bits_(bits), cost_(cost) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (doc_ == NO_MORE_DOCS) {
            return doc_;
        }
        const int64_t next = bits_.next_set_bit(static_cast<size_t>(doc_ + 1));
        // `next_set_bit` reports "no more bits" with `-1`, while a document
        // iterator uses `NO_MORE_DOCS`.
        doc_ = next < 0 ? NO_MORE_DOCS : static_cast<int32_t>(next);
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (target > doc_) {
            doc_ = target - 1;
        }
        return next_doc();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }

private:
    const FixedBitSet &bits_;
    int64_t cost_{0};
    int32_t doc_{-1};
};

/// Collects the documents that match, using the query's leaf predicate.
///
/// Port of the `visit`/`visit_with_packed_value` half of `SparseVisitor` and
/// `DenseVisitor` (`core/document/spatial_query.rs`).
class PredicateCollectVisitor final : public IntersectVisitor {
public:
    PredicateCollectVisitor(const SpatialVisitor &visitor, QueryRelation query_relation, FixedBitSet &result)
            : visitor_(visitor), query_relation_(query_relation), result_(result) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        result_.set(static_cast<size_t>(doc_id));
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        auto match = visitor_.get_leaf_predicate(query_relation_, packed_value);
        if (!match.has_value()) {
            return std::unexpected(std::move(match.error()));
        }
        if (match.value()) {
            result_.set(static_cast<size_t>(doc_id));
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        return visitor_.get_inner_relation(query_relation_, min_packed_value, max_packed_value);
    }

private:
    const SpatialVisitor &visitor_;
    QueryRelation query_relation_;
    FixedBitSet &result_;
};

/// Collects the documents that contain the query shape.
///
/// Port of `ContainsDenseVisitor` (`core/document/spatial_query.rs`): a cell that
/// is fully inside the query excludes its documents, a candidate cell adds them.
class ContainsCollectVisitor final : public IntersectVisitor {
public:
    ContainsCollectVisitor(const SpatialVisitor &visitor, QueryRelation query_relation, FixedBitSet &result,
                           FixedBitSet &excluded)
            : visitor_(visitor), query_relation_(query_relation), result_(result), excluded_(excluded) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        excluded_.set(static_cast<size_t>(doc_id));
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        const size_t index = static_cast<size_t>(doc_id);
        if (excluded_.get_bit(index)) {
            return {};
        }
        auto relation = visitor_.contains(packed_value);
        if (!relation.has_value()) {
            return std::unexpected(std::move(relation.error()));
        }
        switch (relation.value()) {
            case WithinRelation::Candidate:
                result_.set(index);
                break;
            case WithinRelation::NotWithin:
                excluded_.set(index);
                break;
            case WithinRelation::Disjoint:
                break;
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        return visitor_.get_inner_relation(query_relation_, min_packed_value, max_packed_value);
    }

private:
    const SpatialVisitor &visitor_;
    QueryRelation query_relation_;
    FixedBitSet &result_;
    FixedBitSet &excluded_;
};

/// Stops the tree walk as soon as one document matches.
///
/// Port of `HasAnyHitsVisitor` (`core/document/spatial_query.rs`), which reports
/// the early exit through the "collection terminated" error.
class HasAnyHitsVisitor final : public IntersectVisitor {
public:
    HasAnyHitsVisitor(const SpatialVisitor &visitor, QueryRelation query_relation)
            : visitor_(visitor), query_relation_(query_relation) {}

    [[nodiscard]] VoidResult visit(int32_t /*doc_id*/) override {
        return err(LuceneError::collection_terminated("a hit was found"));
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t /*doc_id*/,
                                                     std::span<const uint8_t> packed_value) override {
        auto match = visitor_.get_leaf_predicate(query_relation_, packed_value);
        if (!match.has_value()) {
            return std::unexpected(std::move(match.error()));
        }
        if (match.value()) {
            return err(LuceneError::collection_terminated("a hit was found"));
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        auto relation = visitor_.get_inner_relation(query_relation_, min_packed_value, max_packed_value);
        if (!relation.has_value()) {
            return std::unexpected(std::move(relation.error()));
        }
        if (relation.value() == Relation::kCellInsideQuery) {
            return err(LuceneError::collection_terminated("a hit was found"));
        }
        return relation;
    }

private:
    const SpatialVisitor &visitor_;
    QueryRelation query_relation_;
};

/// Whether any document of the leaf matches; cheap in the adversarial case where
/// almost nothing matches.
[[nodiscard]] Result<bool> has_any_hits(const SpatialVisitor &visitor, QueryRelation query_relation,
                                        const PointValues &values) {
    HasAnyHitsVisitor hit_visitor(visitor, query_relation);
    auto intersected = values.intersect(hit_visitor);
    if (!intersected.has_value()) {
        if (intersected.error().kind() == ErrorKind::kCollectionTerminated) {
            return true;
        }
        return std::unexpected(std::move(intersected.error()));
    }
    return false;
}

/// Supplies the scorer that walks the collected documents.
///
/// Port of `RelationScorerSupplier` (`core/document/spatial_query.rs`).
///
/// **Deviations:** the reference implementation has three collection strategies
/// (a dense bitset, an inverted dense bitset for "almost everything matches", and
/// a `DocIdSetBuilder` for sparse results) and picks one per relation; the port
/// always collects into a [`FixedBitSet`], which is correct at the cost of one bit
/// per document, and takes the cost from the collected count instead of
/// estimating it from the tree.
class RelationScorerSupplier final : public ScorerSupplier {
public:
    RelationScorerSupplier(std::shared_ptr<PointValues> values, std::shared_ptr<const SpatialVisitor> visitor,
                           QueryRelation query_relation, std::string field, float score, ScoreMode score_mode,
                           int32_t max_doc)
            : values_(std::move(values)),
              visitor_(std::move(visitor)),
              query_relation_(query_relation),
              field_(std::move(field)),
              score_(score),
              score_mode_(score_mode),
              max_doc_(max_doc) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return get_scorer();
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        auto scorer = get_scorer();
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        auto scorer_cost = scorer.value()->cost();
        if (!scorer_cost.has_value()) {
            return std::unexpected(std::move(scorer_cost.error()));
        }
        cost_ = scorer_cost.value();
        return cost_;
    }

private:
    [[nodiscard]] Result<std::unique_ptr<Scorer>> get_scorer() {
        if (scorer_ != nullptr) {
            return std::unique_ptr<Scorer>(std::move(scorer_));
        }

        FixedBitSet result(static_cast<size_t>(max_doc_));
        if (query_relation_ == QueryRelation::Contains) {
            FixedBitSet excluded(static_cast<size_t>(max_doc_));
            ContainsCollectVisitor visitor(*visitor_, query_relation_, result, excluded);
            auto intersected = values_->intersect(visitor);
            if (!intersected.has_value()) {
                return std::unexpected(std::move(intersected.error()));
            }
            result.and_not(excluded);
        } else {
            PredicateCollectVisitor visitor(*visitor_, query_relation_, result);
            auto intersected = values_->intersect(visitor);
            if (!intersected.has_value()) {
                return std::unexpected(std::move(intersected.error()));
            }
        }

        const int64_t collected = static_cast<int64_t>(result.cardinality());
        // The iterator borrows the bitset, so the bitset is kept alive alongside the
        // scorer for as long as the supplier lives.
        bitset_ = std::move(result);
        std::unique_ptr<DocIdSetIterator> iterator = std::make_unique<FixedBitSetIterator>(bitset_, collected);
        scorer_ = ConstantScoreScorer::from_disi(score_, score_mode_, std::move(iterator));
        return std::unique_ptr<Scorer>(std::move(scorer_));
    }

    std::shared_ptr<PointValues> values_;
    std::shared_ptr<const SpatialVisitor> visitor_;
    QueryRelation query_relation_;
    std::string field_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
    int32_t max_doc_{0};
    int64_t cost_{-1};
    /// The collected bits are kept alive because the iterator borrows them.
    FixedBitSet bitset_;
    std::unique_ptr<Scorer> scorer_;
};

/// Supplies a scorer for every document of the leaf.
///
/// Port of `ScorerSupplierImpl` (`core/document/spatial_query.rs`).
class AllDocsScorerSupplier final : public ScorerSupplier {
public:
    AllDocsScorerSupplier(float score, ScoreMode score_mode, int32_t max_doc)
            : score_(score), score_mode_(score_mode), max_doc_(max_doc) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return ConstantScoreScorer::from_disi(
                score_, score_mode_, std::unique_ptr<DocIdSetIterator>(std::make_unique<AllDISI>(max_doc_)));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return static_cast<int64_t>(max_doc_);
    }

private:
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
    int32_t max_doc_{0};
};

/// The weight of a [`SpatialQuery`].
///
/// Port of `SpatialWeight` (`core/document/spatial_query.rs`).
///
/// **Deviation:** the reference implementation shares a `ConstantScoreWeight`
/// helper with the other constant score queries; the port has no such helper, so
/// the constant score behaviour is spelled out here.
class SpatialWeight final : public Weight {
public:
    SpatialWeight(const SpatialQuery &query, std::shared_ptr<const SpatialVisitor> visitor, float score,
                  ScoreMode score_mode)
            : query_(query), visitor_(std::move(visitor)), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        // `Weight::scorer_supplier` is non-const (it may cache per-leaf state), so
        // the const `explain` has to reach it through a cast.
        auto supplier = const_cast<SpatialWeight *>(this)->scorer_supplier(context, searcher);
        if (!supplier.has_value()) {
            return std::unexpected(std::move(supplier.error()));
        }
        if (supplier.value() == nullptr) {
            return Explanation::no_match_no_details("no points for field " + query_.field());
        }
        auto scorer = supplier.value()->get(std::numeric_limits<int64_t>::max(), context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        const auto advanced = scorer.value()->advance(doc);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        auto description = query_.to_string("");
        if (!description.has_value()) {
            return std::unexpected(std::move(description.error()));
        }
        if (advanced.value() == doc) {
            return Explanation::match_no_details(score_, description.value());
        }
        return Explanation::no_match_no_details(description.value());
    }

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        LeafReader *reader = context.reader();
        auto field_infos = reader->field_infos();
        if (!field_infos.has_value()) {
            return std::unexpected(std::move(field_infos.error()));
        }
        if (field_infos.value() == nullptr || field_infos.value()->by_name(query_.field()) == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }

        auto values = reader->point_values(query_.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }

        auto max_doc = reader->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        auto doc_count = values.value()->get_doc_count();
        if (!doc_count.has_value()) {
            return std::unexpected(std::move(doc_count.error()));
        }
        auto size = values.value()->size();
        if (!size.has_value()) {
            return std::unexpected(std::move(size.error()));
        }

        auto min_packed = values.value()->get_min_packed_value();
        if (!min_packed.has_value()) {
            return std::unexpected(std::move(min_packed.error()));
        }
        auto max_packed = values.value()->get_max_packed_value();
        if (!max_packed.has_value()) {
            return std::unexpected(std::move(max_packed.error()));
        }
        if (!min_packed.value().has_value() || !max_packed.value().has_value()) {
            return err(LuceneError::illegal_state("the point values have no bounding box"));
        }

        auto relation = visitor_->get_inner_relation(query_.query_relation(), min_packed.value().value().as_slice(),
                                                     max_packed.value().value().as_slice());
        if (!relation.has_value()) {
            return std::unexpected(std::move(relation.error()));
        }

        if (relation.value() == Relation::kCellOutsideQuery ||
            (relation.value() == Relation::kCellInsideQuery && query_.query_relation() == QueryRelation::Contains)) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        if (doc_count.value() == max_doc.value() && relation.value() == Relation::kCellInsideQuery) {
            return std::unique_ptr<ScorerSupplier>(
                    std::make_unique<AllDocsScorerSupplier>(score_, score_mode_, max_doc.value()));
        }

        const QueryRelation query_relation = query_.query_relation();
        if (query_relation != QueryRelation::Intersects && query_relation != QueryRelation::Contains &&
            static_cast<size_t>(doc_count.value()) != size.value()) {
            auto any_hits = has_any_hits(*visitor_, query_relation, *values.value());
            if (!any_hits.has_value()) {
                return std::unexpected(std::move(any_hits.error()));
            }
            if (!any_hits.value()) {
                // Nothing matches at all, which the tree walk would only find out
                // after visiting every point.
                return std::unique_ptr<ScorerSupplier>{};
            }
        }

        return std::unique_ptr<ScorerSupplier>(std::make_unique<RelationScorerSupplier>(
                values.value(), visitor_, query_relation, query_.field(), score_, score_mode_, max_doc.value()));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const SpatialQuery &query_;
    std::shared_ptr<const SpatialVisitor> visitor_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

} // namespace

Relation transpose_relation(Relation relation) noexcept {
    switch (relation) {
        case Relation::kCellInsideQuery:
            return Relation::kCellOutsideQuery;
        case Relation::kCellOutsideQuery:
            return Relation::kCellInsideQuery;
        case Relation::kCellCrossesQuery:
            return Relation::kCellCrossesQuery;
    }
    return Relation::kCellCrossesQuery;
}

Result<bool> SpatialVisitor::contains_predicate(std::span<const uint8_t> packed_value) const {
    auto relation = contains(packed_value);
    if (!relation.has_value()) {
        return std::unexpected(std::move(relation.error()));
    }
    return relation.value() == WithinRelation::Candidate;
}

Result<Relation> SpatialVisitor::get_inner_relation(QueryRelation query_relation,
                                                    std::span<const uint8_t> min_packed_value,
                                                    std::span<const uint8_t> max_packed_value) const {
    auto relation = relate(min_packed_value, max_packed_value);
    if (!relation.has_value()) {
        return std::unexpected(std::move(relation.error()));
    }
    if (query_relation == QueryRelation::Disjoint) {
        return transpose_relation(relation.value());
    }
    return relation;
}

Result<bool> SpatialVisitor::get_leaf_predicate(QueryRelation query_relation,
                                               std::span<const uint8_t> packed_value) const {
    switch (query_relation) {
        case QueryRelation::Intersects:
            return intersects(packed_value);
        case QueryRelation::Within:
            return within(packed_value);
        case QueryRelation::Disjoint: {
            auto match = intersects(packed_value);
            if (!match.has_value()) {
                return std::unexpected(std::move(match.error()));
            }
            return !match.value();
        }
        case QueryRelation::Contains:
            return contains_predicate(packed_value);
    }
    return err(LuceneError::illegal_argument("unknown query relation"));
}

SpatialQuery::SpatialQuery(std::string field, QueryRelation query_relation,
                           std::vector<std::shared_ptr<const Geometry>> geometries,
                           std::shared_ptr<const SpatialQueryBase> sub)
        : field_(std::move(field)),
          query_relation_(query_relation),
          geometries_(std::move(geometries)),
          sub_(std::move(sub)) {}

Result<std::unique_ptr<SpatialQuery>> SpatialQuery::create(std::string field, QueryRelation query_relation,
                                                           std::vector<std::shared_ptr<const Geometry>> geometries,
                                                           std::shared_ptr<const SpatialQueryBase> sub) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }
    if (geometries.empty()) {
        return err(LuceneError::illegal_argument("at least one geometry is required"));
    }
    if (sub == nullptr) {
        return err(LuceneError::illegal_argument("the spatial query needs a visitor factory"));
    }
    return std::unique_ptr<SpatialQuery>(new SpatialQuery(std::move(field), query_relation, std::move(geometries),
                                                         std::move(sub)));
}

const Identity &SpatialQuery::identity() const noexcept { return identity_; }

Result<std::unique_ptr<Weight>> SpatialQuery::create_weight(const IndexSearcher & /*searcher*/, ScoreMode score_mode,
                                                            float boost) const {
    auto visitor = sub_->spatial_visitor();
    if (!visitor.has_value()) {
        return std::unexpected(std::move(visitor.error()));
    }
    std::shared_ptr<const SpatialVisitor> shared(std::move(visitor.value()));
    std::unique_ptr<Weight> weight(new SpatialWeight(*this, std::move(shared), boost, score_mode));
    return weight;
}

VoidResult SpatialQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        if (auto visited = visitor.visit_leaf(*this); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> SpatialQuery::to_string(std::string_view field) const {
    std::string text = "SpatialQuery:";
    if (field != field_) {
        text += " field=" + field_ + ":";
    }
    text += "[";
    text += query_relation_to_string(query_relation_);
    text += ", geometries=" + std::to_string(geometries_.size());
    text += "]";
    return text;
}

bool SpatialQuery::equals(const Query &other) const {
    const auto *spatial = dynamic_cast<const SpatialQuery *>(&other);
    if (spatial == nullptr) {
        return false;
    }
    if (field_ != spatial->field_ || query_relation_ != spatial->query_relation_ ||
        geometries_.size() != spatial->geometries_.size()) {
        return false;
    }
    for (size_t i = 0; i < geometries_.size(); ++i) {
        if (geometries_[i].get() != spatial->geometries_[i].get()) {
            return false;
        }
    }
    return true;
}

size_t SpatialQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    result = result * 31 + static_cast<size_t>(query_relation_);
    result = result * 31 + geometries_.size();
    return result;
}

} // namespace lucene
