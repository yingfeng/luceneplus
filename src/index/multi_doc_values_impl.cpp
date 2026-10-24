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

module lucene_core:multi_doc_values.impl;

import :multi_doc_values;

import std;

import :binary_doc_values;
import :bytes_ref;
import :doc_values;
import :doc_values_iterator;
import :error;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :numeric_doc_values;
import :reader_util;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :types;

namespace lucene {

// ---------------- GlobalOrdinalMap ----------------

Result<GlobalOrdinalMap> GlobalOrdinalMap::build(std::vector<std::vector<BytesRef>> dictionaries) {
    const size_t segment_count = dictionaries.size();
    for (const std::vector<BytesRef> &dictionary : dictionaries) {
        for (size_t i = 1; i < dictionary.size(); ++i) {
            if (dictionary[i - 1].compare_to(dictionary[i]) > 0) {
                return err(LuceneError::illegal_argument("a segment dictionary must be sorted"));
            }
        }
    }

    GlobalOrdinalMap map;
    map.segment_ords_.resize(segment_count);
    for (size_t s = 0; s < segment_count; ++s) {
        map.segment_ords_[s].assign(dictionaries[s].size(), -1);
    }

    std::vector<size_t> positions(segment_count, 0);
    while (true) {
        // Pick the smallest term still pending in any segment.
        size_t smallest_segment = segment_count;
        for (size_t s = 0; s < segment_count; ++s) {
            if (positions[s] == dictionaries[s].size()) {
                continue;
            }
            if (smallest_segment == segment_count ||
                dictionaries[s][positions[s]].compare_to(dictionaries[smallest_segment][positions[smallest_segment]]) <
                        0) {
                smallest_segment = s;
            }
        }
        if (smallest_segment == segment_count) {
            break;
        }
        const BytesRef &smallest = dictionaries[smallest_segment][positions[smallest_segment]];

        // Every segment that has this term shares the new global ordinal; the
        // lowest segment index is the one `lookup_ord` resolves through.
        const auto global_ord = static_cast<int32_t>(map.terms_.size());
        int32_t first_segment = -1;
        int32_t first_ord = -1;
        for (size_t s = 0; s < segment_count; ++s) {
            if (positions[s] == dictionaries[s].size()) {
                continue;
            }
            if (dictionaries[s][positions[s]].compare_to(smallest) != 0) {
                continue;
            }
            map.segment_ords_[s][positions[s]] = global_ord;
            if (first_segment < 0) {
                first_segment = static_cast<int32_t>(s);
                first_ord = static_cast<int32_t>(positions[s]);
            }
            ++positions[s];
        }
        map.terms_.push_back(smallest);
        map.first_segment_.push_back(first_segment);
        map.first_segment_ord_.push_back(first_ord);
    }
    return map;
}

const BytesRef *GlobalOrdinalMap::term(int64_t global_ord) const {
    if (global_ord < 0 || global_ord >= value_count()) {
        return nullptr;
    }
    return &terms_[static_cast<size_t>(global_ord)];
}

int64_t GlobalOrdinalMap::global_ord(size_t segment, int64_t segment_ord) const {
    if (segment >= segment_ords_.size() || segment_ord < 0 ||
        segment_ord >= static_cast<int64_t>(segment_ords_[segment].size())) {
        return -1;
    }
    return segment_ords_[segment][static_cast<size_t>(segment_ord)];
}

Result<std::pair<size_t, int64_t>> GlobalOrdinalMap::first_segment_of(int64_t global_ord) const {
    if (global_ord < 0 || global_ord >= value_count()) {
        return err(LuceneError::illegal_argument(std::format("ordinal {} is out of range", global_ord)));
    }
    const size_t ord = static_cast<size_t>(global_ord);
    return std::pair<size_t, int64_t>{static_cast<size_t>(first_segment_[ord]),
                                      static_cast<int64_t>(first_segment_ord_[ord])};
}

// ---------------- MultiNumericDocValues ----------------

MultiNumericDocValues::MultiNumericDocValues(std::span<const LeafReaderContext> leaves, std::string field,
                                             NumericSource source)
    : leaves_(leaves), field_(std::move(field)), source_(source) {}

Result<std::unique_ptr<NumericDocValues>> MultiNumericDocValues::fetch(size_t leaf) const {
    LeafReader *reader = leaves_[leaf].reader();
    if (source_ == NumericSource::kNorm) {
        return reader->get_norm_values(field_);
    }
    return reader->get_numeric_doc_values(field_);
}

Result<int32_t> MultiNumericDocValues::next_doc() {
    while (true) {
        if (current_ == nullptr) {
            if (next_leaf_ == leaves_.size()) {
                doc_id_ = NO_MORE_DOCS;
                return doc_id_;
            }
            const LeafReaderContext &leaf = leaves_[next_leaf_];
            current_doc_base_ = leaf.doc_base();
            auto fetched = fetch(next_leaf_);
            if (!fetched.has_value()) {
                return std::unexpected(std::move(fetched.error()));
            }
            current_ = std::move(fetched.value());
            ++next_leaf_;
            continue;
        }
        auto new_doc = current_->next_doc();
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() == NO_MORE_DOCS) {
            current_ = nullptr;
        } else {
            doc_id_ = current_doc_base_ + new_doc.value();
            return doc_id_;
        }
    }
}

