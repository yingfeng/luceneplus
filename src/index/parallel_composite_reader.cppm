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

export module lucene_core:parallel_composite_reader;

import :types;
import std;

import :base_composite_reader;
import :error;
import :identity;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :parallel_leaf_reader;
import :stored_fields;
import :term;
import :term_vectors;

namespace lucene {

/// A composite reader over several parallel indexes.
///
/// Port of `ParallelCompositeReader` (`index/parallel_composite_reader.rs`).
///
/// Every index must have the same `max_doc`, the same number of leaves and
/// matching leaf sizes, but each typically holds different fields.  The
/// composite reader replaces each group of corresponding leaves with one
/// [`ParallelLeafReader`], so searching it sees the union of the fields of all
/// the indexes while `max_doc` stays the same.  Deletions come from the first
/// index.
///
/// **Deviations** (C++ mechanics only):
///  * the flattened leaves are borrowed from the sub-readers (non-owning
///    `shared_ptr` aliases), because [`LeafReaderContext`] hands out a plain
///    pointer; the composite keeps the sub-readers alive;
///  * the synthetic [`ParallelLeafReader`]s are created with
///    `close_sub_readers == false`, which is this port's stand-in for the
///    reference implementation's "inflight" hook: closing this reader closes the
///    real sub-readers and the synthetic readers never touch them;
///  * closing twice is a no-op.
export class ParallelCompositeReader final : public IndexReader {
public:
    /// Combines `readers`, closing them when this reader is closed.
    [[nodiscard]] static Result<std::shared_ptr<ParallelCompositeReader>>
    create(std::vector<std::shared_ptr<IndexReader>> readers);

    /// Same as [`create`], but leaves the sub-readers open on close when
    /// `close_sub_readers` is false.
    [[nodiscard]] static Result<std::shared_ptr<ParallelCompositeReader>>
    create_with_close_sub_readers(bool close_sub_readers, std::vector<std::shared_ptr<IndexReader>> readers);

    /// Expert: takes the stored fields from `stored_fields_readers`.
    [[nodiscard]] static Result<std::shared_ptr<ParallelCompositeReader>>
    create_with_stored_fields(bool close_sub_readers, std::vector<std::shared_ptr<IndexReader>> readers,
                              std::vector<std::shared_ptr<IndexReader>> stored_fields_readers);

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return base_.max_doc(); }
    [[nodiscard]] Result<int32_t> num_docs() const override { return base_.num_docs(); }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override;
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override;
    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) override { return base_.doc_freq(term); }
    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) override { return base_.total_term_freq(term); }
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view field) override { return base_.sum_doc_freq(field); }
    [[nodiscard]] Result<int32_t> doc_count(std::string_view field) override { return base_.doc_count(field); }
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view field) override {
        return base_.sum_total_term_freq(field);
    }
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override;
    [[nodiscard]] const Identity &identity() const override { return identity_; }

    /// Closes the real sub-readers (once).
    [[nodiscard]] VoidResult close() const override;

    // ---- introspection ----

    /// The synthetic per-leaf readers, in document order.
    [[nodiscard]] const std::vector<std::shared_ptr<ParallelLeafReader>> &leaf_readers() const noexcept {
        return leaf_readers_;
    }
    [[nodiscard]] std::string to_string() const;

private:
    ParallelCompositeReader() = default;

    std::vector<std::shared_ptr<IndexReader>> readers_;
    std::vector<std::shared_ptr<IndexReader>> stored_fields_readers_;
    /// Every distinct sub-reader, so `close` touches each one once.
    std::vector<std::shared_ptr<IndexReader>> all_readers_;
    std::vector<std::shared_ptr<ParallelLeafReader>> leaf_readers_;
    CompositeReaderBase base_;
    bool close_sub_readers_{true};
    mutable std::vector<LeafReaderContext> leaves_;
    mutable bool leaves_built_{false};
    mutable bool closed_{false};
    Identity identity_{};
};

} // namespace lucene
