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

module lucene_core:top_score_doc_collector_manager.impl;

import :top_score_doc_collector_manager;

import :collector;
import :collector_manager;
import :error;
import :max_score_accumulator;
import :score_doc;
import :top_docs;
import :top_score_doc_collector;
import :types;

import std;

namespace lucene {

TopScoreDocCollectorManager::TopScoreDocCollectorManager(size_t num_hits, std::optional<ScoreDoc> after,
                                                         size_t total_hits_threshold,
                                                         std::shared_ptr<MaxScoreAccumulator> min_score_acc,
                                                         bool check_after)
    : num_hits_(num_hits), after_(std::move(after)), total_hits_threshold_(total_hits_threshold),
      min_score_acc_(std::move(min_score_acc)), check_after_(check_after) {}

Result<std::unique_ptr<TopScoreDocCollectorManager>> TopScoreDocCollectorManager::with_after(
        size_t num_hits, std::optional<ScoreDoc> after, size_t total_hits_threshold) {
    constexpr size_t kMaxInt32 = static_cast<size_t>(std::numeric_limits<int32_t>::max());

    if (total_hits_threshold > kMaxInt32) {
        return err(LuceneError::illegal_argument("totalHitsThreshold must be < " + std::to_string(kMaxInt32) +
                                                 ", got " + std::to_string(total_hits_threshold)));
    }
    if (num_hits > kMaxInt32) {
        return err(LuceneError::illegal_argument(
                "numHits must be <= " + std::to_string(kMaxInt32) +
                "; please use TotalHitCountCollectorManager if you just need the total hit count"));
    }

    // The threshold can never be below the number of requested hits, otherwise
    // the collector would stop counting before its queue is even full.
    const size_t capped_threshold = std::max(total_hits_threshold, num_hits);
    const std::shared_ptr<MaxScoreAccumulator> min_score_acc =
            capped_threshold == kMaxInt32 ? nullptr : std::make_shared<MaxScoreAccumulator>();

    return std::unique_ptr<TopScoreDocCollectorManager>(
            new TopScoreDocCollectorManager(num_hits, std::move(after), capped_threshold, min_score_acc,
                                            /*check_after=*/true));
}

Result<std::unique_ptr<TopScoreDocCollectorManager>> TopScoreDocCollectorManager::create(
        size_t num_hits, size_t total_hits_threshold) {
    auto manager = with_after(num_hits, std::nullopt, total_hits_threshold);
    if (!manager.has_value()) {
        return std::unexpected(std::move(manager.error()));
    }
    // First-page manager: the collectors keep the `check_after = false` fast path.
    return std::unique_ptr<TopScoreDocCollectorManager>(
            new TopScoreDocCollectorManager(num_hits, std::nullopt, manager.value()->total_hits_threshold_,
                                            manager.value()->min_score_acc_, /*check_after=*/false));
}

Result<std::unique_ptr<Collector>> TopScoreDocCollectorManager::new_collector() const {
    auto collector = TopScoreDocCollector::create(num_hits_, after_, total_hits_threshold_, min_score_acc_,
                                                  check_after_);
    if (!collector.has_value()) {
        return std::unexpected(std::move(collector.error()));
    }
    return std::unique_ptr<Collector>(std::move(collector.value()));
}

Result<std::vector<TopDocs>>
TopScoreDocCollectorManager::reduce(std::vector<std::unique_ptr<Collector>> collectors) const {
    auto single = reduce_one(std::move(collectors));
    if (!single.has_value()) {
        return std::unexpected(std::move(single.error()));
    }
    std::vector<TopDocs> results;
    results.push_back(std::move(single.value()));
    return results;
}

Result<TopDocs> TopScoreDocCollectorManager::reduce_one(
        std::vector<std::unique_ptr<Collector>> collectors) const {
    if (collectors.size() == 1) {
        auto *only = dynamic_cast<TopScoreDocCollector *>(collectors.front().get());
        if (only == nullptr) {
            return err(LuceneError::illegal_state("collector is not a TopScoreDocCollector"));
        }
        return only->top_docs();
    }

    std::vector<TopDocs> per_slice;
    per_slice.reserve(collectors.size());
    for (const auto &collector : collectors) {
        auto *top_score_doc = dynamic_cast<TopScoreDocCollector *>(collector.get());
        if (top_score_doc == nullptr) {
            return err(LuceneError::illegal_state("collector is not a TopScoreDocCollector"));
        }
        auto docs = top_score_doc->top_docs();
        if (!docs.has_value()) {
            return std::unexpected(std::move(docs.error()));
        }
        per_slice.push_back(std::move(docs.value()));
    }
    return merge_top_docs_with_start(0, num_hits_, per_slice);
}

} // namespace lucene