Result<int32_t> MultiNumericDocValues::advance(int32_t target) {
    if (target <= doc_id_) {
        return err(LuceneError::illegal_argument(std::format(
                "can only advance beyond current document: on docID={} but targetDocID={}", doc_id_, target)));
    }
    const int32_t reader_index = ReaderUtil::sub_index_with_leaves(target, leaves_);
    if (static_cast<size_t>(reader_index) >= next_leaf_) {
        if (static_cast<size_t>(reader_index) == leaves_.size()) {
            current_ = nullptr;
            doc_id_ = NO_MORE_DOCS;
            return doc_id_;
        }
        const LeafReaderContext &leaf = leaves_[static_cast<size_t>(reader_index)];
        current_doc_base_ = leaf.doc_base();
        auto fetched = fetch(static_cast<size_t>(reader_index));
        if (!fetched.has_value()) {
            return std::unexpected(std::move(fetched.error()));
        }
        current_ = std::move(fetched.value());
        if (current_ == nullptr) {
            return next_doc();
        }
        next_leaf_ = static_cast<size_t>(reader_index) + 1;
    }
    auto new_doc = current_->advance(target - current_doc_base_);
    if (!new_doc.has_value()) {
        return std::unexpected(std::move(new_doc.error()));
    }
    if (new_doc.value() == NO_MORE_DOCS) {
        current_ = nullptr;
        return next_doc();
    }
    doc_id_ = current_doc_base_ + new_doc.value();
    return doc_id_;
}

Result<int64_t> MultiNumericDocValues::long_value() {
    if (current_ == nullptr) {
        return err(LuceneError::illegal_state("long_value() without a current document"));
    }
    return current_->long_value();
}

// ---------------- MultiBinaryDocValues ----------------

MultiBinaryDocValues::MultiBinaryDocValues(std::span<const LeafReaderContext> leaves, std::string field)
    : leaves_(leaves), field_(std::move(field)) {}

Result<std::unique_ptr<BinaryDocValues>> MultiBinaryDocValues::fetch(size_t leaf) const {
    return leaves_[leaf].reader()->get_binary_doc_values(field_);
}

Result<int32_t> MultiBinaryDocValues::next_doc() {
    while (true) {
        if (current_ == nullptr) {
            if (next_leaf_ == leaves_.size()) {
                doc_id_ = NO_MORE_DOCS;
                return doc_id_;
            }
            const LeafReaderContext &leaf = leaves_[next_leaf_];
            current_doc_base_ = leaf.doc_base();
            auto fetched = fetch(next_leaf_);
            if (!fetched.has_value()) {
                return std::unexpected(std::move(fetched.error()));
            }
            current_ = std::move(fetched.value());
            ++next_leaf_;
            continue;
        }
        auto new_doc = current_->next_doc();
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() == NO_MORE_DOCS) {
            current_ = nullptr;
        } else {
            doc_id_ = current_doc_base_ + new_doc.value();
            return doc_id_;
        }
    }
}

