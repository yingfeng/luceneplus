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

module lucene_core:sparse_fixed_bit_set.impl;

import :sparse_fixed_bit_set;

import std;

import :bits;
import :debug;
import :doc_id_set_iterator;
import :error;
import :types;

namespace lucene {

namespace {

/// Number of 4096 bit blocks needed for `length` bits.
size_t blocks_for(size_t length) {
    size_t blocks = length >> 12;
    if ((blocks << 12) < length) {
        ++blocks;
    }
    return blocks;
}

/// The bit of `index` inside its word.
constexpr uint64_t bit_mask(size_t index) noexcept { return uint64_t{1} << (index & 63); }

} // namespace

SparseFixedBitSet::SparseFixedBitSet(size_t length, size_t block_count)
    : indices_(block_count, 0), words_(block_count), length_(length) {}

Result<SparseFixedBitSet> SparseFixedBitSet::create(size_t length) {
    if (length < 1) {
        return err(LuceneError::illegal_argument("length needs to be >= 1"));
    }
    return SparseFixedBitSet(length, blocks_for(length));
}

Result<bool> SparseFixedBitSet::get(size_t index) const {
    if (index >= length_) {
        return err(LuceneError::array_index_out_of_bounds("index " + std::to_string(index) + " >= " +
                                                         std::to_string(length_)));
    }
    const size_t block = index >> 12;
    const size_t word = index >> 6;
    if ((indices_[block] & (uint64_t{1} << (word & 63))) == 0) {
        return false; // The word is all zeros, so it is not stored.
    }
    return (words_[block][offset_in_block(block, word)] & bit_mask(index)) != 0;
}

bool SparseFixedBitSet::set(size_t index) {
    debug_check(index < length_);
    const size_t block = index >> 12;
    const size_t word = index >> 6;
    const uint64_t word_mask = uint64_t{1} << (word & 63);
    uint64_t &indices = indices_[block];

    if ((indices & word_mask) == 0) {
        // First bit of this word: store the word at its ordered position.
        const size_t position = offset_in_block(block, word);
        indices |= word_mask;
        words_[block].insert(words_[block].begin() + static_cast<std::ptrdiff_t>(position), bit_mask(index));
        ++non_zero_long_count_;
        return true;
    }

    const size_t position = offset_in_block(block, word);
    uint64_t &stored = words_[block][position];
    if ((stored & bit_mask(index)) != 0) {
        return false;
    }
    stored |= bit_mask(index);
    return true;
}

bool SparseFixedBitSet::clear(size_t index) {
    debug_check(index < length_);
    const size_t block = index >> 12;
    const size_t word = index >> 6;
    const uint64_t word_mask = uint64_t{1} << (word & 63);
    uint64_t &indices = indices_[block];
    if ((indices & word_mask) == 0) {
        return false;
    }

    const size_t position = offset_in_block(block, word);
    uint64_t &stored = words_[block][position];
    if ((stored & bit_mask(index)) == 0) {
        return false;
    }
    stored &= ~bit_mask(index);
    if (stored == 0) {
        // The word became empty: drop it so the block stays sparse.
        words_[block].erase(words_[block].begin() + static_cast<std::ptrdiff_t>(position));
        indices &= ~word_mask;
        --non_zero_long_count_;
    }
    return true;
}

VoidResult SparseFixedBitSet::or_from_iterator(DocIdSetIterator &it) {
    while (true) {
        auto doc = it.next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() == NO_MORE_DOCS) {
            return {};
        }
        if (doc.value() < 0 || static_cast<size_t>(doc.value()) >= length_) {
            return err(LuceneError::illegal_argument("document " + std::to_string(doc.value()) +
                                                     " is outside the set of " + std::to_string(length_) + " bits"));
        }
        set(static_cast<size_t>(doc.value()));
    }
}

size_t SparseFixedBitSet::cardinality() const noexcept {
    size_t count = 0;
    for (const std::vector<uint64_t> &block : words_) {
        for (const uint64_t word : block) {
            count += static_cast<size_t>(std::popcount(word));
        }
    }
    return count;
}

int64_t SparseFixedBitSet::next_set_bit(size_t from) const {
    if (from >= length_) {
        return -1;
    }

    const size_t block = from >> 12;
    const uint64_t indices = indices_[block];
    const size_t start_word = from >> 6;
    const uint64_t start_word_mask = uint64_t{1} << (start_word & 63);
    size_t position = static_cast<size_t>(std::popcount(indices & (start_word_mask - 1)));

    if ((indices & start_word_mask) != 0) {
        // There is something in the current word; look at the bits at or after
        // `from` only.
        const uint64_t bits = words_[block][position] & (~uint64_t{0} << (from & 63));
        if (bits != 0) {
            return static_cast<int64_t>((start_word << 6) | std::countr_zero(bits));
        }
        ++position;
    }

    // Remaining words of this block.
    const uint64_t remaining = (indices >> (start_word & 63)) >> 1;
    if (remaining != 0) {
        const size_t skipped = static_cast<size_t>(std::countr_zero(remaining)) + 1;
        const size_t word = start_word + skipped;
        return static_cast<int64_t>((word << 6) | std::countr_zero(words_[block][position]));
    }

    // Later blocks.
    for (size_t next = block + 1; next < indices_.size(); ++next) {
        if (indices_[next] == 0) {
            continue;
        }
        const size_t word = static_cast<size_t>(std::countr_zero(indices_[next]));
        return static_cast<int64_t>((next << 12) + (word << 6) + std::countr_zero(words_[next][0]));
    }
    return -1;
}

int64_t SparseFixedBitSet::approx_ram_bytes() const noexcept {
    int64_t bytes = static_cast<int64_t>(indices_.capacity() * sizeof(uint64_t));
    bytes += static_cast<int64_t>(words_.capacity() * sizeof(std::vector<uint64_t>));
    for (const std::vector<uint64_t> &block : words_) {
        bytes += static_cast<int64_t>(block.capacity() * sizeof(uint64_t));
    }
    return bytes;
}

std::string SparseFixedBitSet::to_string() const {
    return "SparseFixedBitSet(length=" + std::to_string(length_) + ", cardinality=" +
           std::to_string(cardinality()) + ")";
}

} // namespace lucene
