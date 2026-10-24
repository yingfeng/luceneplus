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

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.paged_bytes;

import lucene_core;

using namespace lucene;

namespace {

/// An [`IndexInput`] over an in-memory buffer.
class VectorIndexInput final : public IndexInput {
public:
    explicit VectorIndexInput(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (position_ >= bytes_.size()) {
            return err(LuceneError::eof("read past the end"));
        }
        return bytes_[position_++];
    }
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (position_ + b.size() > bytes_.size()) {
            return err(LuceneError::eof("read past the end"));
        }
        std::copy(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                  bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + b.size()), b.begin());
        position_ += b.size();
        return {};
    }
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override {
        position_ += static_cast<size_t>(num_bytes);
        return {};
    }
    [[nodiscard]] Result<size_t> length() const override { return bytes_.size(); }
    [[nodiscard]] Result<size_t> get_file_pointer() const override { return position_; }
    [[nodiscard]] VoidResult seek(size_t pos) override {
        position_ = pos;
        return {};
    }
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> clone() const override {
        auto copy = std::make_unique<VectorIndexInput>(bytes_);
        copy->position_ = position_;
        return std::unique_ptr<IndexInput>(std::move(copy));
    }
    [[nodiscard]] std::string to_string() const override { return "VectorIndexInput"; }

private:
    std::vector<uint8_t> bytes_;
    size_t position_{0};
};

std::vector<uint8_t> bytes_of(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string text_of(const BytesRef &ref) {
    auto text = ref.utf8_to_string();
    return text.has_value() ? text.value() : std::string();
}

} // namespace

TEST(PagedBytesTest, AppendsAcrossPagesAndReadsBack) {
    PagedBytes paged(4); // 16 byte pages
    EXPECT_EQ(paged.block_size(), 16U);
    EXPECT_EQ(paged.get_pointer(), 0);

    const std::string text = "hello world, this is thirty-something bytes";
    VectorIndexInput input(bytes_of(text));
    ASSERT_TRUE(paged.copy_with_input(input, text.size()).has_value());
    EXPECT_EQ(paged.get_pointer(), static_cast<int64_t>(text.size()));

    auto reader = paged.freeze(true);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_TRUE(paged.frozen());
    EXPECT_EQ(reader.value()->block_size(), 16U);
    EXPECT_EQ(reader.value()->page_count(), 3U) << text.size() << " bytes need three 16 byte pages";

    BytesRef slice;
    ASSERT_TRUE(reader.value()->fill_slice(0, 5, slice).has_value());
    EXPECT_EQ(text_of(slice), text.substr(0, 5));

    // A slice that straddles a page boundary (14..18 crosses byte 16).
    ASSERT_TRUE(reader.value()->fill_slice(14, 5, slice).has_value());
    EXPECT_EQ(text_of(slice), text.substr(14, 5));

    EXPECT_EQ(reader.value()->get_byte(0).value(), static_cast<uint8_t>(text[0]));
    EXPECT_EQ(reader.value()->get_byte(text.size() - 1).value(), static_cast<uint8_t>(text.back()));
}

TEST(PagedBytesTest, FreezingIsOneShot) {
    PagedBytes paged(6);
    VectorIndexInput input(bytes_of("abc"));
    ASSERT_TRUE(paged.copy_with_input(input, 3).has_value());
    ASSERT_TRUE(paged.freeze(true).has_value());

    auto again = paged.freeze(true);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().kind(), ErrorKind::kIllegalState);
}

TEST(PagedBytesTest, LengthPrefixedEntriesRoundTrip) {
    PagedBytes paged(8); // 256 byte pages
    const std::vector<std::string> entries{"a", "hello world", std::string(200, 'x')};
    std::vector<int64_t> pointers;
    for (const std::string &entry : entries) {
        const BytesRef bytes(bytes_of(entry));
        auto pointer = paged.copy_using_length_prefix(bytes);
        ASSERT_TRUE(pointer.has_value()) << (pointer ? "" : pointer.error().to_string());
        pointers.push_back(pointer.value());
    }

    auto reader = paged.freeze(true);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    for (size_t i = 0; i < entries.size(); ++i) {
        BytesRef filled;
        ASSERT_TRUE(reader.value()->fill(static_cast<size_t>(pointers[i]), filled).has_value())
                << "entry " << i;
        EXPECT_EQ(text_of(filled), entries[i]) << "entry " << i;
    }
}

TEST(PagedBytesTest, RejectsEntriesThatAreTooLong) {
    PagedBytes paged(8);
    const BytesRef too_long(std::vector<uint8_t>(32768, 7));
    auto pointer = paged.copy_using_length_prefix(too_long);
    ASSERT_FALSE(pointer.has_value());
    EXPECT_EQ(pointer.error().kind(), ErrorKind::kIllegalArgument);

    PagedBytes small(2); // 4 byte pages
    const BytesRef entry(std::vector<uint8_t>(5, 1));
    auto too_big = small.copy_using_length_prefix(entry);
    ASSERT_FALSE(too_big.has_value());
    EXPECT_EQ(too_big.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(PagedBytesTest, CopyingABytesRefInvalidatesFreezing) {
    PagedBytes paged(4);
    const BytesRef first(bytes_of("0123456789abcdef")); // fills the page exactly
    BytesRef copied;
    ASSERT_TRUE(paged.copy_with_bytes_ref(first, copied).has_value());
    EXPECT_EQ(text_of(copied), "0123456789abcdef");

    // Starting a new page this way means pointers were handed out, so freezing
    // must refuse: the pages would move.
    ASSERT_TRUE(paged.copy_with_bytes_ref(first, copied).has_value());
    auto reader = paged.freeze(true);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kIllegalState);
}

TEST(PagedBytesTest, DataInputAndOutputWalkThePages) {
    PagedBytes paged(4); // 16 byte pages
    auto output = get_paged_bytes_data_output(paged);
    ASSERT_NE(output, nullptr);
    auto wrapped = dynamic_cast<PagedBytesDataOutput *>(output.get());
    ASSERT_NE(wrapped, nullptr);
    EXPECT_EQ(wrapped->position(), 0);

    ASSERT_TRUE(output->write_byte('a').has_value());
    ASSERT_TRUE(output->write_bytes(std::span<const uint8_t>(bytes_of("bcdefghij"))).has_value());
    EXPECT_EQ(wrapped->position(), 10);

    auto reader = paged.freeze(true);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    auto input = get_paged_bytes_data_input(paged);
    ASSERT_TRUE(input.has_value()) << (input ? "" : input.error().to_string());

    EXPECT_EQ(input.value()->read_byte().value(), 'a');
    uint8_t rest[9] = {};
    ASSERT_TRUE(input.value()->read_bytes(std::span<uint8_t>(rest, 9)).has_value());
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(rest), 9), "bcdefghij");

    // Reading past the end is an EOF, not a crash.
    auto past_end = input.value()->read_byte();
    ASSERT_FALSE(past_end.has_value());
    EXPECT_EQ(past_end.error().kind(), ErrorKind::kEof);

    auto unfrozen = PagedBytes(4);
    auto rejected = get_paged_bytes_data_input(unfrozen);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalState);
}

TEST(PagedBytesTest, ASingleEmptyFreezeStillHasOnePage) {
    PagedBytes paged(4);
    auto reader = paged.freeze(true);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->page_count(), 1U);
    EXPECT_EQ(reader.value()->to_string(), "PagedBytes(blocksize=16)");
    EXPECT_EQ(paged.to_string(), "PagedBytes(blocksize=16)");
    EXPECT_GT(paged.approx_ram_bytes(), 0);
}
