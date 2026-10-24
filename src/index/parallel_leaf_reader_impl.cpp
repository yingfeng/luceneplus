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

module lucene_core:parallel_leaf_reader.impl;

import :parallel_leaf_reader;

import std;

import :binary_doc_values;
import :bits;
import :document;
import :doc_values_skipper;
import :error;
import :field_infos;
import :field_infos_builder;
import :fields;
import :index_reader;
import :leaf_reader;
import :numeric_doc_values;
import :point_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :sort_field;
import :stored_fields;
import :term_vectors;
import :terms;
import :types;
import :version;

namespace lucene {

namespace {

/// Iterates a plain list of names.
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

} // namespace

// ---------------- ParallelFields ----------------

ParallelFields::ParallelFields(std::vector<TermVectors *> cursors,
                               std::map<std::string, size_t, std::less<>> field_to_cursor, int32_t doc)
    : cursors_(std::move(cursors)), field_to_cursor_(std::move(field_to_cursor)), doc_(doc) {}

Result<std::unique_ptr<FieldNameIterator>> ParallelFields::iterator() const {
    std::vector<std::string> names;
    names.reserve(field_to_cursor_.size());
    for (const auto &[name, cursor] : field_to_cursor_) {
        names.push_back(name);
    }
    return std::unique_ptr<FieldNameIterator>(new NameListIterator(std::move(names)));
}

Result<std::unique_ptr<Terms>> ParallelFields::terms(std::string_view field) const {
    const auto found = field_to_cursor_.find(field);
    if (found == field_to_cursor_.end()) {
        return std::unique_ptr<Terms>{};
    }
    TermVectors *cursor = cursors_[found->second];
    if (cursor == nullptr) {
        return std::unique_ptr<Terms>{};
    }
    return cursor->get_field_terms(doc_, field);
}

Result<int32_t> ParallelFields::size() const { return static_cast<int32_t>(field_to_cursor_.size()); }

// ---------------- ParallelStoredFields ----------------

ParallelStoredFields::ParallelStoredFields(std::vector<std::unique_ptr<StoredFields>> cursors)
    : cursors_(std::move(cursors)) {}

VoidResult ParallelStoredFields::prefetch(int32_t doc_id) {
    for (const std::unique_ptr<StoredFields> &cursor : cursors_) {
        if (auto prefetched = cursor->prefetch(doc_id); !prefetched.has_value()) {
            return prefetched;
        }
    }
    return {};
}

VoidResult ParallelStoredFields::document_with_visitor(int32_t doc_id, StoredFieldVisitor &visitor) {
    for (const std::unique_ptr<StoredFields> &cursor : cursors_) {
        if (auto loaded = cursor->document_with_visitor(doc_id, visitor); !loaded.has_value()) {
            return loaded;
        }
    }
    return {};
}

Result<Document> ParallelStoredFields::document(int32_t doc_id) {
    Document merged;
    for (const std::unique_ptr<StoredFields> &cursor : cursors_) {
        auto document = cursor->document(doc_id);
        if (!document.has_value()) {
            return std::unexpected(std::move(document.error()));
        }
        for (std::unique_ptr<IndexableField> &field : document.value().take_fields()) {
            merged.add(std::move(field));
        }
    }
    return merged;
}

// ---------------- ParallelTermVectors ----------------

ParallelTermVectors::ParallelTermVectors(std::vector<std::unique_ptr<TermVectors>> cursors,
                                         std::map<std::string, size_t, std::less<>> field_to_cursor)
    : cursors_(std::move(cursors)), field_to_cursor_(std::move(field_to_cursor)) {}

VoidResult ParallelTermVectors::prefetch(int32_t doc_id) {
    for (const std::unique_ptr<TermVectors> &cursor : cursors_) {
        if (cursor == nullptr) {
            continue;
        }
        if (auto prefetched = cursor->prefetch(doc_id); !prefetched.has_value()) {
            return prefetched;
        }
    }
    return {};
}

Result<std::unique_ptr<Fields>> ParallelTermVectors::get(int32_t doc) {
    std::map<std::string, size_t, std::less<>> present;
    for (const auto &[name, cursor] : field_to_cursor_) {
        if (cursors_[cursor] == nullptr) {
            continue;
        }
        auto terms = cursors_[cursor]->get_field_terms(doc, name);
        if (!terms.has_value()) {
            return std::unexpected(std::move(terms.error()));
        }
        if (terms.value() != nullptr) {
            present.emplace(name, cursor);
        }
    }
    if (present.empty()) {
        return std::unique_ptr<Fields>{};
    }
    std::vector<TermVectors *> pointers;
    pointers.reserve(cursors_.size());
    for (const std::unique_ptr<TermVectors> &cursor : cursors_) {
        pointers.push_back(cursor.get());
    }
    return std::unique_ptr<Fields>(new ParallelFields(std::move(pointers), std::move(present), doc));
}

Result<std::unique_ptr<Terms>> ParallelTermVectors::get_field_terms(int32_t doc, std::string_view field) {
    const auto found = field_to_cursor_.find(field);
    if (found == field_to_cursor_.end() || cursors_[found->second] == nullptr) {
        return std::unique_ptr<Terms>{};
    }
    return cursors_[found->second]->get_field_terms(doc, field);
}

// ---------------- ParallelLeafReader ----------------

Result<std::shared_ptr<ParallelLeafReader>>
ParallelLeafReader::create(std::vector<std::shared_ptr<LeafReader>> readers) {
    return create_with_close_sub_readers(true, std::move(readers));
}

Result<std::shared_ptr<ParallelLeafReader>>
ParallelLeafReader::create_with_close_sub_readers(bool close_sub_readers,
                                                  std::vector<std::shared_ptr<LeafReader>> readers) {
    return create_with_stored_fields(close_sub_readers, std::move(readers), {});
}

Result<std::shared_ptr<ParallelLeafReader>>
ParallelLeafReader::create_with_stored_fields(bool close_sub_readers,
                                              std::vector<std::shared_ptr<LeafReader>> readers,
                                              std::vector<std::shared_ptr<LeafReader>> stored_fields_readers) {
    if (readers.empty() && !stored_fields_readers.empty()) {
        return err(LuceneError::illegal_argument(
                "There must be at least one main reader if storedFieldsReaders are used."));
    }
    for (const std::shared_ptr<LeafReader> &reader : readers) {
        if (reader == nullptr) {
            return err(LuceneError::illegal_argument("a sub-reader must not be null"));
        }
    }
    for (const std::shared_ptr<LeafReader> &reader : stored_fields_readers) {
        if (reader == nullptr) {
            return err(LuceneError::illegal_argument("a stored-fields reader must not be null"));
        }
    }

    std::shared_ptr<ParallelLeafReader> combined(new ParallelLeafReader());
    combined->close_sub_readers_ = close_sub_readers;
    if (!readers.empty()) {
        auto max_doc = readers[0]->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        auto num_docs = readers[0]->num_docs();
        if (!num_docs.has_value()) {
            return std::unexpected(std::move(num_docs.error()));
        }
        auto has_deletions = readers[0]->has_deletions();
        if (!has_deletions.has_value()) {
            return std::unexpected(std::move(has_deletions.error()));
        }
        combined->max_doc_ = max_doc.value();
        combined->num_docs_ = num_docs.value();
        combined->has_deletions_ = has_deletions.value();
    }

    combined->readers_ = std::move(readers);
    combined->stored_fields_readers_ =
            stored_fields_readers.empty() ? combined->readers_ : std::move(stored_fields_readers);

    // `close` must touch each distinct reader once, even when the same reader
    // serves several positions.
    for (const std::shared_ptr<LeafReader> &reader : combined->readers_) {
        const bool known = std::ranges::any_of(
                combined->all_readers_, [&reader](const std::shared_ptr<LeafReader> &existing) {
                    return existing.get() == reader.get();
                });
        if (!known) {
            combined->all_readers_.push_back(reader);
        }
    }
    for (const std::shared_ptr<LeafReader> &reader : combined->stored_fields_readers_) {
        const bool known = std::ranges::any_of(
                combined->all_readers_, [&reader](const std::shared_ptr<LeafReader> &existing) {
                    return existing.get() == reader.get();
                });
        if (!known) {
            combined->all_readers_.push_back(reader);
        }
    }

    // All readers must agree on the document count.
    for (const std::shared_ptr<LeafReader> &reader : combined->all_readers_) {
        auto max_doc = reader->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        if (max_doc.value() != combined->max_doc_) {
            return err(LuceneError::illegal_argument("All readers must have same maxDoc: " +
                                                     std::to_string(combined->max_doc_) + "!=" +
                                                     std::to_string(max_doc.value())));
        }
    }

    // Merge the sub-readers' metadata; the reference implementation rejects mixed
    // creation versions and mixed index sorts here, before anything else happens.
    std::optional<int32_t> created_version_major;
    std::shared_ptr<const Sort> sort;
    std::optional<Version> min_version = LUCENE_CURRENT_VERSION;
    bool has_blocks = false;
    for (const std::shared_ptr<LeafReader> &reader : combined->readers_) {
        const LeafMetaData &metadata = reader->leaf_meta_data();
        if (!created_version_major.has_value()) {
            created_version_major = metadata.created_version_major();
        } else if (created_version_major.value() != metadata.created_version_major()) {
            return err(LuceneError::illegal_argument(
                    "cannot combine LeafReaders that have different creation versions: saw both version=" +
                    std::to_string(created_version_major.value()) + " and " +
                    std::to_string(metadata.created_version_major())));
        }
        if (sort == nullptr) {
            sort = metadata.sort();
        } else if (metadata.sort() != nullptr && !sort->equals(*metadata.sort())) {
            return err(LuceneError::illegal_argument(
                    "cannot combine LeafReaders that have different index sorts"));
        }
        if (!metadata.min_version().has_value()) {
            min_version.reset();
        } else if (min_version.has_value() && metadata.min_version().value() < min_version.value()) {
            min_version = metadata.min_version();
        }
        has_blocks = has_blocks || metadata.has_blocks();
    }
    if (!created_version_major.has_value()) {
        // No main readers at all: report what this port writes.
        created_version_major = LUCENE_CURRENT_VERSION.major;
        min_version = LUCENE_CURRENT_VERSION;
    }
    if (auto merged = LeafMetaData::create(created_version_major.value(), min_version, sort, has_blocks);
        merged.has_value()) {
        combined->leaf_meta_data_ = std::move(merged.value());
    }

    // Merge the field infos, first reader wins for a shared field name; the same
    // pass builds the field-to-reader maps.
    FieldInfosBuilder builder;
    for (size_t index = 0; index < combined->readers_.size(); ++index) {
        auto infos = combined->readers_[index]->field_infos();
        if (!infos.has_value()) {
            return std::unexpected(std::move(infos.error()));
        }
        for (const FieldInfo &info : infos.value()->fields()) {
            if (combined->field_to_reader_.find(info.name) != combined->field_to_reader_.end()) {
                continue;
            }
            auto added = builder.add_existing(info);
            if (!added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
            combined->field_to_reader_.emplace(info.name, index);
            if (info.has_term_vectors()) {
                combined->tv_field_to_reader_.emplace(info.name, index);
            }
            if (info.index_options != IndexOptions::None) {
                combined->terms_field_to_reader_.emplace(info.name, index);
            }
        }
    }
    auto merged = builder.finish();
    if (!merged.has_value()) {
        return std::unexpected(std::move(merged.error()));
    }
    combined->field_infos_ = std::move(merged.value());
    return combined;
}

Result<std::shared_ptr<LeafReader>> ParallelLeafReader::reader_for_field(std::string_view field) const {
    const auto found = field_to_reader_.find(field);
    if (found == field_to_reader_.end()) {
        return std::shared_ptr<LeafReader>{};
    }
    return readers_[found->second];
}

Result<std::shared_ptr<LeafReader>> ParallelLeafReader::reader_for_terms(std::string_view field) const {
    const auto found = terms_field_to_reader_.find(field);
    if (found == terms_field_to_reader_.end()) {
        return std::shared_ptr<LeafReader>{};
    }
    return readers_[found->second];
}

Result<std::unique_ptr<Terms>> ParallelLeafReader::terms(std::string_view field) {
    auto reader = reader_for_terms(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::unique_ptr<Terms>{};
    }
    return reader.value()->terms(field);
}

Result<std::unique_ptr<NumericDocValues>> ParallelLeafReader::get_numeric_doc_values(std::string_view field) {
    auto reader = reader_for_field(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::unique_ptr<NumericDocValues>{};
    }
    return reader.value()->get_numeric_doc_values(field);
}

Result<std::unique_ptr<BinaryDocValues>> ParallelLeafReader::get_binary_doc_values(std::string_view field) {
    auto reader = reader_for_field(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::unique_ptr<BinaryDocValues>{};
    }
    return reader.value()->get_binary_doc_values(field);
}

Result<std::unique_ptr<SortedDocValues>> ParallelLeafReader::get_sorted_doc_values(std::string_view field) {
    auto reader = reader_for_field(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::unique_ptr<SortedDocValues>{};
    }
    return reader.value()->get_sorted_doc_values(field);
}

Result<std::unique_ptr<SortedNumericDocValues>>
ParallelLeafReader::get_sorted_numeric_doc_values(std::string_view field) {
    auto reader = reader_for_field(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    return reader.value()->get_sorted_numeric_doc_values(field);
}

Result<std::unique_ptr<SortedSetDocValues>> ParallelLeafReader::get_sorted_set_doc_values(std::string_view field) {
    auto reader = reader_for_field(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    return reader.value()->get_sorted_set_doc_values(field);
}

Result<std::unique_ptr<NumericDocValues>> ParallelLeafReader::get_norm_values(std::string_view field) {
    auto reader = reader_for_field(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::unique_ptr<NumericDocValues>{};
    }
    return reader.value()->get_norm_values(field);
}

Result<std::unique_ptr<DocValuesSkipper>> ParallelLeafReader::get_doc_values_skipper(std::string_view field) {
    auto reader = reader_for_field(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::unique_ptr<DocValuesSkipper>{};
    }
    return reader.value()->get_doc_values_skipper(field);
}

Result<std::shared_ptr<PointValues>> ParallelLeafReader::point_values(std::string_view field) {
    auto reader = reader_for_field(field);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    if (reader.value() == nullptr) {
        return std::shared_ptr<PointValues>{};
    }
    return reader.value()->point_values(field);
}

Result<std::unique_ptr<StoredFields>> ParallelLeafReader::stored_fields() {
    std::vector<std::unique_ptr<StoredFields>> cursors;
    cursors.reserve(stored_fields_readers_.size());
    for (const std::shared_ptr<LeafReader> &reader : stored_fields_readers_) {
        auto cursor = reader->stored_fields();
        if (!cursor.has_value()) {
            return std::unexpected(std::move(cursor.error()));
        }
        if (cursor.value() != nullptr) {
            cursors.push_back(std::move(cursor.value()));
        }
    }
    return std::unique_ptr<StoredFields>(new ParallelStoredFields(std::move(cursors)));
}

Result<std::unique_ptr<TermVectors>> ParallelLeafReader::term_vectors() {
    std::vector<std::unique_ptr<TermVectors>> cursors;
    cursors.reserve(readers_.size());
    for (const std::shared_ptr<LeafReader> &reader : readers_) {
        auto infos = reader->field_infos();
        if (!infos.has_value()) {
            return std::unexpected(std::move(infos.error()));
        }
        if (!infos.value()->has_term_vectors()) {
            cursors.emplace_back();
            continue;
        }
        auto cursor = reader->term_vectors();
        if (!cursor.has_value()) {
            return std::unexpected(std::move(cursor.error()));
        }
        cursors.push_back(std::move(cursor.value()));
    }
    return std::unique_ptr<TermVectors>(new ParallelTermVectors(std::move(cursors), tv_field_to_reader_));
}

Result<const Bits *> ParallelLeafReader::live_docs() const {
    if (!has_deletions_ || readers_.empty()) {
        return nullptr;
    }
    // Deletions are taken from the first reader.
    return readers_[0]->live_docs();
}

VoidResult ParallelLeafReader::close() const {
    if (closed_) {
        return {};
    }
    closed_ = true;
    if (!close_sub_readers_) {
        return {};
    }
    std::optional<LuceneError> first_io_error;
    for (const std::shared_ptr<LeafReader> &reader : all_readers_) {
        auto closed_reader = reader->close();
        if (closed_reader.has_value()) {
            continue;
        }
        if (!closed_reader.error().is_io_error()) {
            return std::unexpected(std::move(closed_reader.error()));
        }
        if (!first_io_error.has_value()) {
            first_io_error = closed_reader.error();
        }
    }
    if (first_io_error.has_value()) {
        return std::unexpected(std::move(first_io_error.value()));
    }
    return {};
}

std::string ParallelLeafReader::to_string() const {
    std::string result = "ParallelLeafReader(";
    for (size_t i = 0; i < all_readers_.size(); ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += all_readers_[i]->to_string();
    }
    result += ")";
    return result;
}

} // namespace lucene
