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

export module lucene_core:slow_codec_reader_wrapper;

import :types;
import std;

import :binary_doc_values;
import :bits;
import :codec_reader;
import :doc_values_producer;
import :doc_values_skipper;
import :error;
import :field_infos;
import :fields;
import :fields_producer;
import :index_reader;
import :leaf_reader;
import :norms_producer;
import :numeric_doc_values;
import :point_values;
import :points_reader;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :stored_field_visitor;
import :stored_fields;
import :stored_fields_reader;
import :term_vectors;
import :term_vectors_reader;
import :terms;

namespace lucene {

/// A [`StoredFieldsReader`] that delegates to a [`LeafReader`]'s cursor.
class SlowStoredFieldsReader final : public StoredFieldsReader {
public:
    SlowStoredFieldsReader(std::shared_ptr<LeafReader> reader, std::unique_ptr<StoredFields> cursor);

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) override;
    [[nodiscard]] Result<Document> document(int32_t doc_id) override;

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> get_merge_instance() const override;
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> clone() const override;
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::shared_ptr<LeafReader> reader_;
    std::unique_ptr<StoredFields> cursor_;
};

/// A [`TermVectorsReader`] that delegates to a [`LeafReader`]'s cursor.
class SlowTermVectorsReader final : public TermVectorsReader {
public:
    SlowTermVectorsReader(std::shared_ptr<LeafReader> reader, std::unique_ptr<TermVectors> cursor);

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override;
    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t doc) override;
    [[nodiscard]] Result<std::unique_ptr<Terms>> get_field_terms(int32_t doc, std::string_view field) override;

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> get_merge_instance() const override;
    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> clone() const override;
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::shared_ptr<LeafReader> reader_;
    std::unique_ptr<TermVectors> cursor_;
};

/// A [`NormsProducer`] that reads the norms through a [`LeafReader`].
class SlowNormsProducer final : public NormsProducer {
public:
    explicit SlowNormsProducer(std::shared_ptr<LeafReader> reader) : reader_(std::move(reader)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::shared_ptr<LeafReader> reader_;
};

/// A [`DocValuesProducer`] that reads the doc values through a [`LeafReader`].
class SlowDocValuesProducer final : public DocValuesProducer {
public:
    explicit SlowDocValuesProducer(std::shared_ptr<LeafReader> reader) : reader_(std::move(reader)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override;
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_skipper(const FieldInfo &field) const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::shared_ptr<LeafReader> reader_;
};

/// A [`PointsReader`] that reads the points through a [`LeafReader`].
class SlowPointsReader final : public PointsReader {
public:
    explicit SlowPointsReader(std::shared_ptr<LeafReader> reader) : reader_(std::move(reader)) {}

    [[nodiscard]] VoidResult check_integrity() override { return {}; }

    /// **Deviation:** a field without points yields a null pointer instead of an
    /// error, because that is what [`LeafReader::point_values`] reports.
    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override;

    [[nodiscard]] VoidResult close() override { return {}; }

private:
    std::shared_ptr<LeafReader> reader_;
};

/// A [`FieldsProducer`] that exposes the indexed fields of a [`LeafReader`].
class SlowFieldsProducer final : public FieldsProducer {
public:
    SlowFieldsProducer(std::shared_ptr<LeafReader> reader, std::vector<std::string> indexed_fields);

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override;
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override;
    [[nodiscard]] Result<int32_t> size() const override;
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::shared_ptr<LeafReader> reader_;
    /// Sorted, so the iteration order matches the codec readers.
    std::vector<std::string> indexed_fields_;
};

/// A [`CodecReader`] view of an arbitrary [`LeafReader`].
///
/// Port of `SlowCodecReaderWrapper` (`index/slow_codec_reader_wrapper.rs`).
///
/// The merge path wants a [`CodecReader`] because it reads through the codec
/// readers.  When the source is not one (a [`MultiReader`], a filter, ...), this
/// wrapper fakes the codec readers by delegating every call back to the leaf.
/// That works for any reader, but it is slow and memory hungry: prefer a real
/// codec reader (or a `FilterCodecReader`) whenever one is available.
///
/// The adapters are built eagerly, because this port's [`CodecReader`] hands out
/// borrowed pointers to readers that the wrapper must own.
export class SlowCodecReaderWrapper final : public CodecReader {
public:
    /// Wraps `reader`, keeping it alive.
    [[nodiscard]] static Result<std::shared_ptr<SlowCodecReaderWrapper>> wrap(std::shared_ptr<LeafReader> reader);

    // ---- CodecReader ----

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override;
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override;
    [[nodiscard]] NormsProducer *get_norms_reader() override;
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override;
    [[nodiscard]] FieldsProducer *get_postings_reader() override;
    [[nodiscard]] PointsReader *get_points_reader() override;

    // ---- IndexReader / LeafReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return reader_->max_doc(); }
    [[nodiscard]] Result<int32_t> num_docs() const override { return reader_->num_docs(); }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return reader_->field_infos(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return reader_->live_docs(); }
    [[nodiscard]] VoidResult close() const override { return {}; }

    [[nodiscard]] std::string to_string() const override;

    /// The wrapped reader.
    [[nodiscard]] LeafReader &delegate() const noexcept { return *reader_; }

private:
    SlowCodecReaderWrapper() = default;

    std::shared_ptr<LeafReader> reader_;
    std::unique_ptr<SlowStoredFieldsReader> stored_fields_reader_;
    std::unique_ptr<SlowTermVectorsReader> term_vectors_reader_;
    std::unique_ptr<SlowNormsProducer> norms_producer_;
    std::unique_ptr<SlowDocValuesProducer> doc_values_producer_;
    std::unique_ptr<SlowPointsReader> points_reader_;
    std::unique_ptr<SlowFieldsProducer> fields_producer_;
};

} // namespace lucene
