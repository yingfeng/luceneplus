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

module lucene_core:multi_collector_manager.impl;

import :multi_collector_manager;

import std;

import :collector;
import :collector_manager;
import :error;
import :leaf_reader_context;
import :multi_collector;
import :score_mode;
import :top_docs;
import :types;
import :weight;

namespace lucene {

namespace {

/// A [`MultiCollector`] that owns the collectors it fans out to.
///
/// The port's `MultiCollector` only borrows its children (the reference
/// implementation's `OneOrMultiCollector` owns them), so this wrapper keeps them
/// alive and hands their ownership back at reduce time.
class OwningMultiCollector final : public Collector {
public:
    OwningMultiCollector(std::vector<std::unique_ptr<Collector>> owned, std::unique_ptr<MultiCollector> multi)
        : owned_(std::move(owned)), multi_(std::move(multi)) {}

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext &context, const Weight *weight,
                       const IndexSearcher &searcher) override {
        return multi_->get_leaf_collector(context, weight, searcher);
    }

    [[nodiscard]] ScoreMode score_mode() const override { return multi_->score_mode(); }

    [[nodiscard]] VoidResult set_weight(const Weight *weight) override { return multi_->set_weight(weight); }

    /// The per-manager collectors, released from the fan-out wrapper.
    [[nodiscard]] std::vector<std::unique_ptr<Collector>> take_collectors() {
        multi_.reset();
        return std::move(owned_);
    }

private:
    std::vector<std::unique_ptr<Collector>> owned_;
    std::unique_ptr<MultiCollector> multi_;
};

} // namespace

Result<std::shared_ptr<MultiCollectorManager>>
MultiCollectorManager::create(std::vector<std::shared_ptr<CollectorManager>> collector_managers) {
    if (collector_managers.empty()) {
        return err(LuceneError::illegal_argument("There must be at least one collector manager"));
    }
    for (const std::shared_ptr<CollectorManager> &manager : collector_managers) {
        if (manager == nullptr) {
            return err(LuceneError::illegal_argument("a collector manager must not be null"));
        }
    }
    return std::shared_ptr<MultiCollectorManager>(new MultiCollectorManager(std::move(collector_managers)));
}

Result<std::unique_ptr<Collector>> MultiCollectorManager::new_collector() const {
    std::vector<std::unique_ptr<Collector>> owned;
    std::vector<Collector *> borrowed;
    owned.reserve(collector_managers_.size());
    borrowed.reserve(collector_managers_.size());

    for (const std::shared_ptr<CollectorManager> &manager : collector_managers_) {
        auto collector = manager->new_collector();
        if (!collector.has_value()) {
            return std::unexpected(std::move(collector.error()));
        }
        if (collector.value() == nullptr) {
            return err(LuceneError::illegal_state("a collector manager produced no collector"));
        }
        borrowed.push_back(collector.value().get());
        owned.push_back(std::move(collector.value()));
    }

    auto multi = MultiCollector::wrap(borrowed);
    if (!multi.has_value()) {
        return std::unexpected(std::move(multi.error()));
    }
    return std::unique_ptr<Collector>(new OwningMultiCollector(std::move(owned), std::move(multi.value())));
}

Result<std::vector<TopDocs>>
MultiCollectorManager::reduce(std::vector<std::unique_ptr<Collector>> collectors) const {
    std::vector<std::vector<std::unique_ptr<Collector>>> by_manager(collector_managers_.size());
    for (std::unique_ptr<Collector> &collector : collectors) {
        auto *owning = dynamic_cast<OwningMultiCollector *>(collector.get());
        if (owning == nullptr) {
            return err(LuceneError::illegal_state("the collector was not produced by this manager"));
        }
        std::vector<std::unique_ptr<Collector>> children = owning->take_collectors();
        if (children.size() != collector_managers_.size()) {
            return err(LuceneError::illegal_state("expected " + std::to_string(collector_managers_.size()) +
                                                  " collectors, got " + std::to_string(children.size())));
        }
        for (size_t index = 0; index < children.size(); ++index) {
            by_manager[index].push_back(std::move(children[index]));
        }
    }

    std::vector<TopDocs> results;
    for (size_t index = 0; index < collector_managers_.size(); ++index) {
        auto reduced = collector_managers_[index]->reduce(std::move(by_manager[index]));
        if (!reduced.has_value()) {
            return std::unexpected(std::move(reduced.error()));
        }
        for (TopDocs &docs : reduced.value()) {
            results.push_back(std::move(docs));
        }
    }
    return results;
}

} // namespace lucene
