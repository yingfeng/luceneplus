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

module lucene_core:xy_point_in_geometry_query.impl;

import :xy_point_in_geometry_query;

import std;

import :bits;
import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :field_infos;
import :fixed_bit_set;
import :geo_component2d;
import :geo_geometry;
import :identity;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :numeric_utils;
import :point_values;
import :query;
import :query_visitor;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :types;
import :weight;
import :xy_encoding_utils;

namespace lucene {

namespace {

/// The two constants an XY point is built on, repeated here so this module does
/// not have to import the field module (which imports this one).
constexpr size_t kCoordinateBytes = 4;
constexpr size_t kDimensions = 2;

/// Iterates the set bits of a [`FixedBitSet`] as a document iterator.
class GeometryDocIterator final : public DocIdSetIterator {
public:
    GeometryDocIterator(const FixedBitSet &bits, int64_t cost) : bits_(bits), cost_(cost) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (doc_ == NO_MORE_DOCS) {
            return doc_;
        }
        const int64_t next = bits_.next_set_bit(static_cast<size_t>(doc_ + 1));
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

/// Collects the documents whose point is inside the query shapes.
///
/// Port of `XYPointInGeometryIntersectVisitor`
/// (`core/document/xy_point_in_geometry_query.rs`).
class GeometryIntersectVisitor final : public IntersectVisitor {
public:
    GeometryIntersectVisitor(FixedBitSet &result, const Component2D &component)
            : result_(result), component_(component) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        result_.set(static_cast<size_t>(doc_id));
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        const double x = static_cast<double>(XYEncodingUtils::decode_bytes(packed_value, 0));
        const double y = static_cast<double>(XYEncodingUtils::decode_bytes(packed_value, kCoordinateBytes));
        if (component_.contains(x, y)) {
            result_.set(static_cast<size_t>(doc_id));
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        const double cell_min_x = static_cast<double>(XYEncodingUtils::decode_bytes(min_packed_value, 0));
        const double cell_min_y =
                static_cast<double>(XYEncodingUtils::decode_bytes(min_packed_value, kCoordinateBytes));
        const double cell_max_x = static_cast<double>(XYEncodingUtils::decode_bytes(max_packed_value, 0));
        const double cell_max_y =
                static_cast<double>(XYEncodingUtils::decode_bytes(max_packed_value, kCoordinateBytes));
        return component_.relate(cell_min_x, cell_max_x, cell_min_y, cell_max_y);
    }

private:
    FixedBitSet &result_;
    const Component2D &component_;
};

/// Supplies the scorer over the collected documents.
///
/// **Deviation:** the reference implementation collects into a
/// `DocIdSetBuilder`; the port collects into a [`FixedBitSet`] and takes the cost
/// from the collected count.
class GeometryScorerSupplier final : public ScorerSupplier {
public:
    GeometryScorerSupplier(std::shared_ptr<PointValues> values, std::shared_ptr<Component2D> component, float score,
                           ScoreMode score_mode, int32_t max_doc)
            : values_(std::move(values)),
              component_(std::move(component)),
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
        return scorer_cost.value();
    }

private:
    [[nodiscard]] Result<std::unique_ptr<Scorer>> get_scorer() {
        if (scorer_ != nullptr) {
            return std::unique_ptr<Scorer>(std::move(scorer_));
        }

        FixedBitSet result(static_cast<size_t>(max_doc_));
        GeometryIntersectVisitor visitor(result, *component_);
        auto intersected = values_->intersect(visitor);
        if (!intersected.has_value()) {
            return std::unexpected(std::move(intersected.error()));
        }

        const int64_t collected = static_cast<int64_t>(result.cardinality());
        bitset_ = std::move(result);
        std::unique_ptr<DocIdSetIterator> iterator = std::make_unique<GeometryDocIterator>(bitset_, collected);
        scorer_ = ConstantScoreScorer::from_disi(score_, score_mode_, std::move(iterator));
        return std::unique_ptr<Scorer>(std::move(scorer_));
    }

