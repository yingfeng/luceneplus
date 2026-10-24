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

export module lucene_core:paged_bytes;

import :types;
import std;

import :bytes_ref;
import :data_input;
import :data_output;
import :error;
import :index_input;

namespace lucene {

export class PagedBytesReader;

/// A logical byte buffer stored as a series of fixed size pages.
///
/// Port of `PagedBytes` (`util/paged_bytes.rs`).
///
/// Bytes are appended with [`copy_with_input`] / [`copy_with_bytes_ref`] /
/// [`copy_using_length_prefix`]; [`freeze`] closes the buffer and returns a
/// [`PagedBytesReader`] over it.  Freezing is what makes the pages immutable and
/// shareable, which is the point: the writer hands out one reader and drops its
/// own copy of the pages.
///
/// **Deviations** (C++ mechanics only):
///  * [`copy_with_bytes_ref`] and [`PagedBytesReader::fill_slice`] hand back a
///    **copy** instead of a borrowed slice, because this port's [`BytesRef`] owns
///    its storage (the reference implementation returns a slice into the page);
///  * there is no `Accountable` trait; [`approx_ram_bytes`] reports the same
///    numbers directly.
export class PagedBytes final {
public:
    /// `block_bits` must be in `1..31`; the page size is `1 << block_bits`.
    explicit PagedBytes(size_t block_bits);

    /// Appends `byte_count` bytes read from `input`.
    [[nodiscard]] VoidResult copy_with_input(IndexInput &input, size_t byte_count);

    /// Appends `bytes`, which must fit into one page, and copies them into `out`.
    [[nodiscard]] VoidResult copy_with_bytes_ref(const BytesRef &bytes, BytesRef &out);

    /// Appends `bytes` with a one or two byte length prefix and returns the
    /// pointer of the prefix.
    ///
    /// Fails with `illegal_argument` when `bytes` is 32768 bytes or longer, or
    /// when a page cannot hold it plus its prefix.
    [[nodiscard]] Result<int64_t> copy_using_length_prefix(const BytesRef &bytes);

    /// Closes the buffer and returns a reader over it.
    ///
    /// `trim` drops the unused tail of the last page.  Fails with `illegal_state`
    /// when the buffer was already frozen, or when [`copy_with_bytes_ref`] was
    /// used (its pointers into a page cannot survive freezing).
    [[nodiscard]] Result<std::shared_ptr<const PagedBytesReader>> freeze(bool trim);

    /// Number of bytes written so far.
    [[nodiscard]] int64_t get_pointer() const noexcept;

    /// The reader created by [`freeze`], or `nullptr` while the buffer is still
    /// writable.
    [[nodiscard]] const std::shared_ptr<const PagedBytesReader> &reader() const noexcept { return reader_; }

    [[nodiscard]] size_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] size_t block_bits() const noexcept { return block_bits_; }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    /// Rough heap footprint, for accounting.
    [[nodiscard]] int64_t approx_ram_bytes() const noexcept;

    [[nodiscard]] std::string to_string() const;

private:
    [[nodiscard]] VoidResult start_new_block();
    [[nodiscard]] uint8_t *current_page();
    [[nodiscard]] const uint8_t *current_page() const;

    size_t block_bits_;
    size_t block_size_;
    size_t block_mask_;
    /// Committed pages.
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> pages_;
    /// The page being filled, if any.
    std::shared_ptr<std::vector<uint8_t>> current_page_;
    size_t upto_;
    bool did_skip_bytes_{false};
    bool frozen_{false};
    std::shared_ptr<const PagedBytesReader> reader_;
};

/// Reads slices out of a frozen [`PagedBytes`].
///
/// Port of `Reader` (`util/paged_bytes.rs`).  Instances share the frozen pages,
/// so a reader stays valid after the writer is gone.
export class PagedBytesReader final {
public:
    ~PagedBytesReader() = default;

    /// Copies the `length` bytes at `start` into `out`.
    ///
    /// A slice may straddle one page boundary, which is why the reader copies;
    /// a slice spanning more than two pages is not supported.
    [[nodiscard]] VoidResult fill_slice(size_t start, size_t length, BytesRef &out) const;

    /// The byte at `offset`.
    [[nodiscard]] Result<uint8_t> get_byte(size_t offset) const;

    /// Reads the length prefixed entry at `start` into `out`.  The entry must be
    /// inside one page.
    [[nodiscard]] VoidResult fill(size_t start, BytesRef &out) const;

    [[nodiscard]] size_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] size_t page_count() const noexcept { return pages_ == nullptr ? 0 : pages_->size(); }

    [[nodiscard]] std::string to_string() const;

private:
    friend class PagedBytes;
    PagedBytesReader(std::shared_ptr<const std::vector<std::shared_ptr<const std::vector<uint8_t>>>> pages,
                     size_t block_bits);

    std::shared_ptr<const std::vector<std::shared_ptr<const std::vector<uint8_t>>>> pages_;
    size_t block_bits_;
    size_t block_size_;
    size_t block_mask_;
};

/// A [`DataInput`] that walks the pages of a frozen [`PagedBytes`].
///
/// Port of `PagedBytesDataInput`.
export class PagedBytesDataInput final : public DataInput {
public:
    explicit PagedBytesDataInput(std::shared_ptr<const PagedBytesReader> reader);

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    /// The current byte position.
    [[nodiscard]] size_t get_position() const noexcept;
    /// Seeks to a position previously returned by [`get_position`].
    [[nodiscard]] VoidResult set_position(size_t position);

    [[nodiscard]] std::string to_string() const override { return "PagedBytesDataInput"; }

    [[nodiscard]] const PagedBytesReader &reader() const noexcept { return *reader_; }

private:
    std::shared_ptr<const PagedBytesReader> reader_;
    size_t position_{0};
};

/// A [`DataOutput`] that appends to a [`PagedBytes`].
///
/// Port of `PagedBytesDataOutput`.
export class PagedBytesDataOutput final : public DataOutput {
public:
    explicit PagedBytesDataOutput(PagedBytes &paged_bytes) : paged_bytes_(&paged_bytes) {}

    [[nodiscard]] VoidResult write_byte(uint8_t b) override;
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override;

    /// Number of bytes appended so far.
    [[nodiscard]] int64_t position() const { return paged_bytes_->get_pointer(); }

    [[nodiscard]] std::string to_string() const override { return "PagedBytesDataOutput"; }

private:
    PagedBytes *paged_bytes_;
};

/// A [`DataInput`] over `paged_bytes`, which must be frozen.
[[nodiscard]] Result<std::unique_ptr<DataInput>> get_paged_bytes_data_input(PagedBytes &paged_bytes);

/// A [`DataOutput`] appending to `paged_bytes`.
[[nodiscard]] std::unique_ptr<DataOutput> get_paged_bytes_data_output(PagedBytes &paged_bytes);

} // namespace lucene
