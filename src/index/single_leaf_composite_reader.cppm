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

export module lucene_core:single_leaf_composite_reader;

import :types;
import std;

import :close;
import :error;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :stored_fields;
import :term;
import :term_vectors;

namespace lucene {

/// A [`IndexReader`] over exactly one [`LeafReader`].
///
/// Port of `SingleLeafCompositeReader` (`index/single_leaf_composite_reader.rs`).
///
/// It exists for the cases where an `IndexReader` is required but the caller has
/// a leaf in hand: an `IndexSearcher` over one segment, a query executed against
/// a segment alone, a test fixture.  Everything (document counts, term
/// statistics, stored fields, term vectors) is delegated to the leaf, and the
/// single leaf is published as the one and only leaf of the tree, with
/// `doc_base = 0`.
///
/// **Deviation:** the reader owns the leaf, so [`close`] closes it; the reference
/// implementation shares an `Arc` and lets the last owner drop it.
export class SingleLeafCompositeReader final : public IndexReader {
public:
    /// Wraps `leaf`; it must not be null.
    [[nodiscard]] static Result<std::shared_ptr<SingleLeafCompositeReader>> create(std::shared_ptr<LeafReader> leaf);

    /// The wrapped leaf.
    [[nodiscard]] const std::shared_ptr<LeafReader> &leaf() const noexcept { return leaf_; }

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override;
    [[nodiscard]] Result<int32_t> num_docs() const override;
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override;
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override;
    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) override;
    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) override;
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view field) override;
    [[nodiscard]] Result<int32_t> doc_count(std::string_view field) override;
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view field) override;

    /// The single leaf, with `doc_base = 0`.
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override;

    /// The identity of the wrapped leaf, like the reference implementation's
    /// `index_base`.
    [[nodiscard]] const Identity &identity() const override;

    [[nodiscard]] VoidResult close() const override;

    [[nodiscard]] std::string to_string() const;

private:
    explicit SingleLeafCompositeReader(std::shared_ptr<LeafReader> leaf) : leaf_(std::move(leaf)) {}

    std::shared_ptr<LeafReader> leaf_;
    /// Built on first use, and only once.
    mutable std::vector<LeafReaderContext> leaves_;
};

} // namespace lucene
