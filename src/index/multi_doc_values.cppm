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

export module lucene_core:multi_doc_values;

import :types;
import std;

import :binary_doc_values;
import :bytes_ref;
import :doc_values_iterator;
import :error;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :numeric_doc_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;

namespace lucene {

/// The global ordinal space of a sorted / sorted-set field spread over several
/// segments.
///
/// This replaces the reference implementation's use of `OrdinalMap` for the
/// multi-reader case: `OrdinalMap::build` takes one [`TermsEnum`] per segment,
/// and this port leaves `SortedDocValues::terms_enum` unimplemented, so the map
/// is built straight from the per-segment dictionaries instead (a k-way merge of
/// already-sorted term lists).
///
/// **Deviation:** the whole dictionary is materialised while building, which the
/// reference implementation also effectively does but streams through a terms
/// enum.  Both are "extremely slow" by design; this API is for the rare caller
/// that needs one field across a whole index.
export class GlobalOrdinalMap {
public:
    /// Merge-sorts `dictionaries` (`dictionaries[i]` must be sorted) into one
    /// dictionary; the result maps every segment ordinal to its global one.
    [[nodiscard]] static Result<GlobalOrdinalMap> build(std::vector<std::vector<BytesRef>> dictionaries);

    /// Number of distinct terms in the global space.
    [[nodiscard]] int64_t value_count() const noexcept { return static_cast<int64_t>(terms_.size()); }

    /// The term of `global_ord`, or null when it is out of range.
    [[nodiscard]] const BytesRef *term(int64_t global_ord) const;

    /// The global ordinal of `segment_ord` in `segment`, or `-1` when the
    /// segment has no such ordinal.
    [[nodiscard]] int64_t global_ord(size_t segment, int64_t segment_ord) const;

    /// The first segment that contains `global_ord`, and the ordinal there.
    [[nodiscard]] Result<std::pair<size_t, int64_t>> first_segment_of(int64_t global_ord) const;

private:
    GlobalOrdinalMap() = default;

    std::vector<BytesRef> terms_;
    /// `segment_ords_[s][ord]` is the global ordinal (segment order).
    std::vector<std::vector<int32_t>> segment_ords_;
    /// `global_ord -> first segment index`.
    std::vector<int32_t> first_segment_;
    /// `global_ord -> ordinal in the first segment`.
    std::vector<int32_t> first_segment_ord_;
};

/// Where a [`MultiNumericDocValues`] takes its per-leaf values from.
export enum class NumericSource : uint8_t {
    /// `LeafReader::get_norm_values` (norms live in their own file).
    kNorm = 0,
    /// `LeafReader::get_numeric_doc_values`.
    kDocValues = 1,
};

/// A [`NumericDocValues`] that concatenates the numeric doc values of every leaf,
/// opening one leaf cursor at a time.
///
/// Ports both `NumericDocValuesImpl` (norms) and `NumericDocValuesImpl1` (doc
/// values) of `index/multi_doc_values.rs`; they differ only in which leaf getter
/// they call, so they share one class here.
export class MultiNumericDocValues final : public NumericDocValues {
public:
    /// `leaves` must outlive the cursor (it is the reader's own leaf array).
    MultiNumericDocValues(std::span<const LeafReaderContext> leaves, std::string field, NumericSource source);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }
    [[nodiscard]] Result<int64_t> long_value() override;

private:
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> fetch(size_t leaf) const;

    std::span<const LeafReaderContext> leaves_;
    std::string field_;
    NumericSource source_;
    std::unique_ptr<NumericDocValues> current_;
    size_t next_leaf_{0};
    int32_t current_doc_base_{0};
    int32_t doc_id_{-1};
};

/// A [`BinaryDocValues`] over every leaf of a composite reader.
///
/// Port of `BinaryDocValuesImpl` (`index/multi_doc_values.rs`).
export class MultiBinaryDocValues final : public BinaryDocValues {
public:
    MultiBinaryDocValues(std::span<const LeafReaderContext> leaves, std::string field);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }
    [[nodiscard]] Result<BytesRef> binary_value() override;

private:
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> fetch(size_t leaf) const;

    std::span<const LeafReaderContext> leaves_;
    std::string field_;
    std::unique_ptr<BinaryDocValues> current_;
    size_t next_leaf_{0};
    int32_t current_doc_base_{0};
    int32_t doc_id_{-1};
};

/// A [`SortedNumericDocValues`] over every leaf, with one cursor per leaf (an
/// empty one where the field is absent from that segment).
///
/// Port of `SortedNumericDocValuesImpl` (`index/multi_doc_values.rs`).
export class MultiSortedNumericDocValues final : public SortedNumericDocValues {
public:
    MultiSortedNumericDocValues(std::vector<std::unique_ptr<SortedNumericDocValues>> values,
                                std::span<const LeafReaderContext> leaves, int32_t max_doc, int64_t total_cost);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return total_cost_; }
    [[nodiscard]] Result<int64_t> next_value() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override;

