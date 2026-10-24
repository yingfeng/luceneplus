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

module lucene_core:index_sort_sorted_numeric_doc_values_range_query.impl;

import :index_sort_sorted_numeric_doc_values_range_query;

import std;

import :constant_score_scorer;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :field_exists_query;
import :index_searcher;
import :leaf_metadata;
import :leaf_reader;
import :leaf_reader_context;
import :match_all_docs_query;
import :query;
import :query_visitor;
import :scorable;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :sorted_numeric_doc_values;
import :sort_field;
import :types;
import :weight;

namespace lucene {

namespace {

/// The contiguous run `[first, last)` of document ids.
class DocRangeDisi final : public DocIdSetIterator {
public:
    DocRangeDisi(int32_t first, int32_t last) : first_(first), last_(last) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (doc_ == NO_MORE_DOCS) {
            return doc_;
        }
        // The iterator starts unpositioned (`doc_id() == -1`), as every
        // `DocIdSetIterator` must, so the first step lands on `first_`.
        doc_ = doc_ < 0 ? first_ : doc_ + 1;
        if (doc_ >= last_) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (doc_ >= target) {
            return doc_;
        }
        doc_ = std::max(target, first_);
        if (doc_ >= last_) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override {
        return last_ > first_ ? static_cast<int64_t>(last_ - first_) : 0;
    }

private:
    int32_t first_;
    int32_t last_;
    int32_t doc_{-1};
};

/// The single value of `doc`, or `nullopt` when the document has none.
///
/// A fresh iterator is opened per probe, because doc values only move forward and
/// the binary search jumps back and forth (the reference implementation does the
/// same by re-creating its comparator on every comparison).
Result<std::optional<int64_t>> value_at(LeafReader &reader, const std::string &field, int32_t doc) {
    auto values = reader.get_sorted_numeric_doc_values(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return std::optional<int64_t>{};
    }
    auto exact = values.value()->advance_exact(doc);
    if (!exact.has_value()) {
        return std::unexpected(std::move(exact.error()));
    }
    if (!exact.value()) {
        return std::optional<int64_t>{};
    }
    auto count = values.value()->doc_value_count();
    if (!count.has_value()) {
        return std::unexpected(std::move(count.error()));
    }
    if (count.value() < 1) {
        return std::optional<int64_t>{};
    }
    auto value = values.value()->next_value();
    if (!value.has_value()) {
        return std::unexpected(std::move(value.error()));
    }
    return std::optional<int64_t>(value.value());
}

/// Whether `metadata` says the segment is sorted by `field`, ascending, on an
/// `int` or `long`.
bool sorted_by_field(const LeafMetaData &metadata, const std::string &field) {
    if (metadata.sort() == nullptr) {
        return false;
    }
    const std::vector<SortField> &fields = metadata.sort()->fields();
    if (fields.empty()) {
        return false;
    }
    const SortField &sort_field = fields[0];
    if (!sort_field.field().has_value() || sort_field.field().value() != field) {
        return false;
    }
    return sort_field.type() == SortFieldType::kInt || sort_field.type() == SortFieldType::kLong;
}

/// The contiguous run of documents whose value is in `[lower, upper]`, or
/// `nullopt` when the shortcut cannot be trusted for this leaf.
Result<std::optional<std::pair<int32_t, int32_t>>> search_range(LeafReader &reader, const std::string &field,
                                                               int64_t lower, int64_t upper) {
    const std::optional<std::pair<int32_t, int32_t>> no_shortcut;
    if (!sorted_by_field(reader.leaf_meta_data(), field)) {
        return no_shortcut;
    }
    if (lower > upper) {
        return std::make_optional(std::make_pair(0, 0));
    }

    auto max_doc = reader.max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    if (max_doc.value() == 0) {
        return std::make_optional(std::make_pair(0, 0));
    }

    auto probe = reader.get_sorted_numeric_doc_values(field);
    if (!probe.has_value()) {
        return std::unexpected(std::move(probe.error()));
    }
    if (probe.value() == nullptr || !probe.value()->is_single_valued()) {
        return no_shortcut;
    }

    // A document without a value sorts as 0, so the run is only exact when 0 is
    // outside the range.
    if (lower <= 0 && 0 <= upper) {
        return no_shortcut;
    }

    // The binary search assumes an ascending run; a reverse-sorted (or broken)
    // index goes through the fallback.
    auto first_value = value_at(reader, field, 0);
    if (!first_value.has_value()) {
        return std::unexpected(std::move(first_value.error()));
    }
    auto last_value = value_at(reader, field, max_doc.value() - 1);
    if (!last_value.has_value()) {
        return std::unexpected(std::move(last_value.error()));
    }
    if (!first_value.value().has_value() || !last_value.value().has_value()) {
        return no_shortcut;
    }
    if (first_value.value().value() > last_value.value().value()) {
        return no_shortcut;
    }

    // First document with a value >= lower.
    int32_t low = 0;
    int32_t high = max_doc.value() - 1;
    while (low <= high) {
        const int32_t mid = (low + high) >> 1;
        auto value = value_at(reader, field, mid);
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        if (!value.value().has_value()) {
            return no_shortcut;
        }
        if (value.value().value() >= lower) {
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    const int32_t first = low;

    // First document with a value > upper.
    low = first;
    high = max_doc.value() - 1;
    while (low <= high) {
        const int32_t mid = (low + high) >> 1;
        auto value = value_at(reader, field, mid);
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        if (!value.value().has_value()) {
            return no_shortcut;
        }
        if (value.value().value() > upper) {
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    const int32_t last = low;

    // Verify the boundaries: a run that does not describe a monotonic block means
    // the shortcut cannot be trusted.
    if (first > 0) {
        auto before = value_at(reader, field, first - 1);
        if (!before.has_value()) {
            return std::unexpected(std::move(before.error()));
        }
        if (!before.value().has_value() || before.value().value() >= lower) {
            return no_shortcut;
        }
    }
    if (first == last) {
        if (first < max_doc.value()) {
            auto at = value_at(reader, field, first);
            if (!at.has_value()) {
                return std::unexpected(std::move(at.error()));
            }
            if (at.value().has_value() && at.value().value() <= upper) {
                return no_shortcut;
            }
        }
        return std::make_optional(std::make_pair(first, last));
    }
    auto first_in_run = value_at(reader, field, first);
    if (!first_in_run.has_value()) {
        return std::unexpected(std::move(first_in_run.error()));
    }
    if (!first_in_run.value().has_value() || first_in_run.value().value() < lower ||
        first_in_run.value().value() > upper) {
        return no_shortcut;
    }
    if (last < max_doc.value()) {
        auto after = value_at(reader, field, last);
        if (!after.has_value()) {
            return std::unexpected(std::move(after.error()));
        }
        if (after.value().has_value() && after.value().value() <= upper) {
            return no_shortcut;
        }
    }
    return std::make_optional(std::make_pair(first, last));
}

/// Hands out a constant-score scorer over a pre-computed run.
class RangeScorerSupplier final : public ScorerSupplier {
public:
    RangeScorerSupplier(float score, ScoreMode score_mode, int32_t first, int32_t last)
        : score_(score), score_mode_(score_mode), first_(first), last_(last) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                      const IndexSearcher & /*searcher*/) override {
        return ConstantScoreScorer::from_disi(score_, score_mode_, std::make_unique<DocRangeDisi>(first_, last_));
    }

    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                       const IndexSearcher & /*searcher*/) override {
        return last_ > first_ ? static_cast<int64_t>(last_ - first_) : 0;
    }

private:
    float score_;
    ScoreMode score_mode_;
    int32_t first_;
    int32_t last_;
};

} // namespace

// ---------------- IndexSortSortedNumericDocValuesRangeQuery ----------------

IndexSortSortedNumericDocValuesRangeQuery::IndexSortSortedNumericDocValuesRangeQuery(
        std::string field, int64_t lower_value, int64_t upper_value, std::shared_ptr<Query> fallback_query)
    : field_(std::move(field)),
      lower_value_(lower_value),
      upper_value_(upper_value),
      fallback_query_(std::move(fallback_query)) {}

Result<std::unique_ptr<Weight>> IndexSortSortedNumericDocValuesRangeQuery::create_weight(
        const IndexSearcher &searcher, ScoreMode score_mode, float boost) const {
    auto fallback = fallback_query_->create_weight(searcher, score_mode, boost);
    if (!fallback.has_value()) {
        return std::unexpected(std::move(fallback.error()));
    }
    return IndexSortSortedNumericDocValuesRangeQueryWeight::create(*this, field_, lower_value_, upper_value_,
                                                                  std::move(fallback.value()), boost, score_mode);
}

Result<std::shared_ptr<Query>> IndexSortSortedNumericDocValuesRangeQuery::rewrite(
        const IndexSearcher &searcher) const {
    if (lower_value_ == std::numeric_limits<int64_t>::min() && upper_value_ == std::numeric_limits<int64_t>::max()) {
        return std::shared_ptr<Query>(std::make_shared<FieldExistsQuery>(field_));
    }

    auto rewritten = fallback_query_->rewrite(searcher);
    if (!rewritten.has_value()) {
        return std::unexpected(std::move(rewritten.error()));
    }
    const Query *target = rewritten.value() != nullptr ? rewritten.value().get() : fallback_query_.get();
    if (dynamic_cast<const MatchAllDocsQuery *>(target) != nullptr) {
        return std::shared_ptr<Query>(std::make_shared<MatchAllDocsQuery>());
    }
    if (rewritten.value() != nullptr) {
        return std::shared_ptr<Query>(std::make_shared<IndexSortSortedNumericDocValuesRangeQuery>(
                field_, lower_value_, upper_value_, rewritten.value()));
    }
    return std::shared_ptr<Query>{};
}

VoidResult IndexSortSortedNumericDocValuesRangeQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(field_)) {
        return {};
    }
    if (auto visited = visitor.visit_leaf(*this); !visited.has_value()) {
        return visited;
    }
    return fallback_query_->visit(visitor);
}

Result<std::string> IndexSortSortedNumericDocValuesRangeQuery::to_string(std::string_view field) const {
    std::string result;
    if (field_ != field) {
        result += field_;
        result += ':';
    }
    result += "[" + std::to_string(lower_value_) + " TO " + std::to_string(upper_value_) + "]";
    return result;
}

bool IndexSortSortedNumericDocValuesRangeQuery::equals(const Query &other) const {
    const auto *range = dynamic_cast<const IndexSortSortedNumericDocValuesRangeQuery *>(&other);
    if (range == nullptr) {
        return false;
    }
    return field_ == range->field_ && lower_value_ == range->lower_value_ && upper_value_ == range->upper_value_ &&
           fallback_query_->equals(*range->fallback_query_);
}

size_t IndexSortSortedNumericDocValuesRangeQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    result = result * 31 + static_cast<size_t>(lower_value_);
    result = result * 31 + static_cast<size_t>(upper_value_);
    result = result * 31 + fallback_query_->hash();
    return result;
}

