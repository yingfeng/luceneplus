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

module lucene_core:synonym_query.impl;

import :synonym_query;

import std;

import :boolean_clause;
import :boolean_query;
import :bytes_ref;
import :collection_statistics;
import :doc_id_set_iterator;
import :error;
import :explanation;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :numeric_doc_values;
import :postings_enum;
import :query;
import :query_visitor;
import :scorable;
import :score_mode;
import :scorer;
import :scorer_supplier;
import :similarity;
import :term;
import :term_query;
import :term_statistics;
import :term_states;
import :terms;
import :terms_enum;
import :types;
import :weight;

namespace lucene {

namespace {

/// A [`SimScorer`] that always answers zero, used when scores are not needed.
class ZeroSimScorer final : public SimScorer {
public:
    [[nodiscard]] float score(float /*freq*/, int64_t /*norm*/) const override { return 0.0F; }
};

/// The `PostingsEnum` of every term of the group that occurs in `reader`.
///
/// The reference implementation resolves these through `TermStates`; seeking the
/// leaf's dictionary is equivalent per leaf (see the deviation on
/// [`SynonymWeight`]).
Result<std::vector<std::unique_ptr<PostingsEnum>>> open_group_postings(LeafReader &reader,
                                                                      const std::string &field,
                                                                      const std::vector<Term> &terms) {
    std::vector<std::unique_ptr<PostingsEnum>> postings;
    auto field_terms = reader.terms(field);
    if (!field_terms.has_value()) {
        return std::unexpected(std::move(field_terms.error()));
    }
    if (field_terms.value() == nullptr) {
        return postings; // The leaf has no terms for this field at all.
    }
    auto terms_enum = field_terms.value()->iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    if (terms_enum.value() == nullptr) {
        return postings;
    }

    for (const Term &term : terms) {
        auto found = terms_enum.value()->seek_exact(term.bytes().as_slice());
        if (!found.has_value()) {
            return std::unexpected(std::move(found.error()));
        }
        if (!found.value()) {
            continue; // This term of the group does not occur in the leaf.
        }
        auto term_postings = terms_enum.value()->postings(std::unique_ptr<PostingsEnum>());
        if (!term_postings.has_value()) {
            return std::unexpected(std::move(term_postings.error()));
        }
        if (term_postings.value() != nullptr) {
            postings.push_back(std::move(term_postings.value()));
        }
    }
    return postings;
}

/// Scores a document with the sum of the frequencies of the group's terms.
///
/// Port of `SynonymScorer` (`core/search/synonym_query.rs`).  The reference
/// implementation keeps the postings in a `DisiPriorityQueue`; this port walks the
/// (few) enums of the group linearly, which is the same answer and simpler.
class SynonymScorer final : public Scorer {
public:
    SynonymScorer(std::vector<std::unique_ptr<PostingsEnum>> postings, std::shared_ptr<SimScorer> sim_scorer,
                  std::unique_ptr<NumericDocValues> norms)
        : postings_(std::move(postings)), sim_scorer_(std::move(sim_scorer)), norms_(std::move(norms)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override { return advance_to(doc_ + 1); }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override { return advance_to(target); }

    [[nodiscard]] Result<int64_t> cost() const override {
        int64_t total = 0;
        for (const std::unique_ptr<PostingsEnum> &postings : postings_) {
            auto one = postings->cost();
            if (!one.has_value()) {
                return std::unexpected(std::move(one.error()));
            }
            total += one.value();
        }
        return total;
    }

    // ---- Scorer ----

    [[nodiscard]] Result<int32_t> scoring_doc_id() override { return doc_; }

    /// No impacts are available for a synonym group, so nothing can be skipped.
    [[nodiscard]] Result<float> get_max_score(int32_t /*upto*/) override {
        return std::numeric_limits<float>::infinity();
    }

    [[nodiscard]] TwoPhaseState has_two_phase_iterator() const override { return TwoPhaseState::No; }

    // ---- Scorable ----

    [[nodiscard]] Result<float> score() override {
        return sim_scorer_->score(static_cast<float>(freq_), norm_);
    }

    /// The summed term frequency of the current document.
    [[nodiscard]] int32_t freq() const noexcept { return freq_; }

private:
    [[nodiscard]] Result<int32_t> advance_to(int32_t target) {
        if (postings_.empty()) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }

        int32_t next = NO_MORE_DOCS;
        for (const std::unique_ptr<PostingsEnum> &postings : postings_) {
            int32_t current = postings->doc_id();
            if (current < target) {
                auto moved = postings->advance(target);
                if (!moved.has_value()) {
                    return std::unexpected(std::move(moved.error()));
                }
                current = moved.value();
            }
            next = std::min(next, current);
        }

        doc_ = next;
        if (next == NO_MORE_DOCS) {
            freq_ = 0;
            return doc_;
        }

        int32_t total = 0;
        for (const std::unique_ptr<PostingsEnum> &postings : postings_) {
            if (postings->doc_id() != next) {
                continue;
            }
            auto freq = postings->freq();
            if (!freq.has_value()) {
                return std::unexpected(std::move(freq.error()));
            }
            total += freq.value();
        }
        freq_ = total;

        norm_ = 1;
        if (norms_ != nullptr) {
            auto exact = norms_->advance_exact(next);
            if (!exact.has_value()) {
                return std::unexpected(std::move(exact.error()));
            }
            if (exact.value()) {
                auto value = norms_->long_value();
                if (!value.has_value()) {
                    return std::unexpected(std::move(value.error()));
                }
                norm_ = value.value();
            }
        }
        return doc_;
    }