Result<int32_t> MultiBinaryDocValues::advance(int32_t target) {
    if (target <= doc_id_) {
        return err(LuceneError::illegal_argument(std::format(
                "can only advance beyond current document: on docID={} but targetDocID={}", doc_id_, target)));
    }
    const int32_t reader_index = ReaderUtil::sub_index_with_leaves(target, leaves_);
    if (static_cast<size_t>(reader_index) >= next_leaf_) {
        if (static_cast<size_t>(reader_index) == leaves_.size()) {
            current_ = nullptr;
            doc_id_ = NO_MORE_DOCS;
            return doc_id_;
        }
        const LeafReaderContext &leaf = leaves_[static_cast<size_t>(reader_index)];
        current_doc_base_ = leaf.doc_base();
        auto fetched = fetch(static_cast<size_t>(reader_index));
        if (!fetched.has_value()) {
            return std::unexpected(std::move(fetched.error()));
        }
        current_ = std::move(fetched.value());
        if (current_ == nullptr) {
            return next_doc();
        }
        next_leaf_ = static_cast<size_t>(reader_index) + 1;
    }
    auto new_doc = current_->advance(target - current_doc_base_);
    if (!new_doc.has_value()) {
        return std::unexpected(std::move(new_doc.error()));
    }
    if (new_doc.value() == NO_MORE_DOCS) {
        current_ = nullptr;
        return next_doc();
    }
    doc_id_ = current_doc_base_ + new_doc.value();
    return doc_id_;
}

Result<BytesRef> MultiBinaryDocValues::binary_value() {
    if (current_ == nullptr) {
        return err(LuceneError::illegal_state("binary_value() without a current document"));
    }
    return current_->binary_value();
}

// ---------------- MultiSortedNumericDocValues ----------------

MultiSortedNumericDocValues::MultiSortedNumericDocValues(std::vector<std::unique_ptr<SortedNumericDocValues>> values,
                                                         std::span<const LeafReaderContext> leaves, int32_t max_doc,
                                                         int64_t total_cost)
    : values_(std::move(values)),
      leaves_(leaves),
      max_doc_(max_doc),
      total_cost_(total_cost) {}

Result<int32_t> MultiSortedNumericDocValues::next_doc() {
    while (true) {
        if (current_index_ < 0) {
            if (next_leaf_ == values_.size()) {
                doc_id_ = NO_MORE_DOCS;
                return doc_id_;
            }
            current_doc_base_ = leaves_[next_leaf_].doc_base();
            current_index_ = static_cast<int32_t>(next_leaf_);
            ++next_leaf_;
        }
        auto new_doc = values_[static_cast<size_t>(current_index_)]->next_doc();
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() == NO_MORE_DOCS) {
            current_index_ = -1;
        } else {
            doc_id_ = current_doc_base_ + new_doc.value();
            return doc_id_;
        }
    }
}

Result<int32_t> MultiSortedNumericDocValues::advance(int32_t target) {
    if (target <= doc_id_) {
        return err(LuceneError::illegal_argument(std::format(
                "can only advance beyond current document: on docID={} but targetDocID={}", doc_id_, target)));
    }
    const int32_t reader_index = ReaderUtil::sub_index_with_leaves(target, leaves_);
    if (static_cast<size_t>(reader_index) >= next_leaf_) {
        if (static_cast<size_t>(reader_index) == values_.size()) {
            current_index_ = -1;
            doc_id_ = NO_MORE_DOCS;
            return doc_id_;
        }
        current_doc_base_ = leaves_[static_cast<size_t>(reader_index)].doc_base();
        current_index_ = reader_index;
        next_leaf_ = static_cast<size_t>(reader_index) + 1;
    }
    if (current_index_ < 0) {
        return next_doc();
    }
    auto new_doc = values_[static_cast<size_t>(current_index_)]->advance(target - current_doc_base_);
    if (!new_doc.has_value()) {
        return std::unexpected(std::move(new_doc.error()));
    }
    if (new_doc.value() == NO_MORE_DOCS) {
        current_index_ = -1;
        return next_doc();
    }
    doc_id_ = current_doc_base_ + new_doc.value();
    return doc_id_;
}

