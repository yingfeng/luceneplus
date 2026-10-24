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

module lucene_core:lat_lon_point_distance_feature_query.impl;

import :lat_lon_point_distance_feature_query;

import std;

import :doc_id_set_iterator;
import :error;
import :explanation;
import :geo_encoding_utils;
import :geo_utils;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :point_values;
import :query;
import :query_visitor;
import :score_mode;
import :scorable;
import :scorer;
import :scorer_supplier;
import :sloppy_math;
import :sorted_numeric_doc_values;
import :types;
import :weight;

namespace lucene {

namespace {

/// The immutable parameters a scorer of this query runs on.
///
/// Port of `QueryMeta` (`core/document/lat_lon_point_distance_feature_query.rs`).
struct QueryMeta {
    std::string field;
    double origin_lat{0.0};
    double origin_lon{0.0};
    double pivot_distance{0.0};

    /// The distance of a document whose packed location is `encoded`.
    [[nodiscard]] double distance_of(int64_t encoded) const {
        // The latitude is the high 32 bits and the longitude the low 32 bits.
        const auto packed = static_cast<uint64_t>(encoded);
        const double latitude = GeoEncodingUtils::decode_latitude(static_cast<int32_t>(packed >> 32U));
        const double longitude = GeoEncodingUtils::decode_longitude(static_cast<int32_t>(static_cast<uint32_t>(packed)));
        return SloppyMath::haversin_meters(origin_lat, origin_lon, latitude, longitude);
    }

    /// The score of a document at `distance`.
    [[nodiscard]] float score_with_distance(double distance, float boost) const {
        return static_cast<float>(static_cast<double>(boost) *
                                  (pivot_distance / (pivot_distance + distance)));
    }
};

/// Scores the documents that have a location, by their distance to the origin.
///
/// Port of `DistanceScorer` (`core/document/lat_lon_point_distance_feature_query.rs`).
///
/// **Deviation:** the reference implementation also implements
/// `set_min_competitive_score`, which binary searches the largest distance that
/// can still compete and skips the documents beyond it.  The port always scores
/// the documents it visits, which is the same answer without the pruning.
class DistanceScorer final : public Scorer {
public:
    DistanceScorer(QueryMeta meta, std::unique_ptr<SortedNumericDocValues> values, float boost)
            : meta_(std::move(meta)), values_(std::move(values)), boost_(boost) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        auto next = values_->next_doc();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        doc_ = next.value();
        if (doc_ != NO_MORE_DOCS) {
            auto selected = select_distance();
            if (!selected.has_value()) {
                return std::unexpected(std::move(selected.error()));
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        auto advanced = values_->advance(target);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        doc_ = advanced.value();
        if (doc_ != NO_MORE_DOCS) {
            auto selected = select_distance();
            if (!selected.has_value()) {
                return std::unexpected(std::move(selected.error()));
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return values_->cost(); }

    [[nodiscard]] Result<float> score() override { return meta_.score_with_distance(distance_, boost_); }

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }

    /// The highest score of any document, which a document at distance `0` gets.
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override {
        return meta_.score_with_distance(0.0, boost_);
    }

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

private:
    /// The closest of the current document's locations.
    ///
    /// Port of the `NumericDocValuesImpl` the reference implementation wraps around
    /// the multi-valued iterator: a document with several locations is scored by
    /// the one closest to the origin.
    [[nodiscard]] VoidResult select_distance() {
        auto count = values_->doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        double closest = std::numeric_limits<double>::infinity();
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values_->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            closest = std::min(closest, meta_.distance_of(value.value()));
        }
        distance_ = closest;
        return {};
    }

    QueryMeta meta_;
    std::unique_ptr<SortedNumericDocValues> values_;
    float boost_{1.0F};
    int32_t doc_{-1};
    double distance_{0.0};
};

/// Supplies the scorer of one leaf.
class FeatureScorerSupplier final : public ScorerSupplier {
public:
    FeatureScorerSupplier(QueryMeta meta, std::unique_ptr<SortedNumericDocValues> values, float boost)
            : meta_(std::move(meta)), values_(std::move(values)), boost_(boost) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        if (values_ == nullptr) {
            return err(LuceneError::illegal_state("the scorer was already built"));
        }
        return std::unique_ptr<Scorer>(std::make_unique<DistanceScorer>(meta_, std::move(values_), boost_));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        if (values_ == nullptr) {
            return 0;
        }
        return values_->cost();
    }

private:
    QueryMeta meta_;
    std::unique_ptr<SortedNumericDocValues> values_;
    float boost_{1.0F};
};

/// The weight of a [`LatLonPointDistanceFeatureQuery`].
///
/// Port of `LatLonPointDistanceFeatureWeight`
/// (`core/document/lat_lon_point_distance_feature_query.rs`).
class FeatureWeight final : public Weight {
public:
    FeatureWeight(const LatLonPointDistanceFeatureQuery &query, QueryMeta meta, float boost)
            : query_(query), meta_(std::move(meta)), boost_(boost) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        auto values = context.reader()->get_sorted_numeric_doc_values(meta_.field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return Explanation::no_match_no_details("no doc values for field " + meta_.field);
        }
        auto advanced = values.value()->advance(doc);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (advanced.value() != doc) {
            return Explanation::no_match_no_details("Document " + std::to_string(doc) +
                                                    " doesn't have a value for field " + meta_.field);
        }

