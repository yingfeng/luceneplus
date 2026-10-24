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

export module lucene_core:multi_bits;

import :types;
import std;

import :bits;
import :error;
import :index_reader;

namespace lucene {

/// Concatenates several [`Bits`] on every lookup.
///
/// Port of `MultiBits` (`index/multi_bits.rs`).
///
/// **NOTE:** this is a very costly view: every lookup binary-searches `starts`
/// to find the sub-set.  Iterating the leaves yourself is always cheaper.
///
/// A null sub-set means "no bits were recorded for that reader", in which case
/// [`default_value`] is returned.  That is how live documents are merged: a
/// segment without deletions has no live-docs bits, and every document in it is
/// live.
export class MultiBits final : public Bits {
public:
    /// Wraps `subs`; `starts` has one entry per sub-set plus a trailing
    /// `length`.
    ///
    /// Fails with `illegal_argument` when the sizes do not match or `starts` is
    /// not non-decreasing.
    [[nodiscard]] static Result<MultiBits> create(std::vector<std::shared_ptr<const Bits>> subs,
                                                  std::vector<int32_t> starts,
                                                  bool default_value);

    [[nodiscard]] Result<bool> get(size_t index) const override;
    [[nodiscard]] size_t length() const override;
    [[nodiscard]] std::string to_string() const override;

    // ---- introspection ----

    [[nodiscard]] const std::vector<std::shared_ptr<const Bits>> &subs() const noexcept { return subs_; }
    [[nodiscard]] const std::vector<int32_t> &starts() const noexcept { return starts_; }
    [[nodiscard]] bool default_value() const noexcept { return default_value_; }

private:
    MultiBits(std::vector<std::shared_ptr<const Bits>> subs, std::vector<int32_t> starts, bool default_value);

    std::vector<std::shared_ptr<const Bits>> subs_;
    /// One entry per sub-set plus the trailing length, exactly like the
    /// composite readers' `starts` array.
    std::vector<int32_t> starts_;
    bool default_value_;
};

/// Merges the live documents of every leaf of `reader`.
///
/// Port of `get_live_docs` (`index/multi_bits.rs`).  Returns a null pointer when
/// the reader has no deletions, and the leaf's own bits when it has a single
/// leaf.
///
/// **Deviation:** a single leaf's bits are borrowed from that leaf, so the
/// returned pointer must not outlive `reader`.
export [[nodiscard]] Result<std::shared_ptr<const Bits>> get_live_docs(const IndexReader &reader);

} // namespace lucene
