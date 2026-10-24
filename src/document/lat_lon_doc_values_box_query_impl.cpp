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

module lucene_core:lat_lon_doc_values_box_query.impl;

import :lat_lon_doc_values_box_query;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :geo_encoding_utils;
import :geo_utils;
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

namespace lucene {

namespace {

/// Confirms the candidates the doc values iterator proposes.
///
/// Port of `LatLonDocValuesBoxTwoPhaseIterator`
/// (`core/document/lat_lon_doc_values_box_query.rs`): a document matches when one
/// of its values decodes to a location inside the box.
class BoxTwoPhaseIterator final : public TwoPhaseIterator {
public:
    BoxTwoPhaseIterator(std::unique_ptr<SortedNumericDocValues> values, int32_t min_latitude, int32_t max_latitude,
                        int32_t min_longitude, int32_t max_longitude, bool crosses_dateline)
            : values_(std::move(values)),
              min_latitude_(min_latitude),
              max_latitude_(max_latitude),
              min_longitude_(min_longitude),
              max_longitude_(max_longitude),
              crosses_dateline_(crosses_dateline) {}

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
            // The latitude is the high 32 bits, the longitude the low 32 bits.
            const auto packed = static_cast<uint64_t>(value.value());
            const auto latitude = static_cast<int32_t>(packed >> 32U);
            if (latitude < min_latitude_ || latitude > max_latitude_) {
                continue;
            }

            const auto longitude = static_cast<int32_t>(static_cast<uint32_t>(packed));
            if (crosses_dateline_) {
                if (longitude > max_longitude_ && longitude < min_longitude_) {
                    continue;
                }
            } else if (longitude < min_longitude_ || longitude > max_longitude_) {
                continue;
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] float match_cost() const override { return 5.0F; }

private:
    std::unique_ptr<SortedNumericDocValues> values_;
    int32_t min_latitude_;
    int32_t max_latitude_;
    int32_t min_longitude_;
    int32_t max_longitude_;
    bool crosses_dateline_;
};

/// Supplies the two-phase scorer of one leaf.
class BoxScorerSupplier final : public ScorerSupplier {
public:
    BoxScorerSupplier(std::unique_ptr<SortedNumericDocValues> values, const LatLonDocValuesBoxQuery &query,
                      float score, ScoreMode score_mode)
            : values_(std::move(values)), query_(query), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        std::unique_ptr<TwoPhaseIterator> iterator =
                std::make_unique<BoxTwoPhaseIterator>(std::move(values_), query_.min_latitude(), query_.max_latitude(),
                                                      query_.min_longitude(), query_.max_longitude(),
                                                      query_.crosses_dateline());
        return ConstantScoreScorer::from_tpi(score_, score_mode_, std::move(iterator));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return values_->cost();
    }

private:
    std::unique_ptr<SortedNumericDocValues> values_;
    const LatLonDocValuesBoxQuery &query_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

/// The weight of a [`LatLonDocValuesBoxQuery`].
///
/// Port of `LatLonDocValuesBoxQueryWeight`
/// (`core/document/lat_lon_doc_values_box_query.rs`).
class BoxWeight final : public Weight {
public:
    BoxWeight(const LatLonDocValuesBoxQuery &query, float score, ScoreMode score_mode)
            : query_(query), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        auto supplier = const_cast<BoxWeight *>(this)->scorer_supplier(context, searcher);
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
                std::make_unique<BoxScorerSupplier>(std::move(values.value()), query_, score_, score_mode_));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        // The doc values of the field decide whether the weight can be cached.
        auto values = context.reader()->get_sorted_numeric_doc_values(query_.field());
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        return true;
    }

private:
    const LatLonDocValuesBoxQuery &query_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

} // namespace

LatLonDocValuesBoxQuery::LatLonDocValuesBoxQuery(std::string field, int32_t min_latitude, int32_t max_latitude,
                                                 int32_t min_longitude, int32_t max_longitude, bool crosses_dateline)
        : field_(std::move(field)),
          min_latitude_(min_latitude),
          max_latitude_(max_latitude),
          min_longitude_(min_longitude),
          max_longitude_(max_longitude),
          crosses_dateline_(crosses_dateline) {}

