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

module lucene_core:lat_lon_doc_values_query.impl;

import :lat_lon_doc_values_query;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :geo_component2d;
import :geo_encoding_utils;
import :geo_geometry;
import :geo_predicates;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :shape_field;
import :sorted_numeric_doc_values;
import :two_phase_iterator;
import :types;
import :weight;

namespace lucene {

namespace {

/// Decodes the packed location of one doc value.
[[nodiscard]] std::pair<double, double> decode_location(int64_t value) {
    const auto packed = static_cast<uint64_t>(value);
    const double latitude = GeoEncodingUtils::decode_latitude(static_cast<int32_t>(packed >> 32U));
    const double longitude = GeoEncodingUtils::decode_longitude(static_cast<int32_t>(static_cast<uint32_t>(packed)));
    return {latitude, longitude};
}

/// Confirms the candidates the doc values iterator proposes, for every relation.
///
/// Port of the four two-phase iterators of
/// `core/document/lat_lon_doc_values_query.rs`, collapsed into one class that
/// switches on the relation (the reference keeps one class per relation because
/// Rust has no easy way to reuse the shared body).
class DocValuesTwoPhaseIterator final : public TwoPhaseIterator {
public:
    /// The state a relation needs: the predicate for the three point-in-shape
    /// relations, and the per-shape components for `Contains`.
    struct Context {
        QueryRelation relation{QueryRelation::Intersects};
        std::shared_ptr<const Component2DPredicate> predicate;
        std::vector<std::shared_ptr<Component2D>> components;
    };

    DocValuesTwoPhaseIterator(std::unique_ptr<SortedNumericDocValues> values, Context context)
            : values_(std::move(values)), context_(std::move(context)) {}

    [[nodiscard]] int32_t approx_doc_id() const override { return values_->doc_id(); }

    [[nodiscard]] Result<int32_t> approx_next_doc() override { return values_->next_doc(); }

    [[nodiscard]] Result<int32_t> approx_advance(int32_t target) override { return values_->advance(target); }

    [[nodiscard]] Result<int32_t> approx_slow_advance(int32_t target) override {
        return values_->slow_advance(target);
    }

    [[nodiscard]] Result<int64_t> approx_cost() const override { return values_->cost(); }

    [[nodiscard]] Result<bool> matches() override {
        auto count = values_->doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        switch (context_.relation) {
            case QueryRelation::Intersects:
                return any_value([this](int64_t value) -> Result<bool> {
                    const auto [latitude, longitude] = decode_location(value);
                    (void)latitude;
                    (void)longitude;
                    return inside(value);
                });
            case QueryRelation::Within:
                return all_values([this](int64_t value) -> Result<bool> { return inside(value); });
            case QueryRelation::Disjoint:
                return all_values([this](int64_t value) -> Result<bool> {
                    auto matched = inside(value);
                    if (!matched.has_value()) {
                        return std::unexpected(std::move(matched.error()));
                    }
                    return !matched.value();
                });
            case QueryRelation::Contains:
                return contains(count.value());
        }
        return err(LuceneError::illegal_argument("unknown query relation"));
    }

    /// The reference implementation expects a containment test to be much more
    /// expensive than the box tests of the distance queries.
    [[nodiscard]] float match_cost() const override { return 1000.0F; }

private:
    /// Whether one packed value is inside the query shapes.
    [[nodiscard]] Result<bool> inside(int64_t value) const {
        const auto packed = static_cast<uint64_t>(value);
        const int32_t latitude = static_cast<int32_t>(packed >> 32U);
        const int32_t longitude = static_cast<int32_t>(static_cast<uint32_t>(packed));
        if (context_.predicate == nullptr) {
            return err(LuceneError::illegal_state("the query has no component predicate"));
        }
        return context_.predicate->test(latitude, longitude);
    }

    /// `true` when at least one value satisfies `test`.
    template <typename Test>
    [[nodiscard]] Result<bool> any_value(Test test) {
        auto count = values_->doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values_->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            auto matched = test(value.value());
            if (!matched.has_value()) {
                return std::unexpected(std::move(matched.error()));
            }
            if (matched.value()) {
                return true;
            }
        }
        return false;
    }

    /// `true` when every value satisfies `test` (a document always has one value).
    template <typename Test>
    [[nodiscard]] Result<bool> all_values(Test test) {
        auto count = values_->doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values_->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            auto matched = test(value.value());
            if (!matched.has_value()) {
                return std::unexpected(std::move(matched.error()));
            }
            if (!matched.value()) {
                return false;
            }
        }
        return true;
    }

    /// The `Contains` relation: every location has to be a within-candidate of
    /// every geometry.
    [[nodiscard]] Result<bool> contains(int32_t count) {
        WithinRelation answer = WithinRelation::Disjoint;
        for (int32_t i = 0; i < count; ++i) {
            auto value = values_->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            const auto [latitude, longitude] = decode_location(value.value());
            for (const auto &component : context_.components) {
                auto relation = component->within_point(longitude, latitude);
                if (!relation.has_value()) {
                    return std::unexpected(std::move(relation.error()));
                }
                if (relation.value() == WithinRelation::NotWithin) {
                    return false;
                }
                if (relation.value() != WithinRelation::Disjoint) {
                    answer = relation.value();
                }
            }
        }
        return answer == WithinRelation::Candidate;
    }

    std::unique_ptr<SortedNumericDocValues> values_;
    Context context_;
};