        // The closest of the document's locations decides the score.
        auto count = values.value()->doc_value_count();
        if (!count.has_value()) {
            return std::unexpected(std::move(count.error()));
        }
        double distance = std::numeric_limits<double>::infinity();
        for (int32_t i = 0; i < count.value(); ++i) {
            auto value = values.value()->next_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            distance = std::min(distance, meta_.distance_of(value.value()));
        }

        (void)searcher;
        return Explanation::match_(
                meta_.score_with_distance(distance, boost_),
                "Distance score, computed as weight * pivotDistance / (pivotDistance + abs(distance)) from:",
                {Explanation::match_no_details(boost_, "weight"),
                 Explanation::match_no_details(meta_.pivot_distance, "pivotDistance"),
                 Explanation::match_no_details(meta_.origin_lat, "originLat"),
                 Explanation::match_no_details(meta_.origin_lon, "originLon"),
                 Explanation::match_no_details(distance, "distance")});
    }

    [[nodiscard]] const Query &get_query() const override { return query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                         const IndexSearcher & /*searcher*/) override {
        LeafReader *reader = context.reader();
        // The field has to be indexed as points as well: a leaf without point
        // values cannot answer the query at all.
        auto point_values = reader->point_values(meta_.field);
        if (!point_values.has_value()) {
            return std::unexpected(std::move(point_values.error()));
        }
        if (point_values.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }

        auto values = reader->get_sorted_numeric_doc_values(meta_.field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() == nullptr) {
            return std::unique_ptr<ScorerSupplier>{};
        }
        return std::unique_ptr<ScorerSupplier>(
                std::make_unique<FeatureScorerSupplier>(meta_, std::move(values.value()), boost_));
    }

    /// The scores depend on the origin, so the weight is not cacheable.
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return false; }

private:
    const LatLonPointDistanceFeatureQuery &query_;
    QueryMeta meta_;
    float boost_{1.0F};
};

} // namespace

LatLonPointDistanceFeatureQuery::LatLonPointDistanceFeatureQuery(std::string field, double origin_lat,
                                                                double origin_lon, double pivot_distance)
        : field_(std::move(field)),
          origin_lat_(origin_lat),
          origin_lon_(origin_lon),
          pivot_distance_(pivot_distance) {}

Result<std::unique_ptr<LatLonPointDistanceFeatureQuery>> LatLonPointDistanceFeatureQuery::create(
        std::string field, double origin_lat, double origin_lon, double pivot_distance) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }
    if (auto checked = GeoUtils::check_latitude(origin_lat); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(origin_lon); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (!(pivot_distance > 0.0)) {
        return err(LuceneError::illegal_argument("pivotDistance must be > 0, got " +
                                                 std::to_string(pivot_distance)));
    }
    return std::unique_ptr<LatLonPointDistanceFeatureQuery>(
            new LatLonPointDistanceFeatureQuery(std::move(field), origin_lat, origin_lon, pivot_distance));
}

Result<std::unique_ptr<Weight>> LatLonPointDistanceFeatureQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                              ScoreMode /*score_mode*/,
                                                                              float boost) const {
    QueryMeta meta;
    meta.field = field_;
    meta.origin_lat = origin_lat_;
    meta.origin_lon = origin_lon_;
    meta.pivot_distance = pivot_distance_;
    std::unique_ptr<Weight> weight(new FeatureWeight(*this, std::move(meta), boost));
    return weight;
}

VoidResult LatLonPointDistanceFeatureQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        if (auto visited = visitor.visit_leaf(*this); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> LatLonPointDistanceFeatureQuery::to_string(std::string_view /*field*/) const {
    return "LatLonPointDistanceFeatureQuery(field=" + field_ + ",originLat=" + std::format("{}", origin_lat_) +
           ",originLon=" + std::format("{}", origin_lon_) + ",pivotDistance=" +
           std::format("{}", pivot_distance_) + ")";
}

bool LatLonPointDistanceFeatureQuery::equals(const Query &other) const {
    const auto *feature = dynamic_cast<const LatLonPointDistanceFeatureQuery *>(&other);
    if (feature == nullptr) {
        return false;
    }
    return field_ == feature->field_ && origin_lat_ == feature->origin_lat_ &&
           origin_lon_ == feature->origin_lon_ && pivot_distance_ == feature->pivot_distance_;
}

size_t LatLonPointDistanceFeatureQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    for (const double value : {origin_lat_, origin_lon_, pivot_distance_}) {
        result = result * 31 + std::hash<double>{}(value);
    }
    return result;
}

} // namespace lucene