    std::vector<std::unique_ptr<PostingsEnum>> postings_;
    std::shared_ptr<SimScorer> sim_scorer_;
    std::unique_ptr<NumericDocValues> norms_;
    int32_t doc_{-1};
    int32_t freq_{0};
    int64_t norm_{1};
};

/// Builds the [`SynonymScorer`] of one leaf.
///
/// Port of `SynonymScorerSupplier`.  The group data is shared rather than
/// borrowed, so the supplier outlives the weight safely.
class SynonymScorerSupplier final : public ScorerSupplier {
public:
    SynonymScorerSupplier(std::shared_ptr<const std::string> field, std::shared_ptr<const std::vector<Term>> terms,
                          std::shared_ptr<SimScorer> sim_scorer)
        : field_(std::move(field)), terms_(std::move(terms)), sim_scorer_(std::move(sim_scorer)) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext &context,
                                                      const IndexSearcher & /*searcher*/) override {
        LeafReader *reader = context.reader();
        auto postings = open_group_postings(*reader, *field_, *terms_);
        if (!postings.has_value()) {
            return std::unexpected(std::move(postings.error()));
        }
        if (postings.value().empty()) {
            return std::unique_ptr<Scorer>{}; // No term of the group occurs here.
        }
        if (sim_scorer_ == nullptr) {
            return std::unique_ptr<Scorer>{};
        }

        auto norms = reader->get_norm_values(*field_);
        if (!norms.has_value()) {
            return std::unexpected(std::move(norms.error()));
        }
        return std::unique_ptr<Scorer>(
                new SynonymScorer(std::move(postings.value()), sim_scorer_, std::move(norms.value())));
    }

    /// The sum of the document frequencies of the terms present in the leaf.
    [[nodiscard]] Result<int64_t> cost(const LeafReaderContext &context, const IndexSearcher & /*searcher*/) override {
        LeafReader *reader = context.reader();
        auto field_terms = reader->terms(*field_);
        if (!field_terms.has_value()) {
            return std::unexpected(std::move(field_terms.error()));
        }
        if (field_terms.value() == nullptr) {
            return 0;
        }
        auto terms_enum = field_terms.value()->iterator();
        if (!terms_enum.has_value()) {
            return std::unexpected(std::move(terms_enum.error()));
        }
        if (terms_enum.value() == nullptr) {
            return 0;
        }

        int64_t total = 0;
        for (const Term &term : *terms_) {
            auto found = terms_enum.value()->seek_exact(term.bytes().as_slice());
            if (!found.has_value()) {
                return std::unexpected(std::move(found.error()));
            }
            if (!found.value()) {
                continue;
            }
            auto doc_freq = terms_enum.value()->doc_freq();
            if (!doc_freq.has_value()) {
                return std::unexpected(std::move(doc_freq.error()));
            }
            total += doc_freq.value();
        }
        return total;
    }

private:
    std::shared_ptr<const std::string> field_;
    std::shared_ptr<const std::vector<Term>> terms_;
    std::shared_ptr<SimScorer> sim_scorer_;
};

} // namespace

// ---------------- SynonymQuery::Builder ----------------

VoidResult SynonymQuery::Builder::add_term(Term term, float boost) {
    if (term_bytes_.empty()) {
        field_ = term.field();
    } else if (term.field() != field_) {
        return err(LuceneError::illegal_argument("a synonym group must stay on one field, got \"" + term.field() +
                                                 "\" and \"" + field_ + "\""));
    }
    term_bytes_.push_back(term.bytes());
    boosts_.push_back(boost);
    return {};
}

