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

module lucene_core:index_searcher.impl;

import :index_searcher;

import :bits;
import :bm25_similarity;
import :bulk_scorer;
import :collector;
import :collector_manager;
import :constant_score_query;
import :collection_statistics;
import :doc_id_set_iterator;
import :error;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :query;
import :query_cache;
import :query_caching_policy;
import :score_doc;
import :score_mode;
import :scorer_supplier;
import :similarity;
import :term;
import :term_statistics;
import :terms;
import :top_docs;
import :top_field_collector;
import :top_field_docs;
import :total_hit_count_collector;
import :top_score_doc_collector_manager;
import :types;
import :weight;

import std;

namespace lucene {

namespace {

/// Reduces through a [`CollectorManager`] and keeps the single result a plain
/// manager produces.
///
/// A manager may answer several results (see `MultiCollectorManager`); a caller
/// that asked for one collector manager gets the first.
Result<TopDocs> reduce_single(const CollectorManager &manager, std::vector<std::unique_ptr<Collector>> collectors) {
    auto results = manager.reduce(std::move(collectors));
    if (!results.has_value()) {
        return std::unexpected(std::move(results.error()));
    }
    if (results.value().empty()) {
        return err(LuceneError::illegal_state("the collector manager produced no result"));
    }
    return std::move(results.value().front());
}

/// The default similarity, BM25 with Lucene's default parameters.
std::shared_ptr<Similarity> make_default_similarity() {
    auto bm25 = BM25Similarity::default_similarity();
    if (!bm25.has_value()) {
        return nullptr;
    }
    return std::make_shared<BM25Similarity>(std::move(bm25.value()));
}

} // namespace

IndexSearcher::IndexSearcher(std::shared_ptr<IndexReader> reader, std::shared_ptr<Similarity> similarity)
        : reader_(std::move(reader)) {
    if (similarity != nullptr) {
        similarity_ = std::move(similarity);
    } else {
        // BM25Similarity::default_similarity only fails on invalid static
        // parameters that a constexpr initializer cannot express; keep a hard
        // failure loud rather than silently scoring with nothing.
        similarity_ = make_default_similarity();
        if (similarity_ == nullptr) {
            throw std::logic_error("lucene: BM25Similarity::default_similarity() failed");
        }
    }
}

Result<std::optional<CollectionStatistics>> IndexSearcher::collection_statistics(std::string_view field) const {
    int64_t doc_count = 0;
    int64_t sum_total_term_freq = 0;
    int64_t sum_doc_freq = 0;

    auto leaves = reader_->leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    for (const auto &leaf : leaves.value()) {
        LeafReader &reader = *leaf.reader();
        auto terms = reader.terms(field);
        if (!terms.has_value()) {
            return std::unexpected(std::move(terms.error()));
        }
        if (terms.value() == nullptr) {
            // The reference implementation's `get_terms` substitutes `EmptyTerms` here, which adds
            // nothing to the sums.
            continue;
        }
        auto leaf_doc_count = terms.value()->doc_count();
        if (!leaf_doc_count.has_value()) {
            return std::unexpected(std::move(leaf_doc_count.error()));
        }
        auto leaf_sum_total_term_freq = terms.value()->sum_total_term_freq();
        if (!leaf_sum_total_term_freq.has_value()) {
            return std::unexpected(std::move(leaf_sum_total_term_freq.error()));
        }
        auto leaf_sum_doc_freq = terms.value()->sum_doc_freq();
        if (!leaf_sum_doc_freq.has_value()) {
            return std::unexpected(std::move(leaf_sum_doc_freq.error()));
        }
        doc_count += leaf_doc_count.value();
        sum_total_term_freq += leaf_sum_total_term_freq.value();
        sum_doc_freq += leaf_sum_doc_freq.value();
    }

    if (doc_count == 0) {
        return std::optional<CollectionStatistics>{};
    }

    auto max_doc = reader_->max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }

    auto stats = CollectionStatistics::create(std::string(field), max_doc.value(), doc_count, sum_total_term_freq,
                                             sum_doc_freq);
    if (!stats.has_value()) {
        return std::unexpected(std::move(stats.error()));
    }
    return std::optional<CollectionStatistics>(std::move(stats.value()));
}

