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

module lucene_core:segment_reader.impl;

import :segment_reader;

import :codec;
import :codec_formats;
import :error;
import :live_docs_format;
import :types;

import std;

namespace lucene {

Result<std::shared_ptr<SegmentReader>> SegmentReader::open(Directory &dir, const SegmentCommitInfo &si,
                                                           int32_t created_version_major, const IOContext &context) {
    auto validated = si.validate();
    if (!validated.has_value()) {
        return std::unexpected(std::move(validated.error()));
    }

    std::shared_ptr<SegmentReader> reader(new SegmentReader());
    reader->segment_info_ = si;
    reader->created_version_major_ = created_version_major;

    auto core = SegmentCoreReaders::open(dir, si, context);
    if (!core.has_value()) {
        return std::unexpected(std::move(core.error()));
    }
    reader->core_ = std::move(core.value());
    reader->field_infos_ = reader->core_->core_field_infos();
    // The sort order is recorded per segment since index sorting is ported.
    std::shared_ptr<const Sort> sort;
    if (si.info.sort.has_value()) {
        sort = std::make_shared<const Sort>(si.info.sort.value());
    }
    if (auto meta = LeafMetaData::create(created_version_major, si.info.min_version, std::move(sort),
                                        si.info.has_blocks);
        meta.has_value()) {
        reader->leaf_meta_data_ = std::move(meta.value());
    }
    reader->num_docs_ = si.doc_count();

    // Live docs are stored with the regular directory, never inside the
    // compound file.
    if (si.has_deletions()) {
        const Codec *codec = CodecFormats::for_name(si.info.codec_name);
        if (codec == nullptr) {
            return err(LuceneError::illegal_argument("unknown codec: " + si.info.codec_name));
        }
        auto live = codec->live_docs_format().read_live_docs(dir, si, context);
        if (!live.has_value()) {
            return std::unexpected(std::move(live.error()));
        }
        auto num_bits = live.value().length();
        if (num_bits != static_cast<size_t>(si.info.max_doc)) {
            return err(LuceneError::corrupt_index("live docs of segment " + si.info.name + " has " +
                                                  std::to_string(num_bits) + " bits but maxDoc is " +
                                                  std::to_string(si.info.max_doc)));
        }
        reader->live_docs_ = std::move(live.value());
    }

    return reader;
}

StoredFieldsReader *SegmentReader::get_fields_reader() { return core_->stored_fields_reader(); }

TermVectorsReader *SegmentReader::get_term_vectors_reader() { return core_->term_vectors_reader(); }

NormsProducer *SegmentReader::get_norms_reader() { return core_->norms_producer(); }

DocValuesProducer *SegmentReader::get_doc_values_reader() { return core_->doc_values_producer(); }

FieldsProducer *SegmentReader::get_postings_reader() { return core_->fields(); }

PointsReader *SegmentReader::get_points_reader() { return core_->points_reader(); }

std::string SegmentReader::to_string() const {
    return "SegmentReader(" + segment_info_.info.name + " max_doc=" + std::to_string(segment_info_.info.max_doc) +
           " num_docs=" + std::to_string(num_docs_) + ")";
}

} // namespace lucene
