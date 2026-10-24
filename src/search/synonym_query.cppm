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

export module lucene_core:synonym_query;

import :types;
import std;

import :bytes_ref;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader_context;
import :query;
import :query_visitor;
import :score_mode;
import :scorer_supplier;
import :similarity;
import :term;
import :weight;

namespace lucene {

/// A query that treats several terms of one field as synonyms of each other.
///
/// Port of `SynonymQuery` (`core/search/synonym_query.rs`).
///
/// It matches the documents containing any of the terms, but scores them as if
/// the terms had been indexed as one: the similarity is invoked once with the
/// **sum of the term frequencies** of the document, and with the statistics of a
/// pseudo term whose document frequency is the maximum of the terms' and whose
/// total term frequency is their sum.  That is what makes a synonym expansion
/// score like the original term instead of like a disjunction of unrelated ones.
///
/// **Deviation:** without scores the reference implementation rewrites to a
/// [`BooleanQuery`](:boolean_query) of `Should` clauses so the cheaper matching
/// paths apply; this port keeps the synonym weight and scores with zero instead,
/// because a [`Weight`] here borrows its parent query and a rewritten query would
/// not outlive the weight.
export class SynonymQuery final : public Query {
public:
    /// Collects the terms of the synonym group.
    class Builder {
    public:
        Builder() = default;

        /// Adds `term` with `boost`; every term must belong to the same field.
        [[nodiscard]] VoidResult add_term(Term term, float boost = 1.0F);

        /// Builds the query; an empty group is allowed and matches nothing.
        [[nodiscard]] SynonymQuery build() const;

    private:
        std::string field_;
        std::vector<BytesRef> term_bytes_;
        std::vector<float> boosts_;
    };

    SynonymQuery(std::string field, std::vector<BytesRef> term_bytes, std::vector<float> boosts);

    /// The field all terms belong to.
    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    /// The terms, as `Term`s of [`field`].
    [[nodiscard]] std::vector<Term> get_terms() const;
    /// The raw term bytes, in insertion order.
    [[nodiscard]] const std::vector<BytesRef> &term_bytes() const noexcept { return term_bytes_; }
    /// The boost of each term.
    [[nodiscard]] const std::vector<float> &boosts() const noexcept { return boosts_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    /// An empty group becomes an empty boolean query, a single unboosted term
    /// becomes that [`TermQuery`](:term_query).
    [[nodiscard]] Result<std::shared_ptr<Query>> rewrite(const IndexSearcher &searcher) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    /// `Synonym(term^boost ...)`, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    std::string field_;
    std::vector<BytesRef> term_bytes_;
    std::vector<float> boosts_;
};

/// The [`Weight`] of a [`SynonymQuery`].
///
/// Port of `SynonymWeight` (`core/search/synonym_query.rs`).  It builds the
/// pseudo-term statistics once, at collection level, and hands each leaf a
/// scorer that sums the frequencies of the terms occurring in a document.
///
/// **Deviation:** the reference implementation resolves the per-leaf postings
/// through `TermStates`; this port seeks the leaf's term dictionary for each
/// term, which is equivalent per leaf and avoids duplicating the term-state
/// plumbing.  The statistics still come from `build_term_states`.
export class SynonymWeight final : public Weight {
public:
    /// Builds the weight and the collection-level similarity scorer.
    [[nodiscard]] static Result<std::unique_ptr<SynonymWeight>>
    create(const Query &parent_query, const SynonymQuery &query, const IndexSearcher &searcher, ScoreMode score_mode,
           float boost);

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext &context, int32_t doc,
                                              const IndexSearcher &searcher) const override;

    [[nodiscard]] const Query &get_query() const override { return *parent_query_; }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext &context,
                                                                          const IndexSearcher &searcher) override;

    /// A synonym weight only depends on segment-immutable structures.
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext &context) const override {
        (void)context;
        return true;
    }

    /// Whether the synonym statistics are available, i.e. whether at least one
    /// term occurs in the index.
    [[nodiscard]] bool has_stats() const noexcept { return sim_scorer_ != nullptr; }

    [[nodiscard]] const std::string &field() const noexcept { return field_; }
    [[nodiscard]] const std::vector<Term> &terms() const noexcept { return terms_; }
    [[nodiscard]] const std::shared_ptr<SimScorer> &sim_scorer() const noexcept { return sim_scorer_; }
    [[nodiscard]] const std::shared_ptr<Similarity> &similarity() const noexcept { return similarity_; }
    [[nodiscard]] ScoreMode score_mode() const noexcept { return score_mode_; }

private:
    SynonymWeight(const Query &parent_query, std::string field, std::vector<Term> terms,
                  std::shared_ptr<Similarity> similarity, std::shared_ptr<SimScorer> sim_scorer, ScoreMode score_mode);

    const Query *parent_query_;
    std::string field_;
    std::vector<Term> terms_;
    std::shared_ptr<Similarity> similarity_;
    std::shared_ptr<SimScorer> sim_scorer_;
    ScoreMode score_mode_;
};

} // namespace lucene
