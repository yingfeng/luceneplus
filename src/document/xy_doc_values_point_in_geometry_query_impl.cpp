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

module lucene_core:xy_doc_values_point_in_geometry_query.impl;

import :xy_doc_values_point_in_geometry_query;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :geo_component2d;
import :geo_geometry;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :sorted_numeric_doc_values;
import :two_phase_iterator;
import :types;
import :weight;
import :xy_encoding_utils;

namespace lucene {

namespace {

/// Confirms the candidates the doc values iterator proposes.
///
/// Port of `XYDocValuesPointInGeometryTPI`
/// (`core/document/xy_doc_values_point_in_geometry_query.rs`).
class GeometryTwoPhaseIterator final : public TwoPhaseIterator {
public:
    GeometryTwoPhaseIterator(std::unique_ptr<SortedNumericDocValues> values, std::shared_ptr<Component2D> component)
            : values_(std::move(values)), component_(std::move(component)) {}

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
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values_->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            // The `x` is the high 32 bits, the `y` the low 32 bits.
            const auto packed = static_cast<uint64_t>(value.value());
            const auto x = static_cast<double>(XYEncodingUtils::decode(static_cast<int32_t>(packed >> 32U)));
            const auto y = static_cast<double>(XYEncodingUtils::decode(static_cast<int32_t>(static_cast<uint32_t>(packed))));
            if (component_->contains(x, y)) {
                return true;
            }
        }
        return false;
    }

    /// The reference implementation expects a cardesian containment test to be
    /// much more expensive than the box tests of the lat/lon queries.
    [[nodiscard]] float match_cost() const override { return 1000.0F; }

private:
    std::unique_ptr<SortedNumericDocValues> values_;
    std::shared_ptr<Component2D> component_;
};

/// Supplies the two-phase scorer of one leaf.
class GeometryScorerSupplier final : public ScorerSupplier {
public:
    GeometryScorerSupplier(std::unique_ptr<SortedNumericDocValues> values, std::shared_ptr<Component2D> component,
                           float score, ScoreMode score_mode)
            : values_(std::move(values)),
              component_(std::move(component)),
              score_(score),
              score_mode_(score_mode) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        std::unique_ptr<TwoPhaseIterator> iterator =
                std::make_unique<GeometryTwoPhaseIterator>(std::move(values_), component_);
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
    std::shared_ptr<Component2D> component_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

/// The weight of a [`XYDocValuesPointInGeometryQuery`].
///
/// Port of `XYDocValuesPointInGeometryQueryWeight`
/// (`core/document/xy_doc_values_point_in_geometry_query.rs`).
class GeometryWeight final : public Weight {
public:
    GeometryWeight(const XYDocValuesPointInGeometryQuery &query, std::shared_ptr<Component2D> component, float score,
                   ScoreMode score_mode)
            : query_(query), component_(std::move(component)), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        auto supplier = const_cast<GeometryWeight *>(this)->scorer_supplier(context, searcher);
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
        return std::unique_ptr<ScorerSupplier>(
                std::make_unique<GeometryScorerSupplier>(std::move(values.value()), component_, score_, score_mode_));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        auto values = context.reader()->get_sorted_numeric_doc_values(query_.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        return true;
    }

private:
    const XYDocValuesPointInGeometryQuery &query_;
    std::shared_ptr<Component2D> component_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

} // namespace

XYDocValuesPointInGeometryQuery::XYDocValuesPointInGeometryQuery(
        std::string field, std::vector<std::shared_ptr<const Geometry>> geometries,
        std::shared_ptr<Component2D> component2d)
        : field_(std::move(field)), geometries_(std::move(geometries)), component2d_(std::move(component2d)) {}

Result<std::unique_ptr<XYDocValuesPointInGeometryQuery>> XYDocValuesPointInGeometryQuery::create(
        std::string field, std::vector<std::shared_ptr<const Geometry>> geometries) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }
    if (geometries.empty()) {
        return err(LuceneError::illegal_argument("geometries must not be empty"));
    }

    std::vector<const Geometry *> shapes;
    shapes.reserve(geometries.size());
    for (const auto &geometry : geometries) {
        if (geometry == nullptr) {
            return err(LuceneError::illegal_argument("the geometries must not be null"));
        }
        shapes.push_back(geometry.get());
    }
    auto component = create_geometry_component(shapes);
    if (!component.has_value()) {
        return std::unexpected(std::move(component.error()));
    }

    return std::unique_ptr<XYDocValuesPointInGeometryQuery>(new XYDocValuesPointInGeometryQuery(
            std::move(field), std::move(geometries), std::move(component.value())));
}

Result<std::unique_ptr<Weight>> XYDocValuesPointInGeometryQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                              ScoreMode score_mode,
                                                                              float boost) const {
    std::unique_ptr<Weight> weight(new GeometryWeight(*this, component2d_, boost, score_mode));
    return weight;
}

VoidResult XYDocValuesPointInGeometryQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        if (auto visited = visitor.visit_leaf(*this); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> XYDocValuesPointInGeometryQuery::to_string(std::string_view field) const {
    std::string text = "XYDocValuesPointInGeometryQuery:";
    if (field != field_) {
        text += " field=" + field_ + ":";
    }
    text += "[geometries=" + std::to_string(geometries_.size()) + "]";
    return text;
}

bool XYDocValuesPointInGeometryQuery::equals(const Query &other) const {
    const auto *geometry = dynamic_cast<const XYDocValuesPointInGeometryQuery *>(&other);
    if (geometry == nullptr) {
        return false;
    }
    if (field_ != geometry->field_ || geometries_.size() != geometry->geometries_.size()) {
        return false;
    }
    // Geometries are compared by identity, as everywhere else in the port.
    for (size_t i = 0; i < geometries_.size(); ++i) {
        if (geometries_[i].get() != geometry->geometries_[i].get()) {
            return false;
        }
    }
    return true;
}

size_t XYDocValuesPointInGeometryQuery::hash() const {
    return std::hash<std::string>{}(field_) * 31 + geometries_.size();
}

} // namespace lucene