private:
    std::vector<std::unique_ptr<SortedNumericDocValues>> values_;
    std::span<const LeafReaderContext> leaves_;
    int32_t max_doc_{0};
    int64_t total_cost_{0};
    size_t next_leaf_{0};
    int32_t current_index_{-1};
    int32_t current_doc_base_{0};
    int32_t doc_id_{-1};
};

/// A [`SortedDocValues`] over every leaf, with the ordinals mapped into one
/// global space.
///
/// Port of `MultiSortedDocValues` (`index/multi_doc_values.rs`).
export class MultiSortedDocValues final : public SortedDocValues {
public:
    MultiSortedDocValues(std::vector<std::unique_ptr<SortedDocValues>> values,
                         std::span<const LeafReaderContext> leaves, int32_t max_doc, GlobalOrdinalMap mapping,
                         int64_t total_cost);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return total_cost_; }

    [[nodiscard]] Result<int32_t> ord_value() override;
    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override;
    [[nodiscard]] Result<int32_t> get_value_count() const override;

private:
    [[nodiscard]] Result<int32_t> sub_reader_index(int32_t target) const;

    std::vector<std::unique_ptr<SortedDocValues>> values_;
    std::span<const LeafReaderContext> leaves_;
    int32_t max_doc_{0};
    GlobalOrdinalMap mapping_;
    int64_t total_cost_{0};
    size_t next_leaf_{0};
    int32_t current_index_{-1};
    int32_t current_doc_base_{0};
    int32_t doc_id_{-1};
};

/// A [`SortedSetDocValues`] over every leaf, with the ordinals mapped into one
/// global space.
///
/// Port of `MultiSortedSetDocValues` (`index/multi_doc_values.rs`).
export class MultiSortedSetDocValues final : public SortedSetDocValues {
public:
    MultiSortedSetDocValues(std::vector<std::unique_ptr<SortedSetDocValues>> values,
                            std::span<const LeafReaderContext> leaves, int32_t max_doc, GlobalOrdinalMap mapping,
                            int64_t total_cost);

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }
    [[nodiscard]] Result<int32_t> next_doc() override;
    [[nodiscard]] Result<int32_t> advance(int32_t target) override;
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override;
    [[nodiscard]] Result<int64_t> cost() const override { return total_cost_; }

    [[nodiscard]] Result<int64_t> next_ord() override;
    [[nodiscard]] Result<int32_t> doc_value_count() override;
    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override;
    [[nodiscard]] Result<int64_t> get_value_count() const override;

private:
    std::vector<std::unique_ptr<SortedSetDocValues>> values_;
    std::span<const LeafReaderContext> leaves_;
    int32_t max_doc_{0};
    GlobalOrdinalMap mapping_;
    int64_t total_cost_{0};
    size_t next_leaf_{0};
    int32_t current_index_{-1};
    int32_t current_doc_base_{0};
    int32_t doc_id_{-1};
};

/// Builds the composite doc values cursors, merging the leaves on the fly.
///
/// Port of `MultiDocValues` (`index/multi_doc_values.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * every getter returns a null pointer when no leaf has the field, matching
///    the `LeafReader` getters of this port;
///  * the single-leaf case returns the leaf's own cursor (so a cursor type that
///    supports more, such as `terms_enum`, keeps doing so) instead of wrapping it
///    in the reference implementation's two-alternative enum;
///  * the sorted / sorted-set cursors build their global ordinals from the
///    per-segment dictionaries (see [`GlobalOrdinalMap`]) instead of through
///    `OrdinalMap::build`, and `cost()` errors are ignored while summing.
export class MultiDocValues {
public:
    /// `NumericDocValues` of the norms of `field`, merged over all leaves.
    [[nodiscard]] static Result<std::unique_ptr<NumericDocValues>> get_norm_values(const IndexReader &reader,
                                                                                  std::string_view field);

    /// `NumericDocValues` of `field`, merged over all leaves.
    [[nodiscard]] static Result<std::unique_ptr<NumericDocValues>> get_numeric_values(const IndexReader &reader,
                                                                                     std::string_view field);

    /// `BinaryDocValues` of `field`, merged over all leaves.
    [[nodiscard]] static Result<std::unique_ptr<BinaryDocValues>> get_binary_values(const IndexReader &reader,
                                                                                   std::string_view field);

    /// `SortedNumericDocValues` of `field`, merged over all leaves.
    [[nodiscard]] static Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_values(const IndexReader &reader, std::string_view field);

    /// `SortedDocValues` of `field` with global ordinals.
    ///
    /// **NOTE:** extremely slow; prefer the per-leaf getters.
    [[nodiscard]] static Result<std::unique_ptr<SortedDocValues>> get_sorted_values(const IndexReader &reader,
                                                                                   std::string_view field);

    /// `SortedSetDocValues` of `field` with global ordinals.
    ///
    /// **NOTE:** extremely slow; prefer the per-leaf getters.
    [[nodiscard]] static Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_values(const IndexReader &reader,
                                                                                          std::string_view field);
};

} // namespace lucene