Result<std::unique_ptr<LatLonDocValuesBoxQuery>> LatLonDocValuesBoxQuery::create(std::string field,
                                                                                double min_latitude,
                                                                                double max_latitude,
                                                                                double min_longitude,
                                                                                double max_longitude) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }
    if (auto checked = GeoUtils::check_latitude(min_latitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_latitude(max_latitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(min_longitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(max_longitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (max_latitude < min_latitude) {
        return err(LuceneError::illegal_argument("maxLatitude must not be lower than minLatitude, got " +
                                                 std::to_string(max_latitude) + " < " + std::to_string(min_latitude)));
    }

    // A box whose minimum longitude is larger than its maximum crosses the
    // dateline, which is allowed.
    const bool crosses_dateline = min_longitude > max_longitude;

    auto encoded_min_latitude = GeoEncodingUtils::encode_latitude_ceil(min_latitude);
    if (!encoded_min_latitude.has_value()) {
        return std::unexpected(std::move(encoded_min_latitude.error()));
    }
    auto encoded_max_latitude = GeoEncodingUtils::encode_latitude(max_latitude);
    if (!encoded_max_latitude.has_value()) {
        return std::unexpected(std::move(encoded_max_latitude.error()));
    }
    auto encoded_min_longitude = GeoEncodingUtils::encode_longitude_ceil(min_longitude);
    if (!encoded_min_longitude.has_value()) {
        return std::unexpected(std::move(encoded_min_longitude.error()));
    }
    auto encoded_max_longitude = GeoEncodingUtils::encode_longitude(max_longitude);
    if (!encoded_max_longitude.has_value()) {
        return std::unexpected(std::move(encoded_max_longitude.error()));
    }

    return std::unique_ptr<LatLonDocValuesBoxQuery>(
            new LatLonDocValuesBoxQuery(std::move(field), encoded_min_latitude.value(), encoded_max_latitude.value(),
                                        encoded_min_longitude.value(), encoded_max_longitude.value(),
                                        crosses_dateline));
}

Result<std::unique_ptr<Weight>> LatLonDocValuesBoxQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                      ScoreMode score_mode, float boost) const {
    std::unique_ptr<Weight> weight(new BoxWeight(*this, boost, score_mode));
    return weight;
}

VoidResult LatLonDocValuesBoxQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        if (auto visited = visitor.visit_leaf(*this); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> LatLonDocValuesBoxQuery::to_string(std::string_view field) const {
    std::string text;
    if (field != field_) {
        text += field_ + ":";
    }
    text += "box(minLat=" + std::format("{}", GeoEncodingUtils::decode_latitude(min_latitude_));
    text += ", maxLat=" + std::format("{}", GeoEncodingUtils::decode_latitude(max_latitude_));
    text += ", minLon=" + std::format("{}", GeoEncodingUtils::decode_longitude(min_longitude_));
    text += ", maxLon=" + std::format("{}", GeoEncodingUtils::decode_longitude(max_longitude_));
    text += ")";
    return text;
}

bool LatLonDocValuesBoxQuery::equals(const Query &other) const {
    const auto *box = dynamic_cast<const LatLonDocValuesBoxQuery *>(&other);
    if (box == nullptr) {
        return false;
    }
    return field_ == box->field_ && crosses_dateline_ == box->crosses_dateline_ &&
           min_latitude_ == box->min_latitude_ && max_latitude_ == box->max_latitude_ &&
           min_longitude_ == box->min_longitude_ && max_longitude_ == box->max_longitude_;
}

size_t LatLonDocValuesBoxQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    for (const int32_t value : {min_latitude_, max_latitude_, min_longitude_, max_longitude_}) {
        result = result * 31 + static_cast<size_t>(value);
    }
    result = result * 31 + static_cast<size_t>(crosses_dateline_);
    return result;
}

} // namespace lucene