// ---------------- IndexSortSortedNumericDocValuesRangeQueryWeight ----------------

IndexSortSortedNumericDocValuesRangeQueryWeight::IndexSortSortedNumericDocValuesRangeQueryWeight(
        const Query &parent_query, std::string field, int64_t lower_value, int64_t upper_value,
        std::unique_ptr<Weight> fallback_weight, float boost, ScoreMode score_mode)
    : parent_query_(&parent_query),
      field_(std::move(field)),
      lower_value_(lower_value),
      upper_value_(upper_value),
      fallback_weight_(std::move(fallback_weight)),
      boost_(boost),
      score_mode_(score_mode) {}

Result<std::unique_ptr<IndexSortSortedNumericDocValuesRangeQueryWeight>>
IndexSortSortedNumericDocValuesRangeQueryWeight::create(const Query &parent_query, std::string field,
                                                        int64_t lower_value, int64_t upper_value,
                                                        std::unique_ptr<Weight> fallback_weight, float boost,
                                                        ScoreMode score_mode) {
    return std::unique_ptr<IndexSortSortedNumericDocValuesRangeQueryWeight>(
            new IndexSortSortedNumericDocValuesRangeQueryWeight(parent_query, std::move(field), lower_value, upper_value,
                                                                std::move(fallback_weight), boost, score_mode));
}

