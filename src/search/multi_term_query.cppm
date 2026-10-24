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

export module lucene_core:multi_term_query;

import :types;
import std;

import :error;
import :query;
import :query_visitor;
import :score_mode;
import :terms;
import :terms_enum;
import :weight;

namespace lucene {

/// How a [`MultiTermQuery`] is rewritten into a query the searcher can run.
///
/// Port of the `RewriteMethod` trait objects (`ConstantScoreRewrite`,
/// `ConstantScoreBlendedRewrite`, `ConstantScoreBooleanRewrite`,
/// `ScoringBooleanRewrite` and the three top-terms rewrites) as one value type:
/// the top-terms rewrites carry the maximum number of terms they keep, so they
/// cannot be plain enumerators.
///
/// **Deviations:**
///  * `kConstantScore` and `kConstantScoreBlended` share one implementation: the
///    reference implementation's blended variant only differs in how it splits
///    the work between a boolean query and a bit set, and both assign every
///    matching document the query's boost;
///  * [`kDocValues`](Kind::kDocValues) goes through
///    [`MultiTermQueryDocValuesWrapper`](lucene_core:doc_values_rewrite_method).
export class MultiTermRewriteMethod {
public:
    /// The rewrite the value stands for.
    enum class Kind : uint8_t {
        /// Expand to a constant score query over the matching documents.
        kConstantScore = 0,
        /// Same as [`kConstantScore`](Kind::kConstantScore); see the class note.
        kConstantScoreBlended = 1,
        /// Expand to a constant score query over a boolean query of the terms.
        kConstantScoreBoolean = 2,
        /// Expand to a boolean query of scoring term queries.
        kScoringBoolean = 3,
        /// Keep the best-boosted terms and blend their frequencies.
        kTopTermsBlendedFreqScoring = 4,
        /// Keep the best-boosted terms in a boolean query of scoring term queries.
        kTopTermsScoringBoolean = 5,
        /// Keep the best-boosted terms in a boolean query of constant score term
        /// queries.
        kTopTermsBoostOnlyBoolean = 6,
        /// Enumerate the terms through the field's doc values instead of its
        /// postings.
        kDocValues = 7,
    };

    constexpr MultiTermRewriteMethod() noexcept = default;
    constexpr explicit MultiTermRewriteMethod(Kind kind, size_t size = 0) noexcept : kind_(kind), size_(size) {}

    /// The rewrite the value stands for.
    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
    /// How many terms a top-terms rewrite keeps.
    [[nodiscard]] constexpr size_t size() const noexcept { return size_; }

    // The rewrite methods the reference implementation exposes as constants.
    static const MultiTermRewriteMethod kConstantScore;
    static const MultiTermRewriteMethod kConstantScoreBlended;
    static const MultiTermRewriteMethod kConstantScoreBoolean;
    static const MultiTermRewriteMethod kScoringBoolean;
    /// The doc values rewrite; see
    /// [`MultiTermQueryDocValuesWrapper`](lucene_core:doc_values_rewrite_method).
    static const MultiTermRewriteMethod kDocValues;

    [[nodiscard]] bool operator==(const MultiTermRewriteMethod &other) const noexcept {
        return kind_ == other.kind_ && size_ == other.size_;
    }

    [[nodiscard]] size_t hash() const noexcept {
        return static_cast<size_t>(kind_) * 31 + size_;
    }

private:
    Kind kind_{Kind::kConstantScore};
    size_t size_{0};
};

/// A query that matches documents containing any term of a field that its
/// enumeration accepts.
///
/// Port of `MultiTermQuery` (`core/search/multi_term_query.rs`).
///
/// This class cannot be used directly: a subclass provides the enumeration
/// through [`terms_enum`] (a prefix, a range, an automaton, ...), and
/// [`rewrite`] turns the matched terms into a boolean query, so that
/// [`IndexSearcher`](:index_searcher) can run it.
///
/// **Deviations** (C++ mechanics and scope):
///  * the reference implementation's `RewriteMethod` trait objects become the
///    [`MultiTermRewriteMethod`] enum, and the blended/top-terms rewrites are not
///    ported;
///  * the reference implementation's generic `TermsEnum` associated type becomes a
///    plain [`TermsEnum`] returned as `unique_ptr`;
///  * [`create_weight`] rewrites the query first instead of failing with the
///    reference implementation's "must be rewritten first" assertion, because this port's
///    [`IndexSearcher`](:index_searcher) does not rewrite queries on its own.
export class MultiTermQuery : public Query {
public:
    [[nodiscard]] const std::string &field() const noexcept { return field_; }

    /// The expansion this query stands for, e.g. the prefix, the range or the
    /// automaton, used by [`to_string`] and [`equals`].
    [[nodiscard]] const std::string &pattern() const noexcept { return pattern_; }

