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

module lucene_core:slow_codec_reader_wrapper.impl;

import :slow_codec_reader_wrapper;

import std;

import :binary_doc_values;
import :codec_reader;
import :doc_values_producer;
import :doc_values_skipper;
import :error;
import :field_infos;
import :fields;
import :fields_producer;
import :index_types;
import :leaf_reader;
import :norms_producer;
import :numeric_doc_values;
import :point_values;
import :points_reader;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :stored_fields;
import :stored_fields_reader;
import :term_vectors;
import :term_vectors_reader;
import :terms;
import :types;

namespace lucene {

namespace {

/// Iterates a plain list of field names.
class NameListIterator final : public FieldNameIterator {
public:
    explicit NameListIterator(std::vector<std::string> names) : names_(std::move(names)) {}

    [[nodiscard]] Result<const std::string *> next() override {
        if (position_ >= names_.size()) {
            return static_cast<const std::string *>(nullptr);
        }
        return &names_[position_++];
    }

private:
    std::vector<std::string> names_;
    size_t position_{0};
};

/// Shared by the doc-values delegate methods: the field must have values.
template <class T>
Result<T> required(Result<T> values, std::string_view what, std::string_view field) {
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return err(LuceneError::illegal_state(std::string(what) + " doc value is missing for field " +
                                              std::string(field)));
    }
    return std::move(values.value());
}

} // namespace

// ---------------- SlowStoredFieldsReader ----------------

SlowStoredFieldsReader::SlowStoredFieldsReader(std::shared_ptr<LeafReader> reader, std::unique_ptr<StoredFields> cursor)
    : reader_(std::move(reader)), cursor_(std::move(cursor)) {}

VoidResult SlowStoredFieldsReader::prefetch(int32_t doc_id) { return cursor_->prefetch(doc_id); }

VoidResult SlowStoredFieldsReader::document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) {
    return cursor_->document_with_visitor(doc_id, visitor);
}

Result<Document> SlowStoredFieldsReader::document(int32_t doc_id) { return cursor_->document(doc_id); }

Result<std::unique_ptr<StoredFieldsReader>> SlowStoredFieldsReader::get_merge_instance() const { return clone(); }

Result<std::unique_ptr<StoredFieldsReader>> SlowStoredFieldsReader::clone() const {
    auto cursor = reader_->stored_fields();
    if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
    }
    if (cursor.value() == nullptr) {
        return err(LuceneError::illegal_state("the wrapped reader has no stored fields"));
    }
    return std::unique_ptr<StoredFieldsReader>(
            new SlowStoredFieldsReader(reader_, std::move(cursor.value())));
}

// ---------------- SlowTermVectorsReader ----------------

SlowTermVectorsReader::SlowTermVectorsReader(std::shared_ptr<LeafReader> reader, std::unique_ptr<TermVectors> cursor)
    : reader_(std::move(reader)), cursor_(std::move(cursor)) {}

VoidResult SlowTermVectorsReader::prefetch(int32_t doc_id) { return cursor_->prefetch(doc_id); }

Result<std::unique_ptr<Fields>> SlowTermVectorsReader::get(int32_t doc) { return cursor_->get(doc); }

Result<std::unique_ptr<Terms>> SlowTermVectorsReader::get_field_terms(int32_t doc, std::string_view field) {
    return cursor_->get_field_terms(doc, field);
}

Result<std::unique_ptr<TermVectorsReader>> SlowTermVectorsReader::get_merge_instance() const { return clone(); }

Result<std::unique_ptr<TermVectorsReader>> SlowTermVectorsReader::clone() const {
    auto cursor = reader_->term_vectors();
    if (!cursor.has_value()) {
        return std::unexpected(std::move(cursor.error()));
    }
    if (cursor.value() == nullptr) {
        return err(LuceneError::illegal_state("the wrapped reader has no term vectors"));
    }
    return std::unique_ptr<TermVectorsReader>(new SlowTermVectorsReader(reader_, std::move(cursor.value())));
}

// ---------------- SlowNormsProducer ----------------

Result<std::unique_ptr<NumericDocValues>> SlowNormsProducer::get_norms(const FieldInfo &field) const {
    auto values = reader_->get_norm_values(field.name);
    return required(std::move(values), "norm", field.name);
}

// ---------------- SlowDocValuesProducer ----------------

Result<std::unique_ptr<NumericDocValues>> SlowDocValuesProducer::get_numeric(const FieldInfo &field) const {
    auto values = reader_->get_numeric_doc_values(field.name);
    return required(std::move(values), "numeric", field.name);
}

Result<std::unique_ptr<BinaryDocValues>> SlowDocValuesProducer::get_binary(const FieldInfo &field) const {
    auto values = reader_->get_binary_doc_values(field.name);
    return required(std::move(values), "binary", field.name);
}