Result<std::unique_ptr<ScorerSupplier>> IndexSortSortedNumericDocValuesRangeQueryWeight::scorer_supplier(
        const LeafReaderContext &context, const IndexSearcher &searcher) {
    auto range = search_range(*context.reader(), field_, lower_value_, upper_value_);
    if (!range.has_value()) {
        return std::unexpected(std::move(range.error()));
    }
    if (range.value().has_value()) {
        const auto [first, last] = range.value().value();
        if (first >= last) {
            return std::unique_ptr<ScorerSupplier>{}; // The run is empty: no matches.
        }
        return std::unique_ptr<ScorerSupplier>(new RangeScorerSupplier(boost_, score_mode_, first, last));
    }
    return fallback_weight_->scorer_supplier(context, searcher);
}

Result<int32_t> IndexSortSortedNumericDocValuesRangeQueryWeight::count(const LeafReaderContext &context,
                                                                       const IndexSearcher &searcher) {
    auto has_deletions = context.reader()->has_deletions();
    if (!has_deletions.has_value()) {
        return std::unexpected(std::move(has_deletions.error()));
    }
    if (!has_deletions.value()) {
        auto range = search_range(*context.reader(), field_, lower_value_, upper_value_);
        if (!range.has_value()) {
            return std::unexpected(std::move(range.error()));
        }
        if (range.value().has_value()) {
            const auto [first, last] = range.value().value();
            return last > first ? last - first : 0;
        }
    }
    return fallback_weight_->count(context, searcher);
}

