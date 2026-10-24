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

export module lucene_core:collector_manager;

import :types;

import std;

import :collector;
import :error;
import :top_docs;

namespace lucene {

/// Creates one [`Collector`] per leaf slice and reduces their results.
///
/// Port of the `CollectorManager` trait (`core/search/collector_manager.rs`).
///
/// **Deviation:** the reference implementation's associated types (`C`, `T`) collapse into
/// `std::unique_ptr<Collector>` / [`TopDocs`]: the port only has collectors that
/// produce a `TopDocs`, and reduction therefore has to downcast the collectors it
/// was handed.  Because the reference implementation's `MultiCollectorManager`
/// returns one result *per* wrapped manager, `reduce` answers a vector: a plain
/// manager returns exactly one element.
export class CollectorManager {
public:
    virtual ~CollectorManager() = default;

    /// A fresh collector.
    [[nodiscard]] virtual Result<std::unique_ptr<Collector>> new_collector() const = 0;

    /// Merges the per-slice results.  The collectors are consumed.
    ///
    /// A manager that produces a single result returns a one element vector; a
    /// manager that wraps several others (see
    /// [`MultiCollectorManager`](:multi_collector_manager)) returns one element per wrapped
    /// manager, in the order they were given.
    [[nodiscard]] virtual Result<std::vector<TopDocs>>
    reduce(std::vector<std::unique_ptr<Collector>> collectors) const = 0;
};

} // namespace lucene