Result<std::unique_ptr<SortedDocValues>> SlowDocValuesProducer::get_sorted(const FieldInfo &field) const {
    auto values = reader_->get_sorted_doc_values(field.name);
    return required(std::move(values), "sorted", field.name);
}

Result<std::unique_ptr<SortedNumericDocValues>>
SlowDocValuesProducer::get_sorted_numeric(const FieldInfo &field) const {
    auto values = reader_->get_sorted_numeric_doc_values(field.name);
    return required(std::move(values), "sorted numeric", field.name);
}

Result<std::unique_ptr<SortedSetDocValues>> SlowDocValuesProducer::get_sorted_set(const FieldInfo &field) const {
    auto values = reader_->get_sorted_set_doc_values(field.name);
    return required(std::move(values), "sorted set", field.name);
}

Result<std::unique_ptr<DocValuesSkipper>> SlowDocValuesProducer::get_skipper(const FieldInfo &field) const {
    return reader_->get_doc_values_skipper(field.name);
}

// ---------------- SlowPointsReader ----------------

Result<std::shared_ptr<PointValues>> SlowPointsReader::get_values(std::string_view field_name) const {
    return reader_->point_values(field_name);
}

// ---------------- SlowFieldsProducer ----------------

SlowFieldsProducer::SlowFieldsProducer(std::shared_ptr<LeafReader> reader, std::vector<std::string> indexed_fields)
    : reader_(std::move(reader)), indexed_fields_(std::move(indexed_fields)) {}

Result<std::unique_ptr<FieldNameIterator>> SlowFieldsProducer::iterator() const {
    return std::unique_ptr<FieldNameIterator>(new NameListIterator(indexed_fields_));
}

Result<std::unique_ptr<Terms>> SlowFieldsProducer::terms(std::string_view field) const {
    return reader_->terms(field);
}

Result<int32_t> SlowFieldsProducer::size() const { return static_cast<int32_t>(indexed_fields_.size()); }

// ---------------- SlowCodecReaderWrapper ----------------

Result<std::shared_ptr<SlowCodecReaderWrapper>>
SlowCodecReaderWrapper::wrap(std::shared_ptr<LeafReader> reader) {
    if (reader == nullptr) {
        return err(LuceneError::illegal_argument("cannot wrap a null reader"));
    }
    std::shared_ptr<SlowCodecReaderWrapper> wrapper(new SlowCodecReaderWrapper());
    wrapper->reader_ = std::move(reader);

    auto stored = wrapper->reader_->stored_fields();
    if (!stored.has_value()) {
        return std::unexpected(std::move(stored.error()));
    }
    if (stored.value() != nullptr) {
        wrapper->stored_fields_reader_ = std::make_unique<SlowStoredFieldsReader>(
                wrapper->reader_, std::move(stored.value()));
    }

    auto vectors = wrapper->reader_->term_vectors();
    if (!vectors.has_value()) {
        return std::unexpected(std::move(vectors.error()));
    }
    if (vectors.value() != nullptr) {
        wrapper->term_vectors_reader_ = std::make_unique<SlowTermVectorsReader>(
                wrapper->reader_, std::move(vectors.value()));
    }

    wrapper->norms_producer_ = std::make_unique<SlowNormsProducer>(wrapper->reader_);
    wrapper->doc_values_producer_ = std::make_unique<SlowDocValuesProducer>(wrapper->reader_);
    wrapper->points_reader_ = std::make_unique<SlowPointsReader>(wrapper->reader_);

    auto infos = wrapper->reader_->field_infos();
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    std::vector<std::string> indexed_fields;
    for (const FieldInfo &info : infos.value()->fields()) {
        if (info.index_options != IndexOptions::None) {
            indexed_fields.push_back(info.name);
        }
    }
    std::ranges::sort(indexed_fields);
    wrapper->fields_producer_ =
            std::make_unique<SlowFieldsProducer>(wrapper->reader_, std::move(indexed_fields));
    return wrapper;
}

StoredFieldsReader *SlowCodecReaderWrapper::get_fields_reader() { return stored_fields_reader_.get(); }

TermVectorsReader *SlowCodecReaderWrapper::get_term_vectors_reader() { return term_vectors_reader_.get(); }

NormsProducer *SlowCodecReaderWrapper::get_norms_reader() { return norms_producer_.get(); }

DocValuesProducer *SlowCodecReaderWrapper::get_doc_values_reader() { return doc_values_producer_.get(); }

FieldsProducer *SlowCodecReaderWrapper::get_postings_reader() { return fields_producer_.get(); }

PointsReader *SlowCodecReaderWrapper::get_points_reader() { return points_reader_.get(); }

std::string SlowCodecReaderWrapper::to_string() const {
    return "SlowCodecReaderWrapper(" + reader_->to_string() + ")";
}

} // namespace lucene
