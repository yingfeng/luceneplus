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

export module lucene_core:top_score_doc_collector_manager;

import :types;

import std;

import :collector;
import :collector_manager;
import :error;
import :max_score_accumulator;
import :score_doc;
import :top_docs;

namespace lucene {

/// Creates [`TopScoreDocCollector`](lucene_core:top_score_doc_collector)s that
/// share a hit counter and a maximum-score accumulator, and merges their results.
///
/// Port of `TopScoreDocCollectorManager`
/// (`core/search/top_score_doc_collector_manager.rs`).
///
/// A new manager must be created for each search: it carries the cross-leaf
/// state.  [`create`] builds first-page collectors (no search-after cursor,
/// which is the reference implementation's `CHECK_AFTER = false`); [`with_after`] keeps the cursor
/// support.
export class TopScoreDocCollectorManager final : public CollectorManager {
public:
    /// Creates a manager with a search-after cursor.
    ///
    /// `total_hits_threshold` is how many hits are counted exactly; above it the
    /// reported count becomes a lower bound.  `INT32_MAX` makes it exact but
    /// slower.
    [[nodiscard]] static Result<std::unique_ptr<TopScoreDocCollectorManager>>
    with_after(size_t num_hits, std::optional<ScoreDoc> after, size_t total_hits_threshold);

    /// Creates a first-page manager; equivalent to [`with_after`] without a
    /// cursor and without per-document cursor checks.
    [[nodiscard]] static Result<std::unique_ptr<TopScoreDocCollectorManager>> create(size_t num_hits,
                                                                                     size_t total_hits_threshold);

    [[nodiscard]] Result<std::unique_ptr<Collector>> new_collector() const override;

    [[nodiscard]] Result<std::vector<TopDocs>>
    reduce(std::vector<std::unique_ptr<Collector>> collectors) const override;

    [[nodiscard]] size_t num_hits() const noexcept { return num_hits_; }

private:
    /// The single result of the collectors; [`reduce`] wraps it in a vector.
    [[nodiscard]] Result<TopDocs> reduce_one(std::vector<std::unique_ptr<Collector>> collectors) const;

    TopScoreDocCollectorManager(size_t num_hits, std::optional<ScoreDoc> after, size_t total_hits_threshold,
                                std::shared_ptr<MaxScoreAccumulator> min_score_acc, bool check_after);

    size_t num_hits_;
    std::optional<ScoreDoc> after_;
    size_t total_hits_threshold_;
    std::shared_ptr<MaxScoreAccumulator> min_score_acc_;
    bool check_after_;
};

} // namespace lucene