Result<bool> MultiSortedNumericDocValues::advance_exact(int32_t target) {
    if (target < doc_id_) {
        return err(LuceneError::illegal_argument(std::format(
                "can only advance beyond current document: on docID={} but targetDocID={}", doc_id_, target)));
    }
    if (target >= max_doc_) {
        return err(LuceneError::illegal_argument(std::format("Out of range: {}", target)));
    }
    const int32_t reader_index = ReaderUtil::sub_index_with_leaves(target, leaves_);
    if (static_cast<size_t>(reader_index) >= next_leaf_) {
        current_doc_base_ = leaves_[static_cast<size_t>(reader_index)].doc_base();
        current_index_ = reader_index;
        next_leaf_ = static_cast<size_t>(reader_index) + 1;
    }
    doc_id_ = target;
    if (current_index_ < 0) {
        return false;
    }
    return values_[static_cast<size_t>(current_index_)]->advance_exact(target - current_doc_base_);
}

Result<int64_t> MultiSortedNumericDocValues::next_value() {
    if (current_index_ < 0) {
        return err(LuceneError::illegal_state("next_value() without a current document"));
    }
    return values_[static_cast<size_t>(current_index_)]->next_value();
}

Result<int32_t> MultiSortedNumericDocValues::doc_value_count() {
    if (current_index_ < 0) {
        return err(LuceneError::illegal_state("doc_value_count() without a current document"));
    }
    return values_[static_cast<size_t>(current_index_)]->doc_value_count();
}

// ---------------- MultiSortedDocValues ----------------

MultiSortedDocValues::MultiSortedDocValues(std::vector<std::unique_ptr<SortedDocValues>> values,
                                           std::span<const LeafReaderContext> leaves, int32_t max_doc,
                                           GlobalOrdinalMap mapping, int64_t total_cost)
    : values_(std::move(values)),
      leaves_(leaves),
      max_doc_(max_doc),
      mapping_(std::move(mapping)),
      total_cost_(total_cost) {}

Result<int32_t> MultiSortedDocValues::next_doc() {
    while (true) {
        if (current_index_ < 0) {
            if (next_leaf_ == values_.size()) {
                doc_id_ = NO_MORE_DOCS;
                return doc_id_;
            }
            current_doc_base_ = leaves_[next_leaf_].doc_base();
            current_index_ = static_cast<int32_t>(next_leaf_);
            ++next_leaf_;
        }
        auto new_doc = values_[static_cast<size_t>(current_index_)]->next_doc();
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() == NO_MORE_DOCS) {
            current_index_ = -1;
        } else {
            doc_id_ = current_doc_base_ + new_doc.value();
            return doc_id_;
        }
    }
}

Result<int32_t> MultiSortedDocValues::advance(int32_t target) {
    if (target <= doc_id_) {
        return err(LuceneError::illegal_argument(std::format(
                "can only advance beyond current document: on docID={} but targetDocID={}", doc_id_, target)));
    }
    const int32_t reader_index = ReaderUtil::sub_index_with_leaves(target, leaves_);
    if (static_cast<size_t>(reader_index) >= next_leaf_) {
        if (static_cast<size_t>(reader_index) == values_.size()) {
            current_index_ = -1;
            doc_id_ = NO_MORE_DOCS;
            return doc_id_;
        }
        current_doc_base_ = leaves_[static_cast<size_t>(reader_index)].doc_base();
        current_index_ = reader_index;
        next_leaf_ = static_cast<size_t>(reader_index) + 1;
    }
    if (current_index_ < 0) {
        return next_doc();
    }
    auto new_doc = values_[static_cast<size_t>(current_index_)]->advance(target - current_doc_base_);
    if (!new_doc.has_value()) {
        return std::unexpected(std::move(new_doc.error()));
    }
    if (new_doc.value() == NO_MORE_DOCS) {
        current_index_ = -1;
        return next_doc();
    }
    doc_id_ = current_doc_base_ + new_doc.value();
    return doc_id_;
}