SynonymQuery SynonymQuery::Builder::build() const { return SynonymQuery(field_, term_bytes_, boosts_); }

// ---------------- SynonymQuery ----------------

SynonymQuery::SynonymQuery(std::string field, std::vector<BytesRef> term_bytes, std::vector<float> boosts)
    : field_(std::move(field)), term_bytes_(std::move(term_bytes)), boosts_(std::move(boosts)) {}

std::vector<Term> SynonymQuery::get_terms() const {
    std::vector<Term> terms;
    terms.reserve(term_bytes_.size());
    for (const BytesRef &bytes : term_bytes_) {
        terms.emplace_back(field_, bytes);
    }
    return terms;
}

Result<std::unique_ptr<Weight>> SynonymQuery::create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                            float boost) const {
    auto weight = SynonymWeight::create(*this, *this, searcher, score_mode, boost);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    return std::unique_ptr<Weight>(weight.value().release());
}

Result<std::shared_ptr<Query>> SynonymQuery::rewrite(const IndexSearcher &searcher) const {
    (void)searcher;
    if (term_bytes_.empty()) {
        BooleanQuery::Builder builder;
        auto built = builder.build();
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        return std::shared_ptr<Query>(std::make_shared<BooleanQuery>(std::move(built.value())));
    }
    if (term_bytes_.size() == 1 && boosts_[0] == 1.0F) {
        return std::shared_ptr<Query>(std::make_shared<TermQuery>(Term(field_, term_bytes_[0])));
    }
    return std::shared_ptr<Query>{};
}

VoidResult SynonymQuery::visit(QueryVisitor &visitor) const {
    if (!visitor.accept_field(field_)) {
        return {};
    }
    const std::vector<Term> terms = get_terms();
    return visitor.consume_terms(*this, terms);
}

Result<std::string> SynonymQuery::to_string(std::string_view field) const {
    std::string result = "Synonym(";
    for (size_t i = 0; i < term_bytes_.size(); ++i) {
        if (i != 0) {
            result += ' ';
        }
        const TermQuery term_query(Term(field_, term_bytes_[i]));
        auto text = term_query.to_string(field);
        if (!text.has_value()) {
            return std::unexpected(std::move(text.error()));
        }
        result += text.value();
        if (boosts_[i] != 1.0F) {
            result += std::format("^{:.1f}", boosts_[i]);
        }
    }
    result += ")";
    return result;
}

bool SynonymQuery::equals(const Query &other) const {
    const auto *synonym = dynamic_cast<const SynonymQuery *>(&other);
    if (synonym == nullptr) {
        return false;
    }
    if (field_ != synonym->field_ || boosts_ != synonym->boosts_) {
        return false;
    }
    if (term_bytes_.size() != synonym->term_bytes_.size()) {
        return false;
    }
    for (size_t i = 0; i < term_bytes_.size(); ++i) {
        if (term_bytes_[i] != synonym->term_bytes_[i]) {
            return false;
        }
    }
    return true;
}

size_t SynonymQuery::hash() const {
    size_t result = std::hash<std::string>{}(field_);
    for (const BytesRef &bytes : term_bytes_) {
        for (const uint8_t byte : bytes.as_slice()) {
            result = result * 31 + byte;
        }
        result = result * 31 + 7;
    }
    for (const float boost : boosts_) {
        result = result * 31 + static_cast<size_t>(boost);
    }
    return result;
}

// ---------------- SynonymWeight ----------------

SynonymWeight::SynonymWeight(const Query &parent_query, std::string field, std::vector<Term> terms,
                             std::shared_ptr<Similarity> similarity, std::shared_ptr<SimScorer> sim_scorer,
                             ScoreMode score_mode)
    : parent_query_(&parent_query),
      field_(std::move(field)),
      terms_(std::move(terms)),
      similarity_(std::move(similarity)),
      sim_scorer_(std::move(sim_scorer)),
      score_mode_(score_mode) {}

