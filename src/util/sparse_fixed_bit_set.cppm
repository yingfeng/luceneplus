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

export module lucene_core:sparse_fixed_bit_set;

import :types;
import std;

import :bits;
import :doc_id_set_iterator;
import :error;

namespace lucene {

/// A [`Bits`] implementation that only stores the 64 bit words that have a bit
/// set.
///
/// Port of `SparseFixedBitSet` (`util/sparse_fixed_bit_set.rs`).
///
/// The space is cut into blocks of 4096 bits (64 words).  Each block keeps
///  * an `indices` word, whose bit `i` says that the `i`-th word of the block is
///    present, and
///  * the non-zero words themselves, in order, so the position of word `i` is the
///    number of set bits below `i` in the indices word.
///
/// A block with nothing set costs one word instead of 64, which is what makes
/// this worth it for the sparse sets that queries produce (a filter over a few
/// matching documents of a million), and why
/// [`DocIdSetIterator`]-shaped consumers use it instead of
/// [`FixedBitSet`](:fixed_bit_set).
///
/// **Deviations** (C++ mechanics only):
///  * `create` returns a `Result` because the length is validated, which a
///    constructor cannot report;
///  * the reference implementation's `or_sparse` / `or_dense` fast paths are replaced
///    by one straightforward [`or_from_iterator`], which is correct but does not
///    exploit the ordering of the iterator;
///  * there is no `Accountable` / `BitSet` trait implementation in this port; the
///    useful subset lives directly on the class.
export class SparseFixedBitSet final : public Bits {
public:
    /// Creates a set with `length` bits, all clear.
    ///
    /// Fails with `illegal_argument` when `length < 1`.
    [[nodiscard]] static Result<SparseFixedBitSet> create(size_t length);

    // ---- Bits ----

    /// The bit at `index`; fails with `array_index_out_of_bounds` past the end.
    [[nodiscard]] Result<bool> get(size_t index) const override;
    [[nodiscard]] size_t length() const override { return length_; }
    [[nodiscard]] std::string to_string() const override;

    // ---- mutation ----

    /// Sets the bit at `index`; returns whether it was clear before.
    bool set(size_t index);
    /// Clears the bit at `index`; returns whether it was set before.
    bool clear(size_t index);
    /// Sets every document `it` yields; `it` must be unpositioned.
    [[nodiscard]] VoidResult or_from_iterator(DocIdSetIterator &it);

    // ---- queries ----

    /// Number of set bits.
    [[nodiscard]] size_t cardinality() const noexcept;
    /// The first set bit at or after `from`, or `-1` when there is none.
    [[nodiscard]] int64_t next_set_bit(size_t from) const;
    /// Rough heap footprint, for accounting.
    [[nodiscard]] int64_t approx_ram_bytes() const noexcept;
    /// Number of 64 bit words that are actually stored.
    [[nodiscard]] size_t non_zero_long_count() const noexcept { return non_zero_long_count_; }

private:
    SparseFixedBitSet(size_t length, size_t block_count);

    /// The position of the `i64`-th word of block `block` among the stored words.
    [[nodiscard]] size_t offset_in_block(size_t block, size_t i64) const noexcept {
        return static_cast<size_t>(std::popcount(indices_[block] & ((uint64_t{1} << (i64 & 63)) - 1)));
    }

    std::vector<uint64_t> indices_;
    /// `words_[block]` holds the non-zero words of `block`, in order.
    std::vector<std::vector<uint64_t>> words_;
    size_t length_;
    size_t non_zero_long_count_{0};
};

/// Bits per block: 4096 bits are 64 words.
export inline constexpr size_t SPARSE_FIXED_BIT_SET_BLOCK_BITS = 1U << 12;

} // namespace lucene
