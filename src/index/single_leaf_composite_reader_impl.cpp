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

module lucene_core:single_leaf_composite_reader.impl;

import :single_leaf_composite_reader;

import std;

import :close;
import :error;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :stored_fields;
import :term;
import :term_vectors;
import :types;

namespace lucene {

Result<std::shared_ptr<SingleLeafCompositeReader>>
SingleLeafCompositeReader::create(std::shared_ptr<LeafReader> leaf) {
    if (leaf == nullptr) {
        return err(LuceneError::illegal_argument("a single-leaf reader needs a leaf"));
    }
    return std::shared_ptr<SingleLeafCompositeReader>(new SingleLeafCompositeReader(std::move(leaf)));
}

Result<int32_t> SingleLeafCompositeReader::max_doc() const { return leaf_->max_doc(); }

Result<int32_t> SingleLeafCompositeReader::num_docs() const { return leaf_->num_docs(); }

Result<std::unique_ptr<StoredFields>> SingleLeafCompositeReader::stored_fields() { return leaf_->stored_fields(); }

Result<std::unique_ptr<TermVectors>> SingleLeafCompositeReader::term_vectors() { return leaf_->term_vectors(); }

Result<int32_t> SingleLeafCompositeReader::doc_freq(const Term &term) {
    auto found = leaf_->terms(term.field());
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (found.value() == nullptr) {
        return 0;
    }
    auto found_term = found.value()->iterator();
    if (!found_term.has_value()) {
        return std::unexpected(std::move(found_term.error()));
    }
    if (found_term.value() == nullptr) {
        return 0;
    }
    auto exact = found_term.value()->seek_exact(term.bytes().as_slice());
    if (!exact.has_value()) {
        return std::unexpected(std::move(exact.error()));
    }
    if (!exact.value()) {
        return 0;
    }
    return found_term.value()->doc_freq();
}

Result<int64_t> SingleLeafCompositeReader::total_term_freq(const Term &term) {
    auto found = leaf_->terms(term.field());
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (found.value() == nullptr) {
        return 0;
    }
    auto found_term = found.value()->iterator();
    if (!found_term.has_value()) {
        return std::unexpected(std::move(found_term.error()));
    }
    if (found_term.value() == nullptr) {
        return 0;
    }
    auto exact = found_term.value()->seek_exact(term.bytes().as_slice());
    if (!exact.has_value()) {
        return std::unexpected(std::move(exact.error()));
    }
    if (!exact.value()) {
        return 0;
    }
    return found_term.value()->total_term_freq();
}

Result<int64_t> SingleLeafCompositeReader::sum_doc_freq(std::string_view field) {
    auto found = leaf_->terms(field);
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (found.value() == nullptr) {
        return 0;
    }
    return found.value()->sum_doc_freq();
}

Result<int32_t> SingleLeafCompositeReader::doc_count(std::string_view field) {
    auto found = leaf_->terms(field);
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (found.value() == nullptr) {
        return 0;
    }
    return found.value()->doc_count();
}

Result<int64_t> SingleLeafCompositeReader::sum_total_term_freq(std::string_view field) {
    auto found = leaf_->terms(field);
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    if (found.value() == nullptr) {
        return 0;
    }
    return found.value()->sum_total_term_freq();
}

Result<std::span<const LeafReaderContext>> SingleLeafCompositeReader::leaves() const {
    if (leaves_.empty()) {
        auto max_doc = leaf_->max_doc();
        if (!max_doc.has_value()) {
            return std::unexpected(std::move(max_doc.error()));
        }
        TopParentMeta top_parent;
        top_parent.leaves_num = 1;
        top_parent.max_doc = max_doc.value();
        top_parent.id = leaf_->identity();
        leaves_.emplace_back(leaf_.get(), 0, 0, top_parent);
    }
    return std::span<const LeafReaderContext>(leaves_);
}

const Identity &SingleLeafCompositeReader::identity() const { return leaf_->identity(); }

VoidResult SingleLeafCompositeReader::close() const { return leaf_->close(); }

std::string SingleLeafCompositeReader::to_string() const {
    return "SingleLeafCompositeReader(" + leaf_->to_string() + ")";
}

} // namespace lucene