Result<bool> MultiSortedDocValues::advance_exact(int32_t target) {
    if (target < doc_id_) {
        return err(LuceneError::illegal_argument(std::format(
                "can only advance beyond current document: on docID={} but targetDocID={}", doc_id_, target)));
    }
    if (target >= max_doc_) {
        return err(LuceneError::illegal_argument(std::format("Out of range: {}", target)));
    }
    const int32_t reader_index = ReaderUtil::sub_index_with_leaves(target, leaves_);
    if (static_cast<size_t>(reader_index) >= next_leaf_) {
        current_doc_base_ = leaves_[static_cast<size_t>(reader_index)].doc_base();
        current_index_ = reader_index;
        next_leaf_ = static_cast<size_t>(reader_index) + 1;
    }
    doc_id_ = target;
    if (current_index_ < 0) {
        return false;
    }
    return values_[static_cast<size_t>(current_index_)]->advance_exact(target - current_doc_base_);
}

Result<int32_t> MultiSortedDocValues::ord_value() {
    if (current_index_ < 0) {
        return err(LuceneError::illegal_state("ord_value() without a current document"));
    }
    auto segment_ord = values_[static_cast<size_t>(current_index_)]->ord_value();
    if (!segment_ord.has_value()) {
        return std::unexpected(std::move(segment_ord.error()));
    }
    if (segment_ord.value() < 0) {
        // No value for this document; the reference implementation maps it too,
        // which would be out of range.
        return -1;
    }
    const int64_t global =
            mapping_.global_ord(static_cast<size_t>(current_index_), static_cast<int64_t>(segment_ord.value()));
    if (global < 0) {
        return err(LuceneError::illegal_state("the ordinal map has no global ordinal for this term"));
    }
    return static_cast<int32_t>(global);
}

Result<BytesRef> MultiSortedDocValues::lookup_ord(int32_t ord) {
    auto segment = mapping_.first_segment_of(static_cast<int64_t>(ord));
    if (!segment.has_value()) {
        return std::unexpected(std::move(segment.error()));
    }
    return values_[segment.value().first]->lookup_ord(static_cast<int32_t>(segment.value().second));
}

Result<int32_t> MultiSortedDocValues::get_value_count() const {
    return static_cast<int32_t>(mapping_.value_count());
}

// ---------------- MultiSortedSetDocValues ----------------

MultiSortedSetDocValues::MultiSortedSetDocValues(std::vector<std::unique_ptr<SortedSetDocValues>> values,
                                                 std::span<const LeafReaderContext> leaves, int32_t max_doc,
                                                 GlobalOrdinalMap mapping, int64_t total_cost)
    : values_(std::move(values)),
      leaves_(leaves),
      max_doc_(max_doc),
      mapping_(std::move(mapping)),
      total_cost_(total_cost) {}

Result<int32_t> MultiSortedSetDocValues::next_doc() {
    while (true) {
        if (current_index_ < 0) {
            if (next_leaf_ == values_.size()) {
                doc_id_ = NO_MORE_DOCS;
                return doc_id_;
            }
            current_doc_base_ = leaves_[next_leaf_].doc_base();
            current_index_ = static_cast<int32_t>(next_leaf_);
            ++next_leaf_;
        }
        auto new_doc = values_[static_cast<size_t>(current_index_)]->next_doc();
        if (!new_doc.has_value()) {
            return std::unexpected(std::move(new_doc.error()));
        }
        if (new_doc.value() == NO_MORE_DOCS) {
            current_index_ = -1;
        } else {
            doc_id_ = current_doc_base_ + new_doc.value();
            return doc_id_;
        }
    }
}

