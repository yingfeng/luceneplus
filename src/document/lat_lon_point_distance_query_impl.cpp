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

module lucene_core:lat_lon_point_distance_query.impl;

import :lat_lon_point_distance_query;

import std;

import :bits;
import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :field_infos;
import :fixed_bit_set;
import :geo_encoding_utils;
import :geo_predicates;
import :geo_rectangle;
import :geo_utils;
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

namespace lucene {

namespace {

/// The same two constants [`LatLonPoint`] is built on.  They are repeated here so
/// that this module does not have to import the field module, which imports this
/// one for the query factory.
constexpr size_t kCoordinateBytes = 4;
constexpr size_t kDimensions = 2;

/// Iterates the set bits of a [`FixedBitSet`] as a document iterator.
class DistanceDocIterator final : public DocIdSetIterator {
public:
    DistanceDocIterator(const FixedBitSet &bits, int64_t cost) : bits_(bits), cost_(cost) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (doc_ == NO_MORE_DOCS) {
            return doc_;
        }
        const int64_t next = bits_.next_set_bit(static_cast<size_t>(doc_ + 1));
        // `next_set_bit` says "no more bits" with -1; a document iterator uses
        // `NO_MORE_DOCS`.
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

/// The circle, in the query's own terms: the bounding boxes and the distance
/// predicate that decide whether a cell or a point is in it.
///
/// Port of `LatLonDistanceContext` (`core/document/lat_lon_point_distance_query.rs`).
struct LatLonDistanceContext {
    double latitude{0.0};
    double longitude{0.0};
    int32_t min_lat{0};
    int32_t max_lat{0};
    /// The bounding box of the longitude range; when the circle crosses the
    /// dateline this is the part from `min_lon` to 180 degrees.
    int32_t min_lon{0};
    int32_t max_lon{0};
    /// The second half of a dateline-crossing range (`-180` to this), or
    /// `INT32_MAX` when there is none.
    int32_t min_lon2{0};
    double sort_key{0.0};
    double axis_lat{0.0};
    std::shared_ptr<const DistancePredicate> distance_predicate;

    /// Whether the encoded point is within the circle.
    [[nodiscard]] bool matches(std::span<const uint8_t> packed_value) const {
        const int32_t lat = NumericUtils::sortable_bytes_to_int(packed_value, 0);
        if (lat > max_lat || lat < min_lat) {
            return false;
        }
        const int32_t lon = NumericUtils::sortable_bytes_to_int(packed_value, kCoordinateBytes);
        if ((lon > max_lon || lon < min_lon) && lon < min_lon2) {
            return false;
        }
        return distance_predicate->test(lat, lon);
    }

    /// How the cell relates to the circle.
    ///
    /// The reference implementation's four steps: reject the cells outside the
    /// bounding box(es), reject the disjoint ones, accept the contained ones, and
    /// let the caller recurse into the rest.
    [[nodiscard]] Result<Relation> relate(std::span<const uint8_t> min_packed_value,
                                          std::span<const uint8_t> max_packed_value) const {
        const int32_t lat_lower = NumericUtils::sortable_bytes_to_int(min_packed_value, 0);
        const int32_t lat_upper = NumericUtils::sortable_bytes_to_int(max_packed_value, 0);
        if (lat_lower > max_lat || lat_upper < min_lat) {
            return Relation::kCellOutsideQuery;
        }

        const int32_t lon_lower = NumericUtils::sortable_bytes_to_int(min_packed_value, kCoordinateBytes);
        const int32_t lon_upper = NumericUtils::sortable_bytes_to_int(max_packed_value, kCoordinateBytes);
        if ((lon_lower > max_lon || lon_upper < min_lon) && lon_upper < min_lon2) {
            return Relation::kCellOutsideQuery;
        }

        return GeoUtils::relate(GeoEncodingUtils::decode_latitude(lat_lower),
                                GeoEncodingUtils::decode_latitude(lat_upper),
                                GeoEncodingUtils::decode_longitude(lon_lower),
                                GeoEncodingUtils::decode_longitude(lon_upper), latitude, longitude, sort_key, axis_lat);
    }
};

/// Collects the documents of the leaf whose point is inside the circle.
class DistanceIntersectVisitor final : public IntersectVisitor {
public:
    DistanceIntersectVisitor(FixedBitSet &result, const LatLonDistanceContext &context)
            : result_(result), context_(context) {}

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        result_.set(static_cast<size_t>(doc_id));
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> packed_value) override {
        if (context_.matches(packed_value)) {
            result_.set(static_cast<size_t>(doc_id));
        }
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> min_packed_value,
                                           std::span<const uint8_t> max_packed_value) const override {
        return context_.relate(min_packed_value, max_packed_value);
    }

private:
    FixedBitSet &result_;
    const LatLonDistanceContext &context_;
};

/// Supplies the scorer over the collected documents.
///
/// **Deviation:** the reference implementation uses a `DocIdSetBuilder` and an
/// inverted bitset when nearly every document matches; the port always collects
/// into a [`FixedBitSet`], which is correct at the cost of one bit per document.
class DistanceScorerSupplier final : public ScorerSupplier {
public:
    DistanceScorerSupplier(std::shared_ptr<PointValues> values, LatLonDistanceContext context, float score,
                           ScoreMode score_mode, int32_t max_doc)
            : values_(std::move(values)),
              context_(std::move(context)),
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
        DistanceIntersectVisitor visitor(result, context_);
        auto intersected = values_->intersect(visitor);
        if (!intersected.has_value()) {
            return std::unexpected(std::move(intersected.error()));
        }