    std::shared_ptr<PointValues> values_;
    std::shared_ptr<Component2D> component_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
    int32_t max_doc_{0};
    FixedBitSet bitset_;
    std::unique_ptr<Scorer> scorer_;
};

/// The weight of a [`XYPointInGeometryQuery`].
///
/// Port of `XYPointInGeometryWeight`
/// (`core/document/xy_point_in_geometry_query.rs`).
class GeometryWeight final : public Weight {
public:
    GeometryWeight(const XYPointInGeometryQuery &query, std::shared_ptr<Component2D> component, float score,
                   ScoreMode score_mode)
            : query_(query), component_(std::move(component)), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        auto supplier = const_cast<GeometryWeight *>(this)->scorer_supplier(context, searcher);
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
        LeafReader *reader = context.reader();
        auto field_infos = reader->field_infos();
        if (!field_infos.has_value()) {
            return std::unexpected(std::move(field_infos.error()));
        }
        if (field_infos.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        const FieldInfo *field_info = field_infos.value()->by_name(query_.field());
        if (field_info == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        // The field must be an XY point, unless the name was only used for other
        // field kinds (in which case the dimensions are zero).
        if (field_info->point_dimension_count != 0 && field_info->point_dimension_count != kDimensions) {
            return err(LuceneError::illegal_argument(
                    "field=\"" + query_.field() + "\" was indexed with numDims=" +
                    std::to_string(field_info->point_dimension_count) + " but this point type has numDims=" +
                    std::to_string(kDimensions) + ", is the field really a XYPoint?"));
        }
        if (field_info->point_num_bytes != 0 && field_info->point_num_bytes != kCoordinateBytes) {
            return err(LuceneError::illegal_argument(
                    "field=\"" + query_.field() + "\" was indexed with bytesPerDim=" +
                    std::to_string(field_info->point_num_bytes) + " but this point type has bytesPerDim=" +
                    std::to_string(kCoordinateBytes) + ", is the field really a XYPoint?"));
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

        return std::unique_ptr<ScorerSupplier>(std::make_unique<GeometryScorerSupplier>(
                values.value(), component_, score_, score_mode_, max_doc.value()));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const XYPointInGeometryQuery &query_;
    std::shared_ptr<Component2D> component_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

} // namespace

XYPointInGeometryQuery::XYPointInGeometryQuery(std::string field,
                                               std::vector<std::shared_ptr<const Geometry>> geometries,
                                               std::shared_ptr<Component2D> component2d)
        : field_(std::move(field)), geometries_(std::move(geometries)), component2d_(std::move(component2d)) {}

Result<std::unique_ptr<XYPointInGeometryQuery>> XYPointInGeometryQuery::create(
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

    return std::unique_ptr<XYPointInGeometryQuery>(
            new XYPointInGeometryQuery(std::move(field), std::move(geometries), std::move(component.value())));
}

Result<std::unique_ptr<Weight>> XYPointInGeometryQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                     ScoreMode score_mode, float boost) const {
    std::unique_ptr<Weight> weight(new GeometryWeight(*this, component2d_, boost, score_mode));
    return weight;
}

VoidResult XYPointInGeometryQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        if (auto visited = visitor.visit_leaf(*this); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> XYPointInGeometryQuery::to_string(std::string_view field) const {
    std::string text = "XYPointInGeometryQuery:";
    if (field != field_) {
        text += " field=" + field_ + ":";
    }
    text += "[geometries=" + std::to_string(geometries_.size()) + "]";
    return text;
}

bool XYPointInGeometryQuery::equals(const Query &other) const {
    const auto *geometry = dynamic_cast<const XYPointInGeometryQuery *>(&other);
    if (geometry == nullptr) {
        return false;
    }
    if (field_ != geometry->field_ || geometries_.size() != geometry->geometries_.size()) {
        return false;
    }
    // Geometries are compared by identity (see the deviation noted on `equals`).
    for (size_t i = 0; i < geometries_.size(); ++i) {
        if (geometries_[i].get() != geometry->geometries_[i].get()) {
            return false;
        }
    }
    return true;
}

size_t XYPointInGeometryQuery::hash() const {
    return std::hash<std::string>{}(field_) * 31 + geometries_.size();
}

} // namespace lucene