Result<TermStatistics> IndexSearcher::term_statistics(const Term &term, int32_t doc_freq,
                                                      int64_t total_term_freq) const {
    return TermStatistics::create(term, doc_freq, total_term_freq);
}

Result<int32_t> IndexSearcher::doc_freq(const Term &term) const {
    return reader_->doc_freq(term);
}

Result<int64_t> IndexSearcher::total_term_freq(const Term &term) const {
    return reader_->total_term_freq(term);
}

Result<std::shared_ptr<Query>> IndexSearcher::rewrite(const Query &query) const {
    std::shared_ptr<Query> current;
    const Query *pending = &query;
    // Rewriting can cascade (a query rewrites into another rewritable query), so
    // keep going until an implementation reports "unchanged".
    while (true) {
        auto rewritten = pending->rewrite(*this);
        if (!rewritten.has_value()) {
            return std::unexpected(std::move(rewritten.error()));
        }
        if (!rewritten.value()) {
            return current;
        }
        current = std::move(rewritten.value());
        pending = current.get();
    }
}

Result<std::shared_ptr<Query>> IndexSearcher::rewrite_with_needs_scores(const Query &query,
                                                                        bool needs_scores) const {
    if (needs_scores) {
        return rewrite(query);
    }
    // A non-owning alias: the wrapper never owns the caller's query.
    auto wrapper = std::make_shared<ConstantScoreQuery>(std::shared_ptr<Query>(const_cast<Query *>(&query), [](Query *) {}));
    auto rewritten = rewrite(*wrapper);
    if (!rewritten.has_value()) {
        return std::unexpected(std::move(rewritten.error()));
    }
    // When the wrapper reports "no further rewrite", the wrapper itself is the
    // final form; answering null would make the caller drop it.
    if (rewritten.value() == nullptr) {
        return std::shared_ptr<Query>(std::move(wrapper));
    }
    return rewritten;
}

Result<TopDocs> IndexSearcher::search(const Query &query, int32_t n) const {
    return search_after_score(nullptr, query, n);
}

Result<Explanation> IndexSearcher::explain(const Query &query, int32_t doc) const {
    auto max_doc = reader_->max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    if (doc < 0 || doc >= max_doc.value()) {
        return err(LuceneError::illegal_argument("doc " + std::to_string(doc) + " is out of bounds (maxDoc " +
                                                 std::to_string(max_doc.value()) + ")"));
    }

    auto rewritten = rewrite(query);
    if (!rewritten.has_value()) {
        return std::unexpected(std::move(rewritten.error()));
    }
    const Query *effective = rewritten.value() != nullptr ? rewritten.value().get() : &query;

    auto weight = create_weight(*effective, ScoreMode::Complete, 1.0F);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }

    auto leaves = reader_->leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    for (const LeafReaderContext &leaf : leaves.value()) {
        auto leaf_max_doc = leaf.reader()->max_doc();
        if (!leaf_max_doc.has_value()) {
            return std::unexpected(std::move(leaf_max_doc.error()));
        }
        if (doc < leaf.doc_base() || doc >= leaf.doc_base() + leaf_max_doc.value()) {
            continue;
        }
        return weight.value()->explain(leaf, doc - leaf.doc_base(), *this);
    }
    return err(LuceneError::illegal_argument("no leaf holds doc " + std::to_string(doc)));
}

Result<Explanation> IndexSearcher::explain(const Query &query, const ScoreDoc &score_doc) const {
    return explain(query, score_doc.doc);
}

Result<int32_t> IndexSearcher::count(const Query &query) const {
    TotalHitCountCollector collector;
    auto collected = search_with_collector(query, collector);
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    return collector.total_hits();
}

Result<TopFieldDocs> IndexSearcher::search(const Query &query, const Sort &sort, int32_t num_hits) const {
    if (num_hits < 0) {
        return err(LuceneError::illegal_argument("numHits must be >= 0, got " + std::to_string(num_hits)));
    }
    auto max_doc = reader_->max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    // A reader without documents still has to answer a query with no hits.
    const auto capped = static_cast<int32_t>(
            std::min(static_cast<size_t>(num_hits), static_cast<size_t>(std::max(1, max_doc.value()))));
    auto collector = TopFieldCollector::create(sort, capped);
    if (!collector.has_value()) {
        return std::unexpected(std::move(collector.error()));
    }
    auto collected = search_with_collector(query, *collector.value());
    if (!collected.has_value()) {
        return std::unexpected(std::move(collected.error()));
    }
    return collector.value()->top_docs();
}