        const int64_t collected = static_cast<int64_t>(result.cardinality());
        // The iterator borrows the bitset, so the bitset is kept alive next to the
        // scorer for as long as the supplier lives.
        bitset_ = std::move(result);
        std::unique_ptr<DocIdSetIterator> iterator = std::make_unique<DistanceDocIterator>(bitset_, collected);
        scorer_ = ConstantScoreScorer::from_disi(score_, score_mode_, std::move(iterator));
        return std::unique_ptr<Scorer>(std::move(scorer_));
    }

    std::shared_ptr<PointValues> values_;
    LatLonDistanceContext context_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
    int32_t max_doc_{0};
    FixedBitSet bitset_;
    std::unique_ptr<Scorer> scorer_;
};

/// The weight of a [`LatLonPointDistanceQuery`].
///
/// Port of `LatLonPointDistanceWeight`
/// (`core/document/lat_lon_point_distance_query.rs`).
class DistanceWeight final : public Weight {
public:
    DistanceWeight(const LatLonPointDistanceQuery &query, LatLonDistanceContext context, float score,
                   ScoreMode score_mode)
            : query_(query), context_(std::move(context)), score_(score), score_mode_(score_mode) {}

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override {
        // `Weight::scorer_supplier` is non-const, so the const `explain` reaches it
        // through a cast.
        auto supplier = const_cast<DistanceWeight *>(this)->scorer_supplier(context, searcher);
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
            // No document of this leaf indexed the field.
            return std::unique_ptr<ScorerSupplier>{};
        }
        // The field must be a lat/lon point (or not a point at all, when the same
        // name was only used for other field kinds).
        const size_t dimensions = field_info->point_dimension_count;
        if (dimensions != 0 && dimensions != kDimensions) {
            return err(LuceneError::illegal_argument(
                    "field=\"" + query_.field() + "\" was indexed with numDims=" + std::to_string(dimensions) +
                    " but this point query can only handle numDims=" + std::to_string(kDimensions)));
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

        return std::unique_ptr<ScorerSupplier>(std::make_unique<DistanceScorerSupplier>(
                values.value(), context_, score_, score_mode_, max_doc.value()));
    }

    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    const LatLonPointDistanceQuery &query_;
    LatLonDistanceContext context_;
    float score_{1.0F};
    ScoreMode score_mode_{ScoreMode::TopScores};
};

/// The bounding boxes and the predicate of a circle, which is what the weight
/// runs on.
[[nodiscard]] Result<LatLonDistanceContext> build_context(double latitude, double longitude, double radius_meters) {
    auto box = Rectangle::from_point_distance(latitude, longitude, radius_meters);
    if (!box.has_value()) {
        return std::unexpected(std::move(box.error()));
    }

    auto min_lat = GeoEncodingUtils::encode_latitude(box.value().min_lat());
    if (!min_lat.has_value()) {
        return std::unexpected(std::move(min_lat.error()));
    }
    auto max_lat = GeoEncodingUtils::encode_latitude(box.value().max_lat());
    if (!max_lat.has_value()) {
        return std::unexpected(std::move(max_lat.error()));
    }

    LatLonDistanceContext context;
    context.latitude = latitude;
    context.longitude = longitude;
    context.min_lat = min_lat.value();
    context.max_lat = max_lat.value();
    context.min_lon2 = std::numeric_limits<int32_t>::max();

    if (box.value().crosses_dateline()) {
        // The longitude range is split: `min_lon` to 180 degrees, and -180 degrees
        // to `min_lon2`.
        auto max_lon = GeoEncodingUtils::encode_longitude(box.value().max_lon());
        if (!max_lon.has_value()) {
            return std::unexpected(std::move(max_lon.error()));
        }
        auto second_min_lon = GeoEncodingUtils::encode_longitude(box.value().min_lon());
        if (!second_min_lon.has_value()) {
            return std::unexpected(std::move(second_min_lon.error()));
        }
        context.min_lon = std::numeric_limits<int32_t>::min();
        context.max_lon = max_lon.value();
        context.min_lon2 = second_min_lon.value();
    } else {
        auto min_lon = GeoEncodingUtils::encode_longitude(box.value().min_lon());
        if (!min_lon.has_value()) {
            return std::unexpected(std::move(min_lon.error()));
        }
        auto max_lon = GeoEncodingUtils::encode_longitude(box.value().max_lon());
        if (!max_lon.has_value()) {
            return std::unexpected(std::move(max_lon.error()));
        }
        context.min_lon = min_lon.value();
        context.max_lon = max_lon.value();
    }

    context.sort_key = GeoUtils::distance_query_sort_key(radius_meters);
    context.axis_lat = Rectangle::axis_lat(latitude, radius_meters);

    auto predicate = create_distance_predicate(latitude, longitude, radius_meters);
    if (!predicate.has_value()) {
        return std::unexpected(std::move(predicate.error()));
    }
    context.distance_predicate = std::make_shared<const DistancePredicate>(std::move(predicate.value()));
    return context;
}

} // namespace

