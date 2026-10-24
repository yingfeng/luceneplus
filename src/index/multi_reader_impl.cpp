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

module lucene_core:multi_reader.impl;

import :multi_reader;

import std;

import :base_composite_reader;
import :error;
import :index_reader;
import :leaf_reader_context;
import :stored_fields;
import :term_vectors;
import :types;

namespace lucene {

Result<std::shared_ptr<MultiReader>> MultiReader::create(std::vector<std::shared_ptr<IndexReader>> sub_readers) {
    return create_with_close_sub_readers(true, std::move(sub_readers));
}

Result<std::shared_ptr<MultiReader>>
MultiReader::create_with_close_sub_readers(bool close_sub_readers,
                                           std::vector<std::shared_ptr<IndexReader>> sub_readers) {
    for (const std::shared_ptr<IndexReader> &reader : sub_readers) {
        if (reader == nullptr) {
            return err(LuceneError::illegal_argument("a sub-reader must not be null"));
        }
    }
    auto base = CompositeReaderBase::create(std::move(sub_readers));
    if (!base.has_value()) {
        return std::unexpected(std::move(base.error()));
    }
    std::shared_ptr<MultiReader> reader(new MultiReader());
    reader->base_ = std::move(base.value());
    reader->close_sub_readers_ = close_sub_readers;
    return reader;
}

Result<std::span<const LeafReaderContext>> MultiReader::leaves() const {
    if (leaves_built_) {
        return std::span<const LeafReaderContext>(leaves_);
    }

    const std::vector<std::shared_ptr<IndexReader>> &subs = base_.sub_readers();
    std::vector<std::span<const LeafReaderContext>> child_leaves;
    child_leaves.reserve(subs.size());
    int32_t total = 0;
    for (const std::shared_ptr<IndexReader> &sub : subs) {
        auto leaves_of_sub = sub->leaves();
        if (!leaves_of_sub.has_value()) {
            return std::unexpected(std::move(leaves_of_sub.error()));
        }
        total += static_cast<int32_t>(leaves_of_sub.value().size());
        child_leaves.push_back(leaves_of_sub.value());
    }

    const TopParentMeta top_parent{total, base_.max_doc(), identity_};
    leaves_.clear();
    leaves_.reserve(static_cast<size_t>(total));
    int32_t ord = 0;
    for (size_t i = 0; i < subs.size(); ++i) {
        const int32_t doc_base = base_.starts()[i];
        for (const LeafReaderContext &child : child_leaves[i]) {
            leaves_.emplace_back(child.reader(), ord, doc_base + child.doc_base(), top_parent);
            ++ord;
        }
    }
    leaves_built_ = true;
    return std::span<const LeafReaderContext>(leaves_);
}

Result<std::unique_ptr<StoredFields>> MultiReader::stored_fields() { return base_.stored_fields(); }

Result<std::unique_ptr<TermVectors>> MultiReader::term_vectors() { return base_.term_vectors(); }

VoidResult MultiReader::close() const {
    if (closed_) {
        return {};
    }
    closed_ = true;
    if (!close_sub_readers_) {
        return {};
    }

    std::optional<LuceneError> first_io_error;
    for (const std::shared_ptr<IndexReader> &sub : base_.sub_readers()) {
        auto closed_sub = sub->close();
        if (closed_sub.has_value()) {
            continue;
        }
        if (!closed_sub.error().is_io_error()) {
            // A misuse (already closed, unsupported operation, ...) is worth
            // reporting right away.
            return std::unexpected(std::move(closed_sub.error()));
        }
        if (!first_io_error.has_value()) {
            first_io_error = closed_sub.error();
        }
    }
    if (first_io_error.has_value()) {
        return std::unexpected(std::move(first_io_error.value()));
    }
    return {};
}

std::string MultiReader::to_string() const {
    return "MultiReader(sub_readers=" + std::to_string(base_.sub_readers().size()) +
           ", max_doc=" + std::to_string(base_.max_doc()) + ")";
}

} // namespace lucene