Result<TopDocs> IndexSearcher::search_after_score(const ScoreDoc *after, const Query &query, int32_t num_hits) const {
    if (num_hits < 0) {
        return err(LuceneError::illegal_argument("numHits must be >= 0, got " + std::to_string(num_hits)));
    }
    auto max_doc = reader_->max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    const auto limit = static_cast<size_t>(std::max(1, max_doc.value()));

    if (after != nullptr && static_cast<size_t>(after->doc) >= limit) {
        return err(LuceneError::illegal_argument("after.doc exceeds the number of documents in the reader: after.doc=" +
                                                 std::to_string(after->doc) + " limit=" + std::to_string(limit)));
    }

    const auto capped_num_hits = std::min(static_cast<size_t>(num_hits), limit);
    Result<std::unique_ptr<TopScoreDocCollectorManager>> manager = err(LuceneError::illegal_state("unreachable"));
    if (after != nullptr) {
        manager = TopScoreDocCollectorManager::with_after(capped_num_hits, *after, TOTAL_HITS_THRESHOLD);
    } else {
        manager = TopScoreDocCollectorManager::create(capped_num_hits, TOTAL_HITS_THRESHOLD);
    }
    if (!manager.has_value()) {
        return std::unexpected(std::move(manager.error()));
    }
    return search_with_collector_manager(query, *manager.value());
}

Result<TopDocs> IndexSearcher::search_with_collector_manager(const Query &query,
                                                             const CollectorManager &manager) const {
    auto first = manager.new_collector();
    if (!first.has_value()) {
        return std::unexpected(std::move(first.error()));
    }
    const ScoreMode score_mode = first.value()->score_mode();

    // A rewritten query replaces the original one, which is why it is kept alive
    // here: the weight borrows it.
    std::shared_ptr<Query> rewritten;
    const Query *effective = &query;
    auto rewrite_result = rewrite_with_needs_scores(query, score_mode_needs_scores(score_mode));
    if (!rewrite_result.has_value()) {
        return std::unexpected(std::move(rewrite_result.error()));
    }
    if (rewrite_result.value()) {
        rewritten = std::move(rewrite_result.value());
        effective = rewritten.get();
    }

    auto weight = create_weight(*effective, score_mode, 1.0F);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }

    auto leaves = reader_->leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }

    std::vector<std::unique_ptr<Collector>> collectors;
    if (leaves.value().empty()) {
        // An index without segments has no leaf to search: reduce the single
        // collector without ever calling `search_leaf` (the reference implementation's
        // `leaf_slices.is_empty()` branch).
        collectors.push_back(std::move(first.value()));
        return reduce_single(manager, std::move(collectors));
    }
    collectors.reserve(leaves.value().size());
    collectors.push_back(std::move(first.value()));
    for (size_t i = 1; i < leaves.value().size(); ++i) {
        auto collector = manager.new_collector();
        if (!collector.has_value()) {
            return std::unexpected(std::move(collector.error()));
        }
        if (collector.value()->score_mode() != score_mode) {
            return err(LuceneError::illegal_state(
                    "CollectorManager does not always produce collectors with the same score mode"));
        }
        collectors.push_back(std::move(collector.value()));
    }

    for (size_t i = 0; i < collectors.size(); ++i) {
        if (auto collected = search_leaf(static_cast<int32_t>(i), 0, NO_MORE_DOCS, *weight.value(),
                                         *collectors[i]);
            !collected.has_value()) {
            return std::unexpected(std::move(collected.error()));
        }
    }
    return reduce_single(manager, std::move(collectors));
}

