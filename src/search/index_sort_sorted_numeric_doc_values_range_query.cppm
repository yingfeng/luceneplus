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

export module lucene_core:index_sort_sorted_numeric_doc_values_range_query;

import :types;
import std;

import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer_supplier;
import :weight;

namespace lucene {

/// A range query over a `SortedNumericDocValues` field that exploits an index
/// sort on the same field.
///
/// Port of `IndexSortSortedNumericDocValuesRangeQuery`
/// (`core/search/index_sort_sorted_numeric_doc_values_range_query.rs`).
///
/// When the segment is index sorted by this field, the matching documents form a
/// contiguous run, so the query binary-searches the two boundaries instead of
/// visiting the values of every document.  Otherwise it delegates to
/// `fallback_query`, which is the plain range query the caller came from, so the
/// answer is always the fallback's.
///
/// **NOTE:** the shortcut needs a segment that is index sorted by a
/// **sorted-numeric** field.  This port's writer cannot produce one — neither can
/// the reference implementation, whose `SortField` index sorters cover only
/// string/int/long/float/double — so in practice the shortcut fires only for
/// segments written elsewhere.  The query is here because the reference
/// implementation has it, and because it is the standard entry point for a range
/// over sorted numeric values.
///
/// **Deviations** (they cost speed, never change the matches):
///  * the reference implementation also has a BKD/points shortcut; it is not
///    ported, so such segments go through the fallback;
///  * the reference implementation filters the run with the raw doc-values
///    iterator when some documents have no value; this port only uses the
///    shortcut when documents without a value sort outside `[lower, upper]`;
///  * the shortcut verifies the boundaries it computed and falls back when they
///    do not describe a monotonic run (this also covers reverse-sorted indexes,
///    which the reference implementation does not support either).
export class IndexSortSortedNumericDocValuesRangeQuery final : public Query {
public:
    /// `fallback_query` answers when the shortcut does not apply; it must match
    /// the same documents.
    IndexSortSortedNumericDocValuesRangeQuery(std::string field, int64_t lower_value, int64_t upper_value,
                                              std::shared_ptr<Query> fallback_query);

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] int64_t lower_value() const noexcept { return lower_value_; }
    [[nodiscard]] int64_t upper_value() const noexcept { return upper_value_; }
    [[nodiscard]] const std::shared_ptr<Query> &fallback_query() const noexcept { return fallback_query_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// `[min TO max]` becomes `FieldExistsQuery`, a rewritten fallback is carried
    /// over, and a fallback that rewrites to match-all becomes
    /// [`MatchAllDocsQuery`](:match_all_docs_query).
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `field:[lower TO upper]`, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    std::string field_;
    int64_t lower_value_;
    int64_t upper_value_;
    std::shared_ptr<Query> fallback_query_;
};

/// The [`Weight`] of an [`IndexSortSortedNumericDocValuesRangeQuery`].
///
/// Port of `IndexSortSortedNumericDocValuesRangeQueryWeight`.  It owns the
/// fallback weight and tries the index-sort shortcut per leaf.
export class IndexSortSortedNumericDocValuesRangeQueryWeight final : public Weight {
public:
    /// Builds the weight; `fallback_weight` is the weight of the fallback query.
    [[nodiscard]] static Result<std::unique_ptr<IndexSortSortedNumericDocValuesRangeQueryWeight>>
    create(const Query &parent_query, std::string field, int64_t lower_value, int64_t upper_value,
           std::unique_ptr<Weight> fallback_weight, float boost, ScoreMode score_mode);

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override { return *parent_query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                          const IndexSearcher &searcher) override;

    /// With the shortcut and no deletions the count is the size of the run;
    /// otherwise the fallback counts.
    [[nodiscard]] Result<int32_t> count(const LeafReaderContext &context, const IndexSearcher &searcher) override;

    /// The documents a leaf contributes depend only on the segment and the query.
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        (void)context;
        return true;
    }

    [[nodiscard]] const Weight &fallback_weight() const noexcept { return *fallback_weight_; }

private:
    IndexSortSortedNumericDocValuesRangeQueryWeight(const Query &parent_query, std::string field, int64_t lower_value,
                                                    int64_t upper_value, std::unique_ptr<Weight> fallback_weight,
                                                    float boost, ScoreMode score_mode);

    const Query *parent_query_;
    std::string field_;
    int64_t lower_value_;
    int64_t upper_value_;
    std::unique_ptr<Weight> fallback_weight_;
    float boost_;
    ScoreMode score_mode_;
};

} // namespace lucene