Result<Explanation> IndexSortSortedNumericDocValuesRangeQueryWeight::explain(const LeafReaderContext &context,
                                                                             int32_t doc,
                                                                             const IndexSearcher &searcher) const {
    auto range = search_range(*context.reader(), field_, lower_value_, upper_value_);
    if (!range.has_value()) {
        return std::unexpected(std::move(range.error()));
    }

    if (range.value().has_value()) {
        const auto [first, last] = range.value().value();
        if (doc < first || doc >= last) {
            return Explanation::no_match_no_details("the document is outside the sorted run");
        }
        return Explanation::match_no_details(boost_, "match on an index-sorted range of " + field_);
    }

    // The shortcut does not apply here; ask the fallback weight.  The `const_cast`
    // is safe: the fallback weight is owned by this (non-const) object.
    auto supplier = const_cast<Weight &>(*fallback_weight_).scorer_supplier(context, searcher);
    if (!supplier.has_value()) {
        return std::unexpected(std::move(supplier.error()));
    }
    if (supplier.value() == nullptr) {
        return Explanation::no_match_no_details("the fallback query cannot match");
    }
    auto scorer = supplier.value()->get(0, context, searcher);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    auto advanced = scorer.value()->advance(doc);
    if (!advanced.has_value()) {
        return std::unexpected(std::move(advanced.error()));
    }
    if (advanced.value() != doc) {
        return Explanation::no_match_no_details("the fallback query does not match");
    }
    auto score = scorer.value()->score();
    if (!score.has_value()) {
        return std::unexpected(std::move(score.error()));
    }
    return Explanation::match_no_details(score.value(), "match of the fallback query");
}

} // namespace lucene
