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

export module lucene_core:parallel_leaf_reader;

import :types;
import std;

import :binary_doc_values;
import :bits;
import :document;
import :doc_values_skipper;
import :error;
import :field_infos;
import :fields;
import :index_reader;
import :leaf_reader;
import :numeric_doc_values;
import :point_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :stored_field_visitor;
import :stored_fields;
import :term;
import :term_vectors;
import :terms;

namespace lucene {

/// The term vectors of several parallel readers, exposed as one [`Fields`].
///
/// **Deviation:** the [`Fields`] borrows the cursors of the
/// [`ParallelTermVectors`] that produced it, because `Fields::terms` of this
/// port returns an owning pointer and cannot share the reference
/// implementation's `Arc`.  The `Fields` must not outlive its `TermVectors`.
class ParallelFields final : public Fields {
public:
    ParallelFields(std::vector<TermVectors *> cursors, std::map<std::string, size_t, std::less<>> field_to_cursor,
                   int32_t doc);

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override;
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override;
    [[nodiscard]] Result<int32_t> size() const override;

private:
    std::vector<TermVectors *> cursors_;
    std::map<std::string, size_t, std::less<>> field_to_cursor_;
    int32_t doc_;
};

/// The stored fields of several parallel readers, concatenated per document.
class ParallelStoredFields final : public StoredFields {
public:
    explicit ParallelStoredFields(std::vector<std::unique_ptr<StoredFields>> cursors);

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) override;
    [[nodiscard]] Result<Document> document(int32_t doc_id) override;

private:
    std::vector<std::unique_ptr<StoredFields>> cursors_;
};

/// The term vectors of several parallel readers.
class ParallelTermVectors final : public TermVectors {
public:
    ParallelTermVectors(std::vector<std::unique_ptr<TermVectors>> cursors,
                        std::map<std::string, size_t, std::less<>> field_to_cursor);

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t doc) override;
    [[nodiscard]] Result<std::unique_ptr<Terms>> get_field_terms(int32_t doc, std::string_view field) override;

private:
    std::vector<std::unique_ptr<TermVectors>> cursors_;
    std::map<std::string, size_t, std::less<>> field_to_cursor_;
};

/// A [`LeafReader`] that reads several, parallel indexes as one.
///
/// Port of `ParallelLeafReader` (`index/parallel_leaf_reader.rs`).
///
/// Every index must have the same number of documents and the same `max_doc`,
/// but each typically holds different fields.  A document sees the union of the
/// fields with the same document number, and a term is served by the first
/// reader that has the field.  Deletions come from the first reader.
///
/// **Warning:** nothing validates that the indexes were built the same way; if
/// documents were added in different orders the result is undefined.
///
/// **Deviations** (C++ mechanics only):
///  * sub-readers are `std::shared_ptr`s instead of ref-counted values, and
///    `close_sub_readers == false` simply leaves them open;
///  * `LeafMetaData` (index sort, creation version, blocks) has no counterpart
///    in this port's [`LeafReader`], so the reference implementation's
///    compatibility checks on those are not ported;
///  * closing twice is a no-op.
export class ParallelLeafReader final : public LeafReader {
public:
    /// Combines `readers`, closing them when this reader is closed.
    [[nodiscard]] static Result<std::shared_ptr<ParallelLeafReader>>
    create(std::vector<std::shared_ptr<LeafReader>> readers);

    /// Same as [`create`], but leaves the sub-readers open on close when
    /// `close_sub_readers` is false.
    [[nodiscard]] static Result<std::shared_ptr<ParallelLeafReader>>
    create_with_close_sub_readers(bool close_sub_readers, std::vector<std::shared_ptr<LeafReader>> readers);

    /// Expert: takes the stored fields from `stored_fields_readers` instead of
    /// from `readers` (which must then hold at least one reader).
    [[nodiscard]] static Result<std::shared_ptr<ParallelLeafReader>>
    create_with_stored_fields(bool close_sub_readers, std::vector<std::shared_ptr<LeafReader>> readers,
                              std::vector<std::shared_ptr<LeafReader>> stored_fields_readers);

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<bool> has_deletions() const override { return has_deletions_; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override;
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override;

    /// Closes the sub-readers (once).  An I/O failure is reported after every
    /// reader has been closed; any other failure stops the loop.
    [[nodiscard]] VoidResult close() const override;

    // ---- LeafReader ----

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view field) override;
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view field) override;
    [[nodiscard]] Result<std::shared_ptr<PointValues>> point_values(std::string_view field) override;
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override;

    /// The merged metadata of the sub-readers.
    ///
    /// Port of `ParallelLeafReader::get_metadata`: the creation version and the
    /// index sort must agree across the sub-readers, `min_version` is the oldest
    /// of them (none when any sub-reader has none) and `has_blocks` is their
    /// disjunction.
    [[nodiscard]] const LeafMetaData &leaf_meta_data() const override { return leaf_meta_data_; }

    [[nodiscard]] std::string to_string() const override;

    // ---- introspection ----

    /// The readers passed on initialization, in order.
    [[nodiscard]] const std::vector<std::shared_ptr<LeafReader>> &parallel_readers() const noexcept {
        return readers_;
    }
    /// The merged field infos (first reader wins for a shared field name).
    [[nodiscard]] const FieldInfos &merged_field_infos() const noexcept { return field_infos_; }

private:
    ParallelLeafReader() = default;

    [[nodiscard]] Result<std::shared_ptr<LeafReader>> reader_for_field(std::string_view field) const;
    [[nodiscard]] Result<std::shared_ptr<LeafReader>> reader_for_terms(std::string_view field) const;

    std::vector<std::shared_ptr<LeafReader>> readers_;
    std::vector<std::shared_ptr<LeafReader>> stored_fields_readers_;
    /// Every distinct sub-reader, so `close` touches each one once.
    std::vector<std::shared_ptr<LeafReader>> all_readers_;
    bool close_sub_readers_{true};
    int32_t max_doc_{0};
    int32_t num_docs_{0};
    bool has_deletions_{false};
    FieldInfos field_infos_{FieldInfos::none()};
    /// Field name to the index in [`readers_`] that serves it.
    std::map<std::string, size_t, std::less<>> field_to_reader_;
    /// Field names that are indexed (and therefore have terms).
    std::map<std::string, size_t, std::less<>> terms_field_to_reader_;
    /// Field names with term vectors, mapped to the index in [`readers_`].
    std::map<std::string, size_t, std::less<>> tv_field_to_reader_;
    /// Merged metadata of the sub-readers.
    LeafMetaData leaf_meta_data_{LeafMetaData::unknown()};
    mutable bool closed_{false};
};

} // namespace lucene