    [[nodiscard]] MultiTermRewriteMethod rewrite_method() const noexcept { return rewrite_method_; }
    void set_rewrite_method(MultiTermRewriteMethod rewrite_method) noexcept { rewrite_method_ = rewrite_method; }

    /// Number of unique terms this query expands to, or `-1` when unknown.
    [[nodiscard]] virtual int64_t terms_count() const { return -1; }

    /// The enumeration over the terms this query matches, positioned on the first
    /// one: [`terms_enum`] made public, so that the rewrite methods living in
    /// other modules can drive it.
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> terms_enum_for(Terms &terms) const { return terms_enum(terms); }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// Expands the matched terms into a boolean query, in the form
    /// [`rewrite_method`] asks for.
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

protected:
    MultiTermQuery(std::string field, std::string pattern, MultiTermRewriteMethod rewrite_method);

    /// The enumeration over the terms this query matches, positioned on the first
    /// one.  It must never be null: return an empty enumeration instead.
    [[nodiscard]] virtual Result<std::unique_ptr<TermsEnum>> terms_enum(Terms &terms) const = 0;

    /// The class name used in [`to_string`], e.g. `PrefixQuery`.
    [[nodiscard]] virtual std::string_view query_name() const = 0;

    /// Whether `other` matches the same terms; a subclass with more state than
    /// [`pattern`] overrides this.
    [[nodiscard]] virtual bool peers(const MultiTermQuery &other) const;

private:
    std::string field_;
    std::string pattern_;
    MultiTermRewriteMethod rewrite_method_;
};

/// A [`TermsEnum`] that yields the terms of `inner` that `accept` admits, and
/// stops as soon as `stop` says no later term can match.
///
/// Port of the enumeration side of `PrefixTermsEnum` and `TermRangeTermsEnum`
/// (`core/search/prefix_query.rs`, `core/search/term_range_query.rs`), sharing
/// the filtering loop the two have in common.  It is forward only, like every
/// enum the multi-term rewrites use.
export class PredicateTermsEnum final : public TermsEnum {
public:
    /// `accept` decides whether a term matches, `stop` whether the enumeration
    /// can end because no later term can match.
    using Judge = std::function<bool(const BytesRef &term)>;

    PredicateTermsEnum(std::unique_ptr<TermsEnum> inner, Judge accept, Judge stop);

    // ---- BytesRefIterator ----

    [[nodiscard]] Result<const BytesRef *> next() override;

    // ---- TermsEnum ----

    [[nodiscard]] Result<const BytesRef *> term() const override;
    [[nodiscard]] Result<int64_t> ord() const override;
    [[nodiscard]] Result<int32_t> doc_freq() override;
    [[nodiscard]] Result<int64_t> total_term_freq() override;
    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                                           int32_t flags) override;
    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t flags) override;
    [[nodiscard]] Result<AttributeSource *> attributes() override;
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override;
    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override;
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> target) override;
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override;
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> term, const TermStateEnum &state) override;
    [[nodiscard]] Result<TermStateEnum> term_state() override;

private:
    std::unique_ptr<TermsEnum> inner_;
    Judge accept_;
    Judge stop_;
    bool ended_{false};
};

/// Rewrites a [`MultiTermQuery`] into a constant score query that never exceeds
/// the boolean clause limit.
///
/// Port of `MultiTermQueryConstantScoreWrapper`
/// (`core/search/multi_term_query_constant_score_wrapper.rs`), also used for the
/// reference implementation's blended variant (see [`MultiTermRewriteMethod`]).
/// The wrapper keeps the multi-term query as it is, so it has the field and the
/// term enumeration available when a leaf's scorer is built; the weight then
/// either delegates to a constant score boolean query (the usual case: a handful
/// of terms) or unions the postings of every matching term into a document set,
/// which is what keeps a query with many expansions from overflowing the clause
/// limit.
///
/// **Deviations:** the reference implementation computes the collected terms
/// once per weight and shares the outcome between the slices of a parallel
/// search; the port is serial, so the work happens in each leaf's scorer
/// supplier.  The per-leaf "dense term" shortcut (a term that occurs in every
/// document that has a value for the field) is kept.
export class MultiTermQueryConstantScoreWrapper final : public Query {
public:
    /// Wraps `query`, which must stay alive as long as the wrapper does.
    explicit MultiTermQueryConstantScoreWrapper(std::shared_ptr<const MultiTermQuery> query);

    /// The wrapped multi-term query.
    [[nodiscard]] const std::shared_ptr<const MultiTermQuery> &query() const noexcept { return query_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// The wrapped query, so that it keeps matching the same documents; the text
    /// form is the wrapped query's.
    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    std::shared_ptr<const MultiTermQuery> query_;
};

} // namespace lucene
