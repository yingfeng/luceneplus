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

export module lucene_core:leaf_reader;

import :types;
import std;

import :binary_doc_values;
import :bits;
import :doc_values_skipper;
import :error;
import :field_infos;
import :index_reader;
import :leaf_metadata;
import :leaf_reader_context;
import :numeric_doc_values;
import :point_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :term;
import :terms;

namespace lucene {

/// An atomic reader: it is not composed of sub-readers.
///
/// Port of the `LeafReader` trait (`core/index/leaf_reader.rs`).  Searching is
/// done entirely through this interface, which is why it exposes terms, doc
/// values, norms, stored fields and term vectors.
///
/// **Deviations** (C++ mechanics only):
///  * the associated types collapse into the concrete type-erased interfaces
///    (`std::unique_ptr<Terms>`, `std::unique_ptr<NumericDocValues>`, ...);
///  * a null pointer means "absent" where the reference implementation returns `None`;
///  * the float/byte vector accessors and `search_nearest_vectors*` are not
///    ported (vector formats are out of scope for this port);
///  * points are exposed through [`point_values`] only; the reference implementation's
///    `get_point_values` returns an associated `PointValues` type.
export class LeafReader : public IndexReader {
public:
    ~LeafReader() override = default;

    /// The [`Terms`] of `field`, or a null pointer when the field is not
    /// indexed.
    [[nodiscard]] virtual Result<std::unique_ptr<Terms>> terms(std::string_view field) = 0;

    /// Number of documents containing `term`; `0` when absent.  The default
    /// implementation seeks `terms(field)`.
    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) override;

    /// Number of occurrences of `term` across all documents; `0` when absent.
    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) override;

    /// Sum of the document frequencies of all terms of `field`.
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view field) override;

    /// Number of documents that have at least one term for `field`.
    [[nodiscard]] Result<int32_t> doc_count(std::string_view field) override;

    /// Sum of the total term frequencies of all terms of `field`.
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view field) override;

    /// `NumericDocValues` of `field`, or null when the field has none.
    ///
    /// Every doc values cursor returned here must be confined to the thread that
    /// obtained it.
    [[nodiscard]] virtual Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view field) = 0;

    /// `BinaryDocValues` of `field`, or null when the field has none.
    [[nodiscard]] virtual Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view field) = 0;

    /// `SortedDocValues` of `field`, or null when the field has none.
    [[nodiscard]] virtual Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view field) = 0;

    /// `SortedNumericDocValues` of `field`, or null when the field has none.
    [[nodiscard]] virtual Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view field) = 0;

    /// `SortedSetDocValues` of `field`, or null when the field has none.
    [[nodiscard]] virtual Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(std::string_view field) = 0;

    /// The norms of `field` as `NumericDocValues`, or null when the field does
    /// not index norms.
    [[nodiscard]] virtual Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view field) = 0;

    /// The doc values skip index of `field`, or null when there is none.
    [[nodiscard]] virtual Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view field) = 0;

    /// The point values of `field`, or null when the field has no points.
    ///
    /// **Deviation:** handed out as `std::shared_ptr` (the reference implementation's `Arc`), because
    /// [`PointsReader`] shares one instance between the reader and every caller.
    [[nodiscard]] virtual Result<std::shared_ptr<PointValues>> point_values(std::string_view field);

    /// The field infos of this (single) segment; never null.
    ///
    /// **Deviation:** returned by pointer, because `std::expected` cannot hold a
    /// reference.
    [[nodiscard]] virtual Result<const FieldInfos *> field_infos() const = 0;

    /// The live documents, or a null pointer when nothing is deleted.
    [[nodiscard]] virtual Result<const Bits *> live_docs() const = 0;

    /// This reader, as the single leaf of its own tree.
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override;

    [[nodiscard]] const Identity &identity() const override { return identity_; }

    /// The metadata of the segment behind this reader.
    ///
    /// **Deviation:** the reference implementation requires every leaf to expose
    /// this; readers that do not track it (composites, wrappers, test doubles)
    /// answer [`LeafMetaData::unknown`] instead of failing.
    [[nodiscard]] virtual const LeafMetaData &leaf_meta_data() const { return LeafMetaData::unknown(); }

    /// `LeafReader(<class name> max_doc=... num_docs=...)`, for diagnostics.
    [[nodiscard]] virtual std::string to_string() const;

private:
    /// Identity of this reader as a top-level reader.
    Identity identity_{};
    /// Lazily built single-element leaf array returned by [`leaves`].
    mutable std::vector<LeafReaderContext> self_leaves_{};
};

} // namespace lucene
