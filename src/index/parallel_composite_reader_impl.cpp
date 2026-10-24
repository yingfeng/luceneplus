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

module lucene_core:parallel_composite_reader.impl;

import :parallel_composite_reader;

import std;

import :base_composite_reader;
import :error;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :parallel_leaf_reader;
import :stored_fields;
import :term_vectors;
import :types;

namespace lucene {

namespace {

/// A `shared_ptr` that does not own its target: the flattened leaves belong to
/// the sub-readers the composite reader keeps alive.
std::shared_ptr<LeafReader> borrow(LeafReader *reader) {
    return std::shared_ptr<LeafReader>(reader, [](LeafReader *) {});
}

/// Validates that `readers` agree with `leaf_max_doc`.
VoidResult validate_shape(const std::vector<std::shared_ptr<IndexReader>> &readers, int32_t max_doc,
                          const std::vector<int32_t> &leaf_max_doc) {
    for (const std::shared_ptr<IndexReader> &reader : readers) {
        auto actual_max_doc = reader->max_doc();
        if (!actual_max_doc.has_value()) {
            return std::unexpected(std::move(actual_max_doc.error()));
        }
        if (actual_max_doc.value() != max_doc) {
            return err(LuceneError::illegal_argument("All readers must have same maxDoc: " +
                                                     std::to_string(max_doc) + "!=" +
                                                     std::to_string(actual_max_doc.value())));
        }
        auto leaves = reader->leaves();
        if (!leaves.has_value()) {
            return std::unexpected(std::move(leaves.error()));
        }
        if (leaves.value().size() != leaf_max_doc.size()) {
            return err(LuceneError::illegal_argument("All readers must have same number of leaf readers"));
        }
        for (size_t i = 0; i < leaf_max_doc.size(); ++i) {
            auto leaf_max = leaves.value()[i].reader()->max_doc();
            if (!leaf_max.has_value()) {
                return std::unexpected(std::move(leaf_max.error()));
            }
            if (leaf_max.value() != leaf_max_doc[i]) {
                return err(LuceneError::illegal_argument(
                        "All leaf readers must have same corresponding subReader maxDoc"));
            }
        }
    }
    return {};
}

} // namespace

Result<std::shared_ptr<ParallelCompositeReader>>
ParallelCompositeReader::create(std::vector<std::shared_ptr<IndexReader>> readers) {
    return create_with_close_sub_readers(true, std::move(readers));
}

Result<std::shared_ptr<ParallelCompositeReader>>
ParallelCompositeReader::create_with_close_sub_readers(bool close_sub_readers,
                                                       std::vector<std::shared_ptr<IndexReader>> readers) {
    return create_with_stored_fields(close_sub_readers, std::move(readers), {});
}

Result<std::shared_ptr<ParallelCompositeReader>>
ParallelCompositeReader::create_with_stored_fields(bool close_sub_readers,
                                                   std::vector<std::shared_ptr<IndexReader>> readers,
                                                   std::vector<std::shared_ptr<IndexReader>> stored_fields_readers) {
    if (readers.empty() && !stored_fields_readers.empty()) {
        return err(LuceneError::illegal_argument(
                "There must be at least one main reader if storedFieldsReaders are used."));
    }
    for (const std::shared_ptr<IndexReader> &reader : readers) {
        if (reader == nullptr) {
            return err(LuceneError::illegal_argument("a sub-reader must not be null"));
        }
    }
    for (const std::shared_ptr<IndexReader> &reader : stored_fields_readers) {
        if (reader == nullptr) {
            return err(LuceneError::illegal_argument("a stored-fields reader must not be null"));
        }
    }

    std::shared_ptr<ParallelCompositeReader> combined(new ParallelCompositeReader());
    combined->close_sub_readers_ = close_sub_readers;
    combined->readers_ = std::move(readers);
    combined->stored_fields_readers_ =
            stored_fields_readers.empty() ? combined->readers_ : std::move(stored_fields_readers);

    for (const std::shared_ptr<IndexReader> &reader : combined->readers_) {
        combined->all_readers_.push_back(reader);
    }
    for (const std::shared_ptr<IndexReader> &reader : combined->stored_fields_readers_) {
        const bool known = std::ranges::any_of(
                combined->all_readers_, [&reader](const std::shared_ptr<IndexReader> &existing) {
                    return existing.get() == reader.get();
                });
        if (!known) {
            combined->all_readers_.push_back(reader);
        }
    }

    if (combined->readers_.empty()) {
        auto base = CompositeReaderBase::create({});
        if (!base.has_value()) {
            return std::unexpected(std::move(base.error()));
        }
        combined->base_ = std::move(base.value());
        return combined;
    }

    // The shape of the leaf structure comes from the first reader.
    auto first_leaves = combined->readers_[0]->leaves();
    if (!first_leaves.has_value()) {
        return std::unexpected(std::move(first_leaves.error()));
    }
    auto first_max_doc = combined->readers_[0]->max_doc();
    if (!first_max_doc.has_value()) {
        return std::unexpected(std::move(first_max_doc.error()));
    }
    std::vector<int32_t> leaf_max_doc;
    leaf_max_doc.reserve(first_leaves.value().size());
    for (const LeafReaderContext &context : first_leaves.value()) {
        auto leaf_max = context.reader()->max_doc();
        if (!leaf_max.has_value()) {
            return std::unexpected(std::move(leaf_max.error()));
        }
        leaf_max_doc.push_back(leaf_max.value());
    }

    if (auto valid = validate_shape(combined->readers_, first_max_doc.value(), leaf_max_doc); !valid.has_value()) {
        return std::unexpected(std::move(valid.error()));
    }
    if (auto valid = validate_shape(combined->stored_fields_readers_, first_max_doc.value(), leaf_max_doc);
        !valid.has_value()) {
        return std::unexpected(std::move(valid.error()));
    }

    // Group the corresponding leaves of every reader into one ParallelLeafReader.
    combined->leaf_readers_.reserve(leaf_max_doc.size());
    std::vector<std::shared_ptr<IndexReader>> wrapped;
    wrapped.reserve(leaf_max_doc.size());
    for (size_t leaf_index = 0; leaf_index < leaf_max_doc.size(); ++leaf_index) {
        std::vector<std::shared_ptr<LeafReader>> parallel_leaves;
        parallel_leaves.reserve(combined->readers_.size());
        for (const std::shared_ptr<IndexReader> &reader : combined->readers_) {
            auto leaves = reader->leaves();
            if (!leaves.has_value()) {
                return std::unexpected(std::move(leaves.error()));
            }
            parallel_leaves.push_back(borrow(leaves.value()[leaf_index].reader()));
        }
        std::vector<std::shared_ptr<LeafReader>> stored_leaves;
        stored_leaves.reserve(combined->stored_fields_readers_.size());
        for (const std::shared_ptr<IndexReader> &reader : combined->stored_fields_readers_) {
            auto leaves = reader->leaves();
            if (!leaves.has_value()) {
                return std::unexpected(std::move(leaves.error()));
            }
            stored_leaves.push_back(borrow(leaves.value()[leaf_index].reader()));
        }

        // The synthetic readers must never close the real leaves: `close()`
        // below takes care of them.
        auto parallel = ParallelLeafReader::create_with_stored_fields(false, std::move(parallel_leaves),
                                                                     std::move(stored_leaves));
        if (!parallel.has_value()) {
            return std::unexpected(std::move(parallel.error()));
        }
        wrapped.push_back(parallel.value());
        combined->leaf_readers_.push_back(std::move(parallel.value()));
    }

    auto base = CompositeReaderBase::create(std::move(wrapped));
    if (!base.has_value()) {
        return std::unexpected(std::move(base.error()));
    }
    combined->base_ = std::move(base.value());
    return combined;
}

Result<std::span<const LeafReaderContext>> ParallelCompositeReader::leaves() const {
    if (leaves_built_) {
        return std::span<const LeafReaderContext>(leaves_);
    }
    const size_t leaves_num = leaf_readers_.size();
    leaves_.clear();
    leaves_.reserve(leaves_num);
    const TopParentMeta top_parent{static_cast<int32_t>(leaves_num), base_.max_doc(), identity_};
    for (size_t i = 0; i < leaves_num; ++i) {
        leaves_.emplace_back(leaf_readers_[i].get(), static_cast<int32_t>(i), base_.starts()[i], top_parent);
    }
    leaves_built_ = true;
    return std::span<const LeafReaderContext>(leaves_);
}

Result<std::unique_ptr<StoredFields>> ParallelCompositeReader::stored_fields() { return base_.stored_fields(); }

Result<std::unique_ptr<TermVectors>> ParallelCompositeReader::term_vectors() { return base_.term_vectors(); }

VoidResult ParallelCompositeReader::close() const {
    if (closed_) {
        return {};
    }
    closed_ = true;

    std::optional<LuceneError> first_io_error;
    if (close_sub_readers_) {
        for (const std::shared_ptr<IndexReader> &reader : all_readers_) {
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
    }

    // The synthetic leaves have no state of their own; closing them only marks
    // them closed.
    for (const std::shared_ptr<ParallelLeafReader> &reader : leaf_readers_) {
        auto closed_leaf = reader->close();
        if (closed_leaf.has_value()) {
            continue;
        }
        if (!closed_leaf.error().is_io_error()) {
            return std::unexpected(std::move(closed_leaf.error()));
        }
        if (!first_io_error.has_value()) {
            first_io_error = closed_leaf.error();
        }
    }

    if (first_io_error.has_value()) {
        return std::unexpected(std::move(first_io_error.value()));
    }
    return {};
}

std::string ParallelCompositeReader::to_string() const {
    std::string result = "ParallelCompositeReader(";
    for (size_t i = 0; i < leaf_readers_.size(); ++i) {
        if (i != 0) {
            result += " ";
        }
        result += leaf_readers_[i]->to_string();
    }
    result += ")";
    return result;
}

} // namespace lucene