Result<int32_t> MultiSortedSetDocValues::advance(int32_t target) {
    if (target <= doc_id_) {
        return err(LuceneError::illegal_argument(std::format(
                "can only advance beyond current document: on docID={} but targetDocID={}", doc_id_, target)));
    }
    const int32_t reader_index = ReaderUtil::sub_index_with_leaves(target, leaves_);
    if (static_cast<size_t>(reader_index) >= next_leaf_) {
        if (static_cast<size_t>(reader_index) == values_.size()) {
            current_index_ = -1;
            doc_id_ = NO_MORE_DOCS;
            return doc_id_;
        }
        current_doc_base_ = leaves_[static_cast<size_t>(reader_index)].doc_base();
        current_index_ = reader_index;
        next_leaf_ = static_cast<size_t>(reader_index) + 1;
    }
    if (current_index_ < 0) {
        return next_doc();
    }
    auto new_doc = values_[static_cast<size_t>(current_index_)]->advance(target - current_doc_base_);
    if (!new_doc.has_value()) {
        return std::unexpected(std::move(new_doc.error()));
    }
    if (new_doc.value() == NO_MORE_DOCS) {
        current_index_ = -1;
        return next_doc();
    }
    doc_id_ = current_doc_base_ + new_doc.value();
    return doc_id_;
}

Result<bool> MultiSortedSetDocValues::advance_exact(int32_t target) {
    if (target < doc_id_) {
        return err(LuceneError::illegal_argument(std::format(
                "can only advance beyond current document: on docID={} but targetDocID={}", doc_id_, target)));
    }
    if (target >= max_doc_) {
        return err(LuceneError::illegal_argument(std::format("Out of range: {}", target)));
    }
    const int32_t reader_index = ReaderUtil::sub_index_with_leaves(target, leaves_);
    if (static_cast<size_t>(reader_index) >= next_leaf_) {
        current_doc_base_ = leaves_[static_cast<size_t>(reader_index)].doc_base();
        current_index_ = reader_index;
        next_leaf_ = static_cast<size_t>(reader_index) + 1;
    }
    doc_id_ = target;
    if (current_index_ < 0) {
        return false;
    }
    return values_[static_cast<size_t>(current_index_)]->advance_exact(target - current_doc_base_);
}

Result<int64_t> MultiSortedSetDocValues::next_ord() {
    if (current_index_ < 0) {
        return err(LuceneError::illegal_state("next_ord() without a current document"));
    }
    auto segment_ord = values_[static_cast<size_t>(current_index_)]->next_ord();
    if (!segment_ord.has_value()) {
        return std::unexpected(std::move(segment_ord.error()));
    }
    if (segment_ord.value() == NO_MORE_ORDS) {
        return NO_MORE_ORDS;
    }
    const int64_t global = mapping_.global_ord(static_cast<size_t>(current_index_), segment_ord.value());
    if (global < 0) {
        return err(LuceneError::illegal_state("the ordinal map has no global ordinal for this term"));
    }
    return global;
}

Result<int32_t> MultiSortedSetDocValues::doc_value_count() {
    if (current_index_ < 0) {
        return err(LuceneError::illegal_state("doc_value_count() without a current document"));
    }
    return values_[static_cast<size_t>(current_index_)]->doc_value_count();
}

Result<BytesRef> MultiSortedSetDocValues::lookup_ord(int64_t ord) {
    auto segment = mapping_.first_segment_of(ord);
    if (!segment.has_value()) {
        return std::unexpected(std::move(segment.error()));
    }
    return values_[segment.value().first]->lookup_ord(segment.value().second);
}

Result<int64_t> MultiSortedSetDocValues::get_value_count() const { return mapping_.value_count(); }

// ---------------- MultiDocValues ----------------

namespace {

/// Cost is only a hint; a cursor that does not implement it contributes 0.
int64_t cost_or_zero(DocIdSetIterator &iterator) {
    auto cost = iterator.cost();
    return cost.has_value() ? cost.value() : int64_t{0};
}

} // namespace