LatLonPointDistanceQuery::LatLonPointDistanceQuery(std::string field, double latitude, double longitude,
                                                   double radius_meters)
        : field_(std::move(field)), latitude_(latitude), longitude_(longitude), radius_meters_(radius_meters) {}

Result<std::unique_ptr<LatLonPointDistanceQuery>> LatLonPointDistanceQuery::create(std::string field, double latitude,
                                                                                  double longitude,
                                                                                  double radius_meters) {
    if (field.empty()) {
        return err(LuceneError::illegal_argument("the field name must not be empty"));
    }
    if (!std::isfinite(radius_meters) || radius_meters < 0.0) {
        return err(LuceneError::illegal_argument("radiusMeters: '" + std::to_string(radius_meters) + "' is invalid"));
    }
    if (auto checked = GeoUtils::check_latitude(latitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    if (auto checked = GeoUtils::check_longitude(longitude); !checked.has_value()) {
        return std::unexpected(std::move(checked.error()));
    }
    return std::unique_ptr<LatLonPointDistanceQuery>(
            new LatLonPointDistanceQuery(std::move(field), latitude, longitude, radius_meters));
}

Result<std::unique_ptr<Weight>> LatLonPointDistanceQuery::create_weight(const IndexSearcher & /*searcher*/,
                                                                       ScoreMode score_mode, float boost) const {
    auto context = build_context(latitude_, longitude_, radius_meters_);
    if (!context.has_value()) {
        return std::unexpected(std::move(context.error()));
    }
    std::unique_ptr<Weight> weight(new DistanceWeight(*this, std::move(context.value()), boost, score_mode));
    return weight;
}

VoidResult LatLonPointDistanceQuery::visit(QueryVisitor &visitor) const {
    if (visitor.accept_field(field_)) {
        if (auto visited = visitor.visit_leaf(*this); !visited.has_value()) {
            return visited;
        }
    }
    return {};
}

Result<std::string> LatLonPointDistanceQuery::to_string(std::string_view field) const {
    std::string text;
    if (field != field_) {
        text += field_ + ":";
    }
    text += std::format("{:.1f}", latitude_);
    text += ",";
    text += std::format("{:.1f}", longitude_);
    text += " +/- ";
    text += std::format("{:.1f}", radius_meters_);
    text += " meters";
    return text;
}

bool LatLonPointDistanceQuery::equals(const Query &other) const {
    const auto *distance = dynamic_cast<const LatLonPointDistanceQuery *>(&other);
    if (distance == nullptr) {
        return false;
    }
    return field_ == distance->field_ && latitude_ == distance->latitude_ && longitude_ == distance->longitude_ &&
           radius_meters_ == distance->radius_meters_;
}

size_t LatLonPointDistanceQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    for (const double value : {latitude_, longitude_, radius_meters_}) {
        result = result * 31 + std::hash<double>{}(value);
    }
    return result;
}

} // namespace lucene