VoidResult IndexSearcher::search_with_collector(const Query &query, Collector &collector) const {
    const ScoreMode score_mode = collector.score_mode();

    std::shared_ptr<Query> rewritten;
    const Query *effective = &query;
    auto rewrite_result = rewrite_with_needs_scores(query, score_mode_needs_scores(score_mode));
    if (!rewrite_result.has_value()) {
        return std::unexpected(std::move(rewrite_result.error()));
    }
    if (rewrite_result.value()) {
        rewritten = std::move(rewrite_result.value());
        effective = rewritten.get();
    }

    auto weight = create_weight(*effective, score_mode, 1.0F);
    if (!weight.has_value()) {
        return std::unexpected(std::move(weight.error()));
    }
    if (auto set = collector.set_weight(weight.value().get()); !set.has_value()) {
        return std::unexpected(std::move(set.error()));
    }

    auto leaves = reader_->leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    for (size_t i = 0; i < leaves.value().size(); ++i) {
        if (auto collected = search_leaf(static_cast<int32_t>(i), 0, NO_MORE_DOCS, *weight.value(), collector);
            !collected.has_value()) {
            return std::unexpected(std::move(collected.error()));
        }
    }
    return {};
}

VoidResult IndexSearcher::search_leaf(int32_t ord, int32_t min_doc_id, int32_t max_doc_id, Weight &weight,
                                      Collector &collector) const {
    auto leaves = reader_->leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    if (ord < 0 || static_cast<size_t>(ord) >= leaves.value().size()) {
        return err(LuceneError::illegal_argument("leaf ordinal out of range: " + std::to_string(ord)));
    }
    const LeafReaderContext &context = leaves.value()[static_cast<size_t>(ord)];

    auto leaf_collector = collector.get_leaf_collector(context, &weight, *this);
    if (!leaf_collector.has_value()) {
        if (leaf_collector.error().kind() == ErrorKind::kCollectionTerminated) {
            // No document of interest in this leaf.
            return {};
        }
        return std::unexpected(std::move(leaf_collector.error()));
    }

    auto supplier = weight.scorer_supplier(context, *this);
    if (!supplier.has_value()) {
        return std::unexpected(std::move(supplier.error()));
    }
    if (supplier.value() != nullptr) {
        if (auto top_level = supplier.value()->set_top_level_scoring_clause(); !top_level.has_value()) {
            return std::unexpected(std::move(top_level.error()));
        }
        auto bulk_scorer = supplier.value()->bulk_scorer(context, *this);
        if (!bulk_scorer.has_value()) {
            return std::unexpected(std::move(bulk_scorer.error()));
        }
        if (bulk_scorer.value() == nullptr) {
            return err(LuceneError::illegal_state("BulkScorer is null"));
        }
        auto live_docs = context.reader()->live_docs();
        if (!live_docs.has_value()) {
            return std::unexpected(std::move(live_docs.error()));
        }
        auto scored = bulk_scorer.value()->score(*leaf_collector.value(), live_docs.value(), min_doc_id, max_doc_id);
        if (!scored.has_value() && scored.error().kind() != ErrorKind::kCollectionTerminated) {
            // A collection-terminated error just means "this leaf is done
            // early"; `finish` is still called, as in the reference implementation.
            return std::unexpected(std::move(scored.error()));
        }
    }
    return leaf_collector.value()->finish();
}


Result<std::unique_ptr<Weight>> IndexSearcher::create_weight(const Query &query, ScoreMode score_mode,
                                                            float boost) const {
    auto weight = query.create_weight(*this, score_mode, boost);
    if (!weight.has_value()) {
        return weight;
    }
    if (query_cache_ == nullptr || caching_policy_ == nullptr) {
        return weight;
    }
    if (score_mode_needs_scores(score_mode)) {
        // Cached document sets carry no scores, so the cache only applies when
        // the search does not need them.
        return weight;
    }
    // The cache keys entries on a shared pointer to the query; a query that was
    // not created through one simply cannot be cached.
    auto owned = query.weak_from_this().lock();
    if (owned == nullptr) {
        return weight;
    }
    std::shared_ptr<const Query> shared(std::move(owned));
    auto cached = query_cache_->do_cache(std::move(shared), std::move(weight.value()), caching_policy_);
    if (!cached.has_value()) {
        return std::unexpected(std::move(cached.error()));
    }
    return cached;
}

} // namespace lucene