Result<std::unique_ptr<NumericDocValues>> MultiDocValues::get_norm_values(const IndexReader &reader,
                                                                          std::string_view field) {
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    const std::span<const LeafReaderContext> contexts = leaves.value();
    if (contexts.empty()) {
        return std::unique_ptr<NumericDocValues>{};
    }
    if (contexts.size() == 1) {
        return contexts[0].reader()->get_norm_values(field);
    }
    bool any = false;
    for (const LeafReaderContext &context : contexts) {
        auto values = context.reader()->get_norm_values(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() != nullptr) {
            any = true;
            break;
        }
    }
    if (!any) {
        return std::unique_ptr<NumericDocValues>{};
    }
    return std::unique_ptr<NumericDocValues>(
            new MultiNumericDocValues(contexts, std::string(field), NumericSource::kNorm));
}

Result<std::unique_ptr<NumericDocValues>> MultiDocValues::get_numeric_values(const IndexReader &reader,
                                                                             std::string_view field) {
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    const std::span<const LeafReaderContext> contexts = leaves.value();
    if (contexts.empty()) {
        return std::unique_ptr<NumericDocValues>{};
    }
    if (contexts.size() == 1) {
        return contexts[0].reader()->get_numeric_doc_values(field);
    }
    bool any = false;
    for (const LeafReaderContext &context : contexts) {
        auto values = context.reader()->get_numeric_doc_values(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() != nullptr) {
            any = true;
            break;
        }
    }
    if (!any) {
        return std::unique_ptr<NumericDocValues>{};
    }
    return std::unique_ptr<NumericDocValues>(
            new MultiNumericDocValues(contexts, std::string(field), NumericSource::kDocValues));
}

Result<std::unique_ptr<BinaryDocValues>> MultiDocValues::get_binary_values(const IndexReader &reader,
                                                                          std::string_view field) {
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    const std::span<const LeafReaderContext> contexts = leaves.value();
    if (contexts.empty()) {
        return std::unique_ptr<BinaryDocValues>{};
    }
    if (contexts.size() == 1) {
        return contexts[0].reader()->get_binary_doc_values(field);
    }
    bool any = false;
    for (const LeafReaderContext &context : contexts) {
        auto values = context.reader()->get_binary_doc_values(field);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        if (values.value() != nullptr) {
            any = true;
            break;
        }
    }
    if (!any) {
        return std::unique_ptr<BinaryDocValues>{};
    }
    return std::unique_ptr<BinaryDocValues>(new MultiBinaryDocValues(contexts, std::string(field)));
}

