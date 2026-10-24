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

export module lucene_core:multi_reader;

import :types;
import std;

import :base_composite_reader;
import :error;
import :identity;
import :index_reader;
import :leaf_reader_context;
import :stored_fields;
import :term;
import :term_vectors;

namespace lucene {

/// Reads several indexes as one, appending their documents in order.
///
/// Port of `MultiReader` (`index/multi_reader.rs`).
///
/// Document numbers are global: the documents of sub-reader `i` start at
/// `doc_base(i)` and the leaves reported by [`leaves`] carry that offset, so a
/// search sees one flat index.
///
/// **Deviations** (C++ mechanics only):
///  * sub-readers are `std::shared_ptr<IndexReader>`, so the reference
///    implementation's ref-count bookkeeping for `close_sub_readers == false` has
///    no counterpart: [`close`] simply leaves the sub-readers open;
///  * the reference implementation's `Display` is unimplemented (`todo!()`); here
///    [`to_string`] lists the sub-reader count.
export class MultiReader final : public IndexReader {
public:
    ~MultiReader() override = default;

    /// Concatenates `sub_readers`, closing them when this reader is closed.
    ///
    /// An empty list is allowed and yields an empty index.
    [[nodiscard]] static Result<std::shared_ptr<MultiReader>>
    create(std::vector<std::shared_ptr<IndexReader>> sub_readers);

    /// Same as [`create`], but leaves the sub-readers open on [`close`] when
    /// `close_sub_readers` is false (their owner keeps using them).
    [[nodiscard]] static Result<std::shared_ptr<MultiReader>>
    create_with_close_sub_readers(bool close_sub_readers, std::vector<std::shared_ptr<IndexReader>> sub_readers);

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

    /// Closes the sub-readers when [`create`] was used.
    ///
    /// An I/O failure does not stop the loop (the remaining readers are still
    /// closed); the first one is returned.  Any other failure is returned
    /// immediately.  Calling this twice is a no-op.
    [[nodiscard]] VoidResult close() const override;

    // ---- introspection ----

    [[nodiscard]] const std::vector<std::shared_ptr<IndexReader>> &sub_readers() const noexcept {
        return base_.sub_readers();
    }
    [[nodiscard]] bool close_sub_readers() const noexcept { return close_sub_readers_; }

    [[nodiscard]] std::string to_string() const;

private:
    MultiReader() = default;

    CompositeReaderBase base_;
    bool close_sub_readers_{true};
    mutable std::vector<LeafReaderContext> leaves_;
    mutable bool leaves_built_{false};
    mutable bool closed_{false};
    Identity identity_{};
};

} // namespace lucene
