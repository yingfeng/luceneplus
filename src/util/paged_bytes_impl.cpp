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

module lucene_core:paged_bytes.impl;

import :paged_bytes;

import std;

import :bytes_ref;
import :data_input;
import :data_output;
import :debug;
import :error;
import :index_input;
import :types;

namespace lucene {

namespace {

constexpr size_t MAX_ENTRY_LENGTH = 32768;

} // namespace

// ---------------- PagedBytes ----------------

PagedBytes::PagedBytes(size_t block_bits)
    : block_bits_(block_bits),
      block_size_(size_t{1} << block_bits),
      block_mask_(block_size_ - 1),
      upto_(block_size_) {
    // The reference asserts the same range; a page must hold a whole entry.
    debug_check(block_bits_ > 0 && block_bits_ <= 31);
}

VoidResult PagedBytes::start_new_block() {
    current_page_ = std::make_shared<std::vector<uint8_t>>(block_size_, 0);
    upto_ = 0;
    return {};
}

uint8_t *PagedBytes::current_page() { return current_page_ == nullptr ? nullptr : current_page_->data(); }

const uint8_t *PagedBytes::current_page() const {
    return current_page_ == nullptr ? nullptr : current_page_->data();
}

VoidResult PagedBytes::copy_with_input(IndexInput &input, size_t byte_count) {
    while (byte_count > 0) {
        size_t left = block_size_ - upto_;
        if (left == 0) {
            if (current_page_ != nullptr) {
                pages_.push_back(current_page_);
                current_page_ = nullptr;
            }
            if (auto started = start_new_block(); !started.has_value()) {
                return started;
            }
            left = block_size_;
        }

        const size_t take = std::min(left, byte_count);
        if (auto read = input.read_bytes(std::span<uint8_t>(current_page() + upto_, take)); !read.has_value()) {
            return read;
        }
        upto_ += take;
        byte_count -= take;
    }
    return {};
}

VoidResult PagedBytes::copy_with_bytes_ref(const BytesRef &bytes, BytesRef &out) {
    const size_t length = bytes.length();
    const size_t left = block_size_ - upto_;
    if (length > left || current_page_ == nullptr) {
        if (current_page_ != nullptr) {
            pages_.push_back(current_page_);
            current_page_ = nullptr;
            did_skip_bytes_ = true;
        }
        if (auto started = start_new_block(); !started.has_value()) {
            return started;
        }
        if (length > block_size_) {
            return err(LuceneError::illegal_argument("the entry (" + std::to_string(length) +
                                                     " bytes) does not fit into a page of " +
                                                     std::to_string(block_size_) + " bytes"));
        }
    }

    const std::span<const uint8_t> source = bytes.as_slice();
    std::copy(source.begin(), source.end(), current_page() + static_cast<std::ptrdiff_t>(upto_));

    std::vector<uint8_t> copy(source.begin(), source.end());
    out = BytesRef(std::move(copy));
    upto_ += length;
    return {};
}

Result<int64_t> PagedBytes::copy_using_length_prefix(const BytesRef &bytes) {
    const size_t length = bytes.length();
    if (length >= MAX_ENTRY_LENGTH) {
        return err(LuceneError::illegal_argument("max length is 32767 (got " + std::to_string(length) + ")"));
    }
    if (upto_ + length + 2 > block_size_) {
        if (length + 2 > block_size_) {
            return err(LuceneError::illegal_argument("block size " + std::to_string(block_size_) +
                                                     " is too small to store length " + std::to_string(length) +
                                                     " bytes"));
        }
        if (current_page_ != nullptr) {
            pages_.push_back(current_page_);
            current_page_ = nullptr;
        }
        if (auto started = start_new_block(); !started.has_value()) {
            return std::unexpected(std::move(started.error()));
        }
    }

    const int64_t pointer = get_pointer();
    uint8_t *page = current_page();
    if (length < 128) {
        page[upto_] = static_cast<uint8_t>(length);
        upto_ += 1;
    } else {
        const auto flagged = static_cast<uint16_t>(length | 0x8000U);
        page[upto_] = static_cast<uint8_t>(flagged >> 8);
        page[upto_ + 1] = static_cast<uint8_t>(flagged);
        upto_ += 2;
    }
    const std::span<const uint8_t> source = bytes.as_slice();
    std::copy(source.begin(), source.end(), page + static_cast<std::ptrdiff_t>(upto_));
    upto_ += length;
    return pointer;
}

Result<std::shared_ptr<const PagedBytesReader>> PagedBytes::freeze(bool trim) {
    if (frozen_) {
        return err(LuceneError::illegal_state("already frozen"));
    }
    if (did_skip_bytes_) {
        return err(LuceneError::illegal_state("cannot freeze when copy(BytesRef, BytesRef) was used"));
    }

    if (current_page_ != nullptr) {
        if (trim && upto_ < block_size_) {
            current_page_->resize(upto_);
        }
        pages_.push_back(current_page_);
        current_page_ = nullptr;
    } else {
        pages_.push_back(std::make_shared<const std::vector<uint8_t>>());
    }

    frozen_ = true;
    auto frozen_pages = std::make_shared<const std::vector<std::shared_ptr<const std::vector<uint8_t>>>>(pages_);
    reader_ = std::shared_ptr<const PagedBytesReader>(new PagedBytesReader(std::move(frozen_pages), block_bits_));
    return reader_;
}

int64_t PagedBytes::get_pointer() const noexcept {
    if (current_page_ == nullptr) {
        return 0;
    }
    return static_cast<int64_t>(pages_.size()) * static_cast<int64_t>(block_size_) + static_cast<int64_t>(upto_);
}

int64_t PagedBytes::approx_ram_bytes() const noexcept {
    int64_t bytes = static_cast<int64_t>(pages_.capacity() * sizeof(std::shared_ptr<const std::vector<uint8_t>>));
    bytes += static_cast<int64_t>(pages_.size()) * static_cast<int64_t>(block_size_);
    if (current_page_ != nullptr) {
        bytes += static_cast<int64_t>(current_page_->capacity());
    }
    return bytes;
}

std::string PagedBytes::to_string() const {
    return "PagedBytes(blocksize=" + std::to_string(block_size_) + ")";
}

// ---------------- PagedBytesReader ----------------

PagedBytesReader::PagedBytesReader(
        std::shared_ptr<const std::vector<std::shared_ptr<const std::vector<uint8_t>>>> pages, size_t block_bits)
    : pages_(std::move(pages)),
      block_bits_(block_bits),
      block_size_(size_t{1} << block_bits),
      block_mask_(block_size_ - 1) {}

VoidResult PagedBytesReader::fill_slice(size_t start, size_t length, BytesRef &out) const {
    if (length == 0) {
        out = BytesRef();
        return {};
    }
    const size_t index = start >> block_bits_;
    const size_t offset = start & block_mask_;
    if (index >= pages_->size()) {
        return err(LuceneError::array_index_out_of_bounds("page " + std::to_string(index) + " >= " +
                                                         std::to_string(pages_->size())));
    }
    const std::vector<uint8_t> &page = *(*pages_)[index];
    if (offset + length <= page.size()) {
        std::vector<uint8_t> copy(page.begin() + static_cast<std::ptrdiff_t>(offset),
                                  page.begin() + static_cast<std::ptrdiff_t>(offset + length));
        out = BytesRef(std::move(copy));
        return {};
    }

    // The slice straddles one page boundary.
    if (index + 1 >= pages_->size()) {
        return err(LuceneError::eof("the slice runs past the last page"));
    }
    const size_t first_length = block_size_ - offset;
    if (first_length > page.size()) {
        return err(LuceneError::corrupt_index("the page is shorter than the block it belongs to"));
    }
    const std::vector<uint8_t> &next = *(*pages_)[index + 1];
    const size_t second_length = length - first_length;
    if (second_length > next.size()) {
        return err(LuceneError::eof("the slice runs past the last page"));
    }

    std::vector<uint8_t> copy;
    copy.reserve(length);
    copy.insert(copy.end(), page.begin() + static_cast<std::ptrdiff_t>(offset), page.end());
    copy.insert(copy.end(), next.begin(), next.begin() + static_cast<std::ptrdiff_t>(second_length));
    out = BytesRef(std::move(copy));
    return {};
}

Result<uint8_t> PagedBytesReader::get_byte(size_t offset) const {
    const size_t index = offset >> block_bits_;
    const size_t inside = offset & block_mask_;
    if (index >= pages_->size()) {
        return err(LuceneError::array_index_out_of_bounds("page " + std::to_string(index) + " >= " +
                                                         std::to_string(pages_->size())));
    }
    const std::vector<uint8_t> &page = *(*pages_)[index];
    if (inside >= page.size()) {
        return err(LuceneError::array_index_out_of_bounds("offset " + std::to_string(inside) + " >= " +
                                                         std::to_string(page.size())));
    }
    return page[inside];
}

VoidResult PagedBytesReader::fill(size_t start, BytesRef &out) const {
    const size_t index = start >> block_bits_;
    const size_t offset = start & block_mask_;
    if (index >= pages_->size()) {
        return err(LuceneError::array_index_out_of_bounds("page " + std::to_string(index) + " >= " +
                                                         std::to_string(pages_->size())));
    }
    const std::vector<uint8_t> &page = *(*pages_)[index];
    if (offset >= page.size()) {
        return err(LuceneError::eof("the entry runs past the last page"));
    }

    size_t length = 0;
    size_t prefix = 0;
    if ((page[offset] & 128) == 0) {
        length = page[offset];
        prefix = 1;
    } else {
        if (offset + 1 >= page.size()) {
            return err(LuceneError::eof("the length prefix runs past the last page"));
        }
        const auto flagged = static_cast<uint16_t>((static_cast<uint16_t>(page[offset]) << 8) | page[offset + 1]);
        length = static_cast<size_t>(flagged & 0x7FFF);
        prefix = 2;
    }
    return fill_slice(start + prefix, length, out);
}

std::string PagedBytesReader::to_string() const {
    return "PagedBytes(blocksize=" + std::to_string(block_size_) + ")";
}

// ---------------- data input / output ----------------

PagedBytesDataInput::PagedBytesDataInput(std::shared_ptr<const PagedBytesReader> reader)
    : reader_(std::move(reader)) {}

Result<uint8_t> PagedBytesDataInput::read_byte() {
    auto byte = reader_->get_byte(position_);
    if (!byte.has_value()) {
        return err(LuceneError::eof("read past the end of the paged bytes at " + std::to_string(position_)));
    }
    ++position_;
    return byte;
}

VoidResult PagedBytesDataInput::read_bytes(std::span<uint8_t> b) {
    BytesRef slice;
    if (auto filled = reader_->fill_slice(position_, b.size(), slice); !filled.has_value()) {
        return err(LuceneError::eof("read past the end of the paged bytes at " + std::to_string(position_)));
    }
    std::copy(slice.as_slice().begin(), slice.as_slice().end(), b.begin());
    position_ += b.size();
    return {};
}

VoidResult PagedBytesDataInput::skip_bytes(int64_t num_bytes) {
    if (num_bytes < 0) {
        return err(LuceneError::illegal_argument("cannot skip a negative number of bytes"));
    }
    position_ += static_cast<size_t>(num_bytes);
    return {};
}

size_t PagedBytesDataInput::get_position() const noexcept { return position_; }

VoidResult PagedBytesDataInput::set_position(size_t position) {
    position_ = position;
    return {};
}

VoidResult PagedBytesDataOutput::write_byte(uint8_t b) {
    const BytesRef bytes(std::vector<uint8_t>{b});
    BytesRef copied;
    return paged_bytes_->copy_with_bytes_ref(bytes, copied);
}

VoidResult PagedBytesDataOutput::write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) {
    if (offset + length > b.size()) {
        return err(LuceneError::illegal_argument("the write runs past the end of the buffer"));
    }
    const BytesRef bytes(std::vector<uint8_t>(b.begin() + static_cast<std::ptrdiff_t>(offset),
                                              b.begin() + static_cast<std::ptrdiff_t>(offset + length)));
    BytesRef copied;
    return paged_bytes_->copy_with_bytes_ref(bytes, copied);
}

Result<std::unique_ptr<DataInput>> get_paged_bytes_data_input(PagedBytes &paged_bytes) {
    const std::shared_ptr<const PagedBytesReader> &reader = paged_bytes.reader();
    if (reader == nullptr) {
        return err(LuceneError::illegal_state("the paged bytes must be frozen to be read"));
    }
    return std::unique_ptr<DataInput>(new PagedBytesDataInput(reader));
}

std::unique_ptr<DataOutput> get_paged_bytes_data_output(PagedBytes &paged_bytes) {
    return std::unique_ptr<DataOutput>(new PagedBytesDataOutput(paged_bytes));
}

} // namespace lucene