Result<std::unique_ptr<SortedNumericDocValues>> MultiDocValues::get_sorted_numeric_values(const IndexReader &reader,
                                                                                          std::string_view field) {
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    auto max_doc = reader.max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    const std::span<const LeafReaderContext> contexts = leaves.value();
    if (contexts.empty()) {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    if (contexts.size() == 1) {
        return contexts[0].reader()->get_sorted_numeric_doc_values(field);
    }

    std::vector<std::unique_ptr<SortedNumericDocValues>> values;
    values.reserve(contexts.size());
    bool any = false;
    int64_t total_cost = 0;
    for (const LeafReaderContext &context : contexts) {
        auto leaf_values = context.reader()->get_sorted_numeric_doc_values(field);
        if (!leaf_values.has_value()) {
            return std::unexpected(std::move(leaf_values.error()));
        }
        if (leaf_values.value() != nullptr) {
            any = true;
            total_cost += cost_or_zero(*leaf_values.value());
            values.push_back(std::move(leaf_values.value()));
        } else {
            values.push_back(std::make_unique<EmptySortedNumericDocValues>());
        }
    }
    if (!any) {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    return std::unique_ptr<SortedNumericDocValues>(
            new MultiSortedNumericDocValues(std::move(values), contexts, max_doc.value(), total_cost));
}

Result<std::unique_ptr<SortedDocValues>> MultiDocValues::get_sorted_values(const IndexReader &reader,
                                                                          std::string_view field) {
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    auto max_doc = reader.max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    const std::span<const LeafReaderContext> contexts = leaves.value();
    if (contexts.empty()) {
        return std::unique_ptr<SortedDocValues>{};
    }
    if (contexts.size() == 1) {
        return contexts[0].reader()->get_sorted_doc_values(field);
    }

    std::vector<std::unique_ptr<SortedDocValues>> values;
    values.reserve(contexts.size());
    std::vector<std::vector<BytesRef>> dictionaries;
    dictionaries.reserve(contexts.size());
    bool any = false;
    int64_t total_cost = 0;
    for (const LeafReaderContext &context : contexts) {
        auto leaf_values = context.reader()->get_sorted_doc_values(field);
        if (!leaf_values.has_value()) {
            return std::unexpected(std::move(leaf_values.error()));
        }
        std::vector<BytesRef> dictionary;
        if (leaf_values.value() != nullptr) {
            any = true;
            total_cost += cost_or_zero(*leaf_values.value());
            auto count = leaf_values.value()->get_value_count();
            if (!count.has_value()) {
                return std::unexpected(std::move(count.error()));
            }
            dictionary.reserve(static_cast<size_t>(count.value()));
            for (int32_t ord = 0; ord < count.value(); ++ord) {
                auto term = leaf_values.value()->lookup_ord(ord);
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                dictionary.push_back(std::move(term.value()));
            }
            values.push_back(std::move(leaf_values.value()));
        } else {
            values.push_back(std::make_unique<EmptySortedDocValues>());
        }
        dictionaries.push_back(std::move(dictionary));
    }
    if (!any) {
        return std::unique_ptr<SortedDocValues>{};
    }
    auto mapping = GlobalOrdinalMap::build(std::move(dictionaries));
    if (!mapping.has_value()) {
        return std::unexpected(std::move(mapping.error()));
    }
    return std::unique_ptr<SortedDocValues>(new MultiSortedDocValues(
            std::move(values), contexts, max_doc.value(), std::move(mapping.value()), total_cost));
}

Result<std::unique_ptr<SortedSetDocValues>> MultiDocValues::get_sorted_set_values(const IndexReader &reader,
                                                                                 std::string_view field) {
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    auto max_doc = reader.max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    const std::span<const LeafReaderContext> contexts = leaves.value();
    if (contexts.empty()) {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    if (contexts.size() == 1) {
        return contexts[0].reader()->get_sorted_set_doc_values(field);
    }

    std::vector<std::unique_ptr<SortedSetDocValues>> values;
    values.reserve(contexts.size());
    std::vector<std::vector<BytesRef>> dictionaries;
    dictionaries.reserve(contexts.size());
    bool any = false;
    int64_t total_cost = 0;
    for (const LeafReaderContext &context : contexts) {
        auto leaf_values = context.reader()->get_sorted_set_doc_values(field);
        if (!leaf_values.has_value()) {
            return std::unexpected(std::move(leaf_values.error()));
        }
        std::vector<BytesRef> dictionary;
        if (leaf_values.value() != nullptr) {
            any = true;
            total_cost += cost_or_zero(*leaf_values.value());
            auto count = leaf_values.value()->get_value_count();
            if (!count.has_value()) {
                return std::unexpected(std::move(count.error()));
            }
            dictionary.reserve(static_cast<size_t>(count.value()));
            for (int64_t ord = 0; ord < count.value(); ++ord) {
                auto term = leaf_values.value()->lookup_ord(ord);
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                dictionary.push_back(std::move(term.value()));
            }
            values.push_back(std::move(leaf_values.value()));
        } else {
            values.push_back(std::make_unique<EmptySortedSetDocValues>());
        }
        dictionaries.push_back(std::move(dictionary));
    }
    if (!any) {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    auto mapping = GlobalOrdinalMap::build(std::move(dictionaries));
    if (!mapping.has_value()) {
        return std::unexpected(std::move(mapping.error()));
    }
    return std::unique_ptr<SortedSetDocValues>(new MultiSortedSetDocValues(
            std::move(values), contexts, max_doc.value(), std::move(mapping.value()), total_cost));
}

} // namespace lucene
