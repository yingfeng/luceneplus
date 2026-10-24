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

export module lucene_core:segment_reader;

import :types;
import std;

import :bits;
import :codec_reader;
import :directory;
import :doc_values_producer;
import :error;
import :field_infos;
import :fields_producer;
import :fixed_bit_set;
import :io_context;
import :norms_producer;
import :points_reader;
import :segment_commit_info;
import :segment_core_readers;
import :stored_fields_reader;
import :term_vectors_reader;

namespace lucene {

/// A [`LeafReader`] over one on-disk segment.
///
/// Port of `SegmentReader` (`index/segment_reader.rs`), reduced to what a
/// non-NRT reader needs: the codec readers of the segment plus the live docs of
/// the commit being read.
///
/// **Deviations**:
///  * near-real-time readers (`new_from_reader`), doc-values updates and
///    field-infos updates are not ported: a segment is always read as its
///    `.si`/`.fnm` files describe it, so the field infos are the core ones and
///    there is no `init_doc_values_producer` step;
///  * the reference counting / parent-reader registry / cache helper are
///    replaced by `std::shared_ptr`;
///  * `LeafMetaData` (created version, index sort, blocks) is not modelled;
///    only `created_version_major` is kept, for diagnostics.
///
/// **Lifetime:** the [`Directory`] passed to [`open`] must outlive the reader;
/// the segment readers keep reading from it (or from the compound directory
/// built over it) on every call.
export class SegmentReader final : public CodecReader {
public:
    ~SegmentReader() override = default;

    /// Opens `si` from `dir`.
    ///
    /// `created_version_major` is the Lucene major version recorded in
    /// `segments_N` when the index was created; it only feeds diagnostics in
    /// this port.
    [[nodiscard]] static Result<std::shared_ptr<SegmentReader>> open(Directory &dir, const SegmentCommitInfo &si,
                                                                    int32_t created_version_major,
                                                                    const IOContext &context);

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return segment_info_.info.max_doc; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }

    // ---- LeafReader ----

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return field_infos_.get(); }

    /// The live documents of this commit, or null when nothing is deleted.
    [[nodiscard]] Result<const Bits *> live_docs() const override {
        return live_docs_.has_value() ? static_cast<const Bits *>(&live_docs_.value())
                                      : static_cast<const Bits *>(nullptr);
    }

    [[nodiscard]] std::string to_string() const override;

    // ---- CodecReader ----

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override;
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override;
    [[nodiscard]] NormsProducer *get_norms_reader() override;
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override;
    [[nodiscard]] FieldsProducer *get_postings_reader() override;
    [[nodiscard]] PointsReader *get_points_reader() override;

    // ---- segment accessors ----

    /// The commit this reader was opened from.
    [[nodiscard]] const SegmentCommitInfo &segment_info() const noexcept { return segment_info_; }
    /// Name of the segment (`_0`, `_1`, ...).
    [[nodiscard]] std::string_view segment_name() const noexcept { return segment_info_.info.name; }
    /// The shared codec readers, so a caller can reuse them.
    [[nodiscard]] const std::shared_ptr<SegmentCoreReaders> &core() const noexcept { return core_; }
    /// Lucene major version the index was created with.
    [[nodiscard]] int32_t created_version_major() const noexcept { return created_version_major_; }

    /// The metadata of this segment, built from its `SegmentInfo`.
    [[nodiscard]] const LeafMetaData &leaf_meta_data() const override { return leaf_meta_data_; }

private:
    SegmentReader() = default;

    SegmentCommitInfo segment_info_;
    int32_t created_version_major_{0};
    int32_t num_docs_{0};
    /// Live documents; empty when `has_deletions()` is false.
    std::optional<FixedBitSet> live_docs_;
    std::shared_ptr<SegmentCoreReaders> core_;
    LeafMetaData leaf_meta_data_{LeafMetaData::unknown()};
    std::shared_ptr<const FieldInfos> field_infos_;
};

} // namespace lucene