/// Supplies the two-phase scorer of one leaf.
class DocValuesScorerSupplier final : public ScorerSupplier {
public:
    DocValuesScorerSupplier(std::unique_ptr<SortedNumericDocValues> values,
                            DocValuesTwoPhaseIterator::Context context, float score, ScoreMode score_mode)
            : values_(std::move(values)),
              context_(std::move(context)),
              score_(score),
              score_mode_(score_mode) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        std::unique_ptr<TwoPhaseIterator> iterator =
                std::make_unique<DocValuesTwoPhaseIterator>(std::move(values_), context_);
        return ConstantScoreScorer::from_tpi(score_, score_mode_, std::move(iterator));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        if (values_ == nullptr) {
            // `get` already consumed the iterator.
            return 0;
        }
        return values_->cost();
    }

private:
    std::unique_ptr<SortedNumericDocValues> values_;
    DocValuesTwoPhaseIterator::Context context_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

/// The weight of a [`LatLonDocValuesQuery`].
///
/// Port of `LatLonDocValuesQueryWeight`
/// (`core/document/lat_lon_doc_values_query.rs`).
class DocValuesWeight final : public Weight {
public:
    DocValuesWeight(const LatLonDocValuesQuery &query, DocValuesTwoPhaseIterator::Context context, float score,
                    ScoreMode score_mode)
            : query_(query), context_(std::move(context)), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        auto supplier = const_cast<DocValuesWeight *>(this)->scorer_supplier(context, searcher);
        if (!supplier.has_value()) {
            return std::unexpected(std::move(supplier.error()));
        }
        if (supplier.value() == nullptr) {
            return Explanation::no_match_no_details("no doc values for field " + query_.field());
        }
        auto scorer = supplier.value()->get(std::numeric_limits<int64_t>::max(), context, searcher);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        auto advanced = scorer.value()->advance(doc);
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
        auto values = context.reader()->get_sorted_numeric_doc_values(query_.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        return std::unique_ptr<ScorerSupplier>(std::make_unique<DocValuesScorerSupplier>(
                std::move(values.value()), context_, score_, score_mode_));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        auto values = context.reader()->get_sorted_numeric_doc_values(query_.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        return true;
    }

private:
    const LatLonDocValuesQuery &query_;
    DocValuesTwoPhaseIterator::Context context_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

/// Builds the state the two-phase iterator needs for `relation`.
[[nodiscard]] Result<DocValuesTwoPhaseIterator::Context> build_context(
        QueryRelation relation, const std::vector<std::shared_ptr<const Geometry>> &geometries) {
    DocValuesTwoPhaseIterator::Context context;
    context.relation = relation;

    std::vector<const Geometry *> shapes;
    shapes.reserve(geometries.size());
    for (const auto &geometry : geometries) {
        shapes.push_back(geometry.get());
    }

    if (relation == QueryRelation::Contains) {
        // Every shape gets its own component: the relation is checked one shape at
        // a time.
        for (const Geometry *shape : shapes) {
            std::vector<const Geometry *> single{shape};
            auto component = create_geometry_component(single);
            if (!component.has_value()) {
                return std::unexpected(std::move(component.error()));
            }
            context.components.push_back(std::move(component.value()));
        }
        return context;
    }

    auto component = create_geometry_component(shapes);
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }
    auto predicate = create_component_predicate(std::move(component.value()));
    if (!predicate.has_value()) {
        return std::unexpected(std::move(predicate.error()));
    }
    context.predicate = std::make_shared<const Component2DPredicate>(std::move(predicate.value()));
    return context;
}

} // namespace

LatLonDocValuesQuery::LatLonDocValuesQuery(std::string field, QueryRelation query_relation,
                                           std::vector<std::shared_ptr<const Geometry>> geometries)
        : field_(std::move(field)), query_relation_(query_relation), geometries_(std::move(geometries)) {}

Result<std::unique_ptr<LatLonDocValuesQuery>> LatLonDocValuesQuery::create(
        std::string field, QueryRelation query_relation, std::vector<std::shared_ptr<const Geometry>> geometries) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }
    if (geometries.empty()) {
        return err(LuceneError::illegal_argument("geometries must not be empty"));
    }
    for (const auto &geometry : geometries) {
        if (geometry == nullptr) {
            return err(LuceneError::illegal_argument("the geometries must not be null"));
        }
    }
    return std::unique_ptr<LatLonDocValuesQuery>(
            new LatLonDocValuesQuery(std::move(field), query_relation, std::move(geometries)));
}

Result<std::unique_ptr<Weight>> LatLonDocValuesQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                   ScoreMode score_mode, float boost) const {
    auto context = build_context(query_relation_, geometries_);
    if (!context.has_value()) {
        return std::unexpected(std::move(context.error()));
    }
    std::unique_ptr<Weight> weight(new DocValuesWeight(*this, std::move(context.value()), boost, score_mode));
    return weight;
}

VoidResult LatLonDocValuesQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        if (auto visited = visitor.visit_leaf(*this); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> LatLonDocValuesQuery::to_string(std::string_view field) const {
    std::string text;
    if (field != field_) {
        text += field_ + ":";
    }
    text += std::string(query_relation_to_string(query_relation_));
    text += ":geometries(" + std::to_string(geometries_.size()) + ")";
    return text;
}

bool LatLonDocValuesQuery::equals(const Query &other) const {
    const auto *query = dynamic_cast<const LatLonDocValuesQuery *>(&other);
    if (query == nullptr) {
        return false;
    }
    if (field_ != query->field_ || query_relation_ != query->query_relation_ ||
        geometries_.size() != query->geometries_.size()) {
        return false;
    }
    // Geometries are compared by identity, as everywhere else in the port.
    for (size_t i = 0; i < geometries_.size(); ++i) {
        if (geometries_[i].get() != query->geometries_[i].get()) {
            return false;
        }
    }
    return true;
}

size_t LatLonDocValuesQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    result = result * 31 + static_cast<size_t>(query_relation_);
    result = result * 31 + geometries_.size();
    return result;
}

} // namespace lucene