Result<std::unique_ptr<SynonymWeight>> SynonymWeight::create(const Query &parent_query, const SynonymQuery &query,
                                                             const IndexSearcher &searcher, ScoreMode score_mode,
                                                             float boost) {
    if (!score_mode_needs_scores(score_mode)) {
        // Nothing scores, so the statistics are irrelevant; a zero scorer keeps
        // the matching path uniform (see the deviation on the class).
        return std::unique_ptr<SynonymWeight>(
                new SynonymWeight(parent_query, query.field(), query.get_terms(), searcher.similarity_ptr(),
                                  std::make_shared<ZeroSimScorer>(), score_mode));
    }

    auto collection_stats = searcher.collection_statistics(query.field());
    if (!collection_stats.has_value()) {
        return std::unexpected(std::move(collection_stats.error()));
    }

    int32_t doc_freq = 0;
    int64_t total_term_freq = 0;
    for (const Term &term : query.get_terms()) {
        auto states = build_term_states(searcher, term, true);
        if (!states.has_value()) {
            return std::unexpected(std::move(states.error()));
        }
        auto term_doc_freq = states.value().doc_freq();
        if (!term_doc_freq.has_value()) {
            return std::unexpected(std::move(term_doc_freq.error()));
        }
        if (term_doc_freq.value() > 0) {
            doc_freq = std::max(doc_freq, term_doc_freq.value());
            auto term_total = states.value().total_term_freq();
            if (!term_total.has_value()) {
                return std::unexpected(std::move(term_total.error()));
            }
            total_term_freq += term_total.value();
        }
    }

    std::shared_ptr<SimScorer> sim_scorer;
    if (doc_freq > 0) {
        // The pseudo term stands for the whole group: the highest document
        // frequency and the summed total term frequency.
        auto pseudo = TermStatistics::create(Term(query.field(), BytesRef::from_string("synonym pseudo-term")),
                                             doc_freq, total_term_freq);
        if (!pseudo.has_value()) {
            return std::unexpected(std::move(pseudo.error()));
        }
        const std::array<TermStatistics, 1> stats{pseudo.value()};
        auto scorer = searcher.similarity().scorer(boost, *collection_stats.value(), stats);
        if (!scorer.has_value()) {
            return std::unexpected(std::move(scorer.error()));
        }
        sim_scorer = std::move(scorer.value());
    }

    return std::unique_ptr<SynonymWeight>(new SynonymWeight(parent_query, query.field(), query.get_terms(),
                                                            searcher.similarity_ptr(), std::move(sim_scorer),
                                                            score_mode));
}

Result<std::unique_ptr<ScorerSupplier>> SynonymWeight::scorer_supplier(const LeafReaderContext & /*context*/,
                                                                       const IndexSearcher & /*searcher*/) {
    if (sim_scorer_ == nullptr) {
        return std::unique_ptr<ScorerSupplier>{}; // No term of the group occurs in the index.
    }
    return std::unique_ptr<ScorerSupplier>(new SynonymScorerSupplier(
            std::make_shared<const std::string>(field_), std::make_shared<const std::vector<Term>>(terms_),
            sim_scorer_));
}

Result<Explanation> SynonymWeight::explain(const LeafReaderContext &context, int32_t doc,
                                           const IndexSearcher & /*searcher*/) const {
    auto postings = open_group_postings(*context.reader(), field_, terms_);
    if (!postings.has_value()) {
        return std::unexpected(std::move(postings.error()));
    }

    int32_t freq = 0;
    for (std::unique_ptr<PostingsEnum> &term_postings : postings.value()) {
        auto advanced = term_postings->advance(doc);
        if (!advanced.has_value()) {
            return std::unexpected(std::move(advanced.error()));
        }
        if (advanced.value() != doc) {
            continue;
        }
        auto one = term_postings->freq();
        if (!one.has_value()) {
            return std::unexpected(std::move(one.error()));
        }
        freq += one.value();
    }
    if (freq == 0) {
        return Explanation::no_match_no_details("no matching term");
    }

    int64_t norm = 1;
    auto norms = context.reader()->get_norm_values(field_);
    if (!norms.has_value()) {
        return std::unexpected(std::move(norms.error()));
    }
    if (norms.value() != nullptr) {
        auto exact = norms.value()->advance_exact(doc);
        if (!exact.has_value()) {
            return std::unexpected(std::move(exact.error()));
        }
        if (exact.value()) {
            auto value = norms.value()->long_value();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            norm = value.value();
        }
    }

    if (sim_scorer_ == nullptr) {
        return err(LuceneError::illegal_state("the synonym similarity is missing for a matching document"));
    }
    auto freq_explanation = Explanation::match_no_details(static_cast<float>(freq),
                                                          "termFreq=" + std::to_string(freq));
    return sim_scorer_->explain(freq_explanation, norm);
}

} // namespace lucene
