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

module lucene_core:multi_bits.impl;

import :multi_bits;

import std;

import :bits;
import :error;
import :index_reader;
import :leaf_reader;
import :leaf_reader_context;
import :reader_util;
import :types;

namespace lucene {

MultiBits::MultiBits(std::vector<std::shared_ptr<const Bits>> subs, std::vector<int32_t> starts,
                     bool default_value)
    : subs_(std::move(subs)), starts_(std::move(starts)), default_value_(default_value) {}

Result<MultiBits> MultiBits::create(std::vector<std::shared_ptr<const Bits>> subs, std::vector<int32_t> starts,
                                    bool default_value) {
    if (starts.size() != subs.size() + 1) {
        return err(LuceneError::illegal_argument("starts must have one more entry than subs (starts=" +
                                                 std::to_string(starts.size()) +
                                                 ", subs=" + std::to_string(subs.size()) + ")"));
    }
    for (size_t i = 1; i < starts.size(); ++i) {
        if (starts[i] < starts[i - 1]) {
            return err(LuceneError::illegal_argument("starts must be non-decreasing"));
        }
    }
    return MultiBits(std::move(subs), std::move(starts), default_value);
}

Result<bool> MultiBits::get(size_t index) const {
    if (index >= length()) {
        return err(LuceneError::array_index_out_of_bounds("index " + std::to_string(index) + " >= " +
                                                          std::to_string(length())));
    }
    const int32_t reader = ReaderUtil::sub_index(static_cast<int32_t>(index), starts_);
    if (reader < 0 || static_cast<size_t>(reader) >= subs_.size()) {
        return err(LuceneError::illegal_argument("document " + std::to_string(index) + " is outside the bits"));
    }
    const std::shared_ptr<const Bits> &bits = subs_[static_cast<size_t>(reader)];
    if (bits == nullptr) {
        return default_value_;
    }
    return bits->get(index - static_cast<size_t>(starts_[static_cast<size_t>(reader)]));
}

size_t MultiBits::length() const { return static_cast<size_t>(starts_.back()); }

std::string MultiBits::to_string() const {
    std::string result = std::to_string(subs_.size()) + " subs: ";
    for (size_t i = 0; i < subs_.size(); ++i) {
        if (i != 0) {
            result += "; ";
        }
        const std::shared_ptr<const Bits> &bits = subs_[i];
        if (bits == nullptr) {
            result += "s=" + std::to_string(starts_[i]) + " l=null";
        } else {
            result += "s=" + std::to_string(starts_[i]) + " l=" + std::to_string(bits->length()) +
                      " b=" + bits->to_string();
        }
    }
    result += " end=" + std::to_string(starts_.back());
    return result;
}

Result<std::shared_ptr<const Bits>> get_live_docs(const IndexReader &reader) {
    auto has_deletions = reader.has_deletions();
    if (!has_deletions.has_value()) {
        return std::unexpected(std::move(has_deletions.error()));
    }
    if (!has_deletions.value()) {
        return std::shared_ptr<const Bits>{};
    }

    auto max_doc = reader.max_doc();
    if (!max_doc.has_value()) {
        return std::unexpected(std::move(max_doc.error()));
    }
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    const std::span<const LeafReaderContext> contexts = leaves.value();
    if (contexts.empty()) {
        return err(LuceneError::illegal_state("a reader with deletions must have at least one leaf"));
    }

    if (contexts.size() == 1) {
        auto bits = contexts[0].reader()->live_docs();
        if (!bits.has_value()) {
            return std::unexpected(std::move(bits.error()));
        }
        if (bits.value() == nullptr) {
            return std::shared_ptr<const Bits>{};
        }
        // Borrowed from the leaf; `reader` keeps it alive (see the doc comment).
        return std::shared_ptr<const Bits>(bits.value(), [](const Bits *) {});
    }

    std::vector<std::shared_ptr<const Bits>> live_docs;
    std::vector<int32_t> starts;
    live_docs.reserve(contexts.size());
    starts.reserve(contexts.size() + 1);
    for (const LeafReaderContext &context : contexts) {
        auto bits = context.reader()->live_docs();
        if (!bits.has_value()) {
            return std::unexpected(std::move(bits.error()));
        }
        if (bits.value() == nullptr) {
            live_docs.emplace_back();
        } else {
            live_docs.emplace_back(bits.value(), [](const Bits *) {});
        }
        starts.push_back(context.doc_base());
    }
    starts.push_back(max_doc.value());

    auto multi = MultiBits::create(std::move(live_docs), std::move(starts), true);
    if (!multi.has_value()) {
        return std::unexpected(std::move(multi.error()));
    }
    return std::make_shared<MultiBits>(std::move(multi.value()));
}

} // namespace lucene
