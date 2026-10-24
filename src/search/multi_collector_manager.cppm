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

export module lucene_core:multi_collector_manager;

import :types;
import std;

import :collector;
import :collector_manager;
import :error;
import :top_docs;

namespace lucene {

/// A [`CollectorManager`] that runs several collector managers at once.
///
/// Port of `MultiCollectorManager` (`core/search/multi_collector_manager.rs`).
///
/// [`new_collector`] answers one collector that fans every hit out to a collector
/// per wrapped manager (via [`MultiCollector`](:multi_collector)), and [`reduce`]
/// splits that collected bundle again so every wrapped manager reduces its own
/// share: the result holds one entry per wrapped manager, in the order they were
/// given.
///
/// **Deviation:** the reference implementation's `OneOrMultiCollector` collapses a
/// single wrapped manager into the collector itself; this port always builds the
/// fan-out collector, which costs one indirection and keeps the reduce step
/// uniform.
export class MultiCollectorManager final : public CollectorManager {
public:
    /// Wraps `collector_managers`, which must not be empty and must not hold
    /// null pointers.
    [[nodiscard]] static Result<std::shared_ptr<MultiCollectorManager>>
    create(std::vector<std::shared_ptr<CollectorManager>> collector_managers);

    /// A collector that owns one collector per wrapped manager.
    [[nodiscard]] Result<std::unique_ptr<Collector>> new_collector() const override;

    /// One result per wrapped manager; a wrapped manager that answers several
    /// results contributes all of them, in order.
    [[nodiscard]] Result<std::vector<TopDocs>>
    reduce(std::vector<std::unique_ptr<Collector>> collectors) const override;

    [[nodiscard]] const std::vector<std::shared_ptr<CollectorManager>> &collector_managers() const noexcept {
        return collector_managers_;
    }
    [[nodiscard]] size_t size() const noexcept { return collector_managers_.size(); }

private:
    explicit MultiCollectorManager(std::vector<std::shared_ptr<CollectorManager>> collector_managers)
        : collector_managers_(std::move(collector_managers)) {}

    std::vector<std::shared_ptr<CollectorManager>> collector_managers_;
};

} // namespace lucene
