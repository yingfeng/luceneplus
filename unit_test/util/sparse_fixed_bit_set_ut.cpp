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
#include <vector>

module lucene_core:ut.sparse_fixed_bit_set;

import lucene_core;

using namespace lucene;

namespace {

/// A [`DocIdSetIterator`] over an explicit list of documents.
class VectorDocIdSetIterator final : public DocIdSetIterator {
public:
    explicit VectorDocIdSetIterator(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        if (next_ >= docs_.size()) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        doc_ = docs_[next_++];
        return doc_;
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (true) {
            auto doc = next_doc();
            if (!doc.has_value() || doc.value() == NO_MORE_DOCS || doc.value() >= target) {
                return doc;
            }
        }
    }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    std::vector<int32_t> docs_;
    size_t next_{0};
    int32_t doc_{-1};
};

SparseFixedBitSet make(size_t length) {
    auto set = SparseFixedBitSet::create(length);
    EXPECT_TRUE(set.has_value()) << (set ? "" : set.error().to_string());
    return set.has_value() ? std::move(set.value()) : SparseFixedBitSet::create(1).value();
}

} // namespace

TEST(SparseFixedBitSetTest, RejectsAnEmptySet) {
    auto empty = SparseFixedBitSet::create(0);
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SparseFixedBitSetTest, SetsAndClearsBits) {
    SparseFixedBitSet set = make(100);
    EXPECT_EQ(set.length(), 100U);
    EXPECT_EQ(set.cardinality(), 0U);
    EXPECT_FALSE(set.get(0).value());
    EXPECT_FALSE(set.get(99).value());

    EXPECT_TRUE(set.set(3)) << "the first set returns true";
    EXPECT_FALSE(set.set(3)) << "setting it again returns false";
    EXPECT_TRUE(set.set(64));
    EXPECT_TRUE(set.set(99));
    EXPECT_EQ(set.cardinality(), 3U);
    EXPECT_TRUE(set.get(3).value());
    EXPECT_TRUE(set.get(64).value());
    EXPECT_TRUE(set.get(99).value());
    EXPECT_FALSE(set.get(4).value());

    EXPECT_TRUE(set.clear(64));
    EXPECT_FALSE(set.clear(64)) << "already clear";
    EXPECT_EQ(set.cardinality(), 2U);
    EXPECT_FALSE(set.get(64).value());

    auto out_of_bounds = set.get(100);
    ASSERT_FALSE(out_of_bounds.has_value());
    EXPECT_EQ(out_of_bounds.error().kind(), ErrorKind::kArrayIndexOutOfBounds);
    EXPECT_NE(set.to_string().find("cardinality=2"), std::string::npos);
}

TEST(SparseFixedBitSetTest, OnlyStoresTheWordsThatAreSet) {
    // A million bits with three documents is what this structure is for: three
    // words stored, not 15625.
    SparseFixedBitSet set = make(1'000'000);
    EXPECT_EQ(set.non_zero_long_count(), 0U);
    EXPECT_TRUE(set.set(0));
    EXPECT_TRUE(set.set(500'000));
    EXPECT_TRUE(set.set(999'999));
    EXPECT_EQ(set.non_zero_long_count(), 3U);
    EXPECT_EQ(set.cardinality(), 3U);
    EXPECT_GT(set.approx_ram_bytes(), 0);

    // Two documents in the same word share it.
    EXPECT_TRUE(set.set(999'998));
    EXPECT_EQ(set.non_zero_long_count(), 3U);
    EXPECT_EQ(set.cardinality(), 4U);

    EXPECT_TRUE(set.clear(999'998));
    EXPECT_EQ(set.non_zero_long_count(), 3U);
    EXPECT_TRUE(set.clear(999'999));
    EXPECT_EQ(set.non_zero_long_count(), 2U) << "an emptied word is dropped";
}

TEST(SparseFixedBitSetTest, NextSetBitWalksAcrossWordsAndBlocks) {
    SparseFixedBitSet set = make(20'000);
    EXPECT_EQ(set.next_set_bit(0), -1) << "an empty set has no set bit";

    set.set(5);
    set.set(64);
    set.set(4096);
    set.set(19'999);

    EXPECT_EQ(set.next_set_bit(0), 5);
    EXPECT_EQ(set.next_set_bit(5), 5);
    EXPECT_EQ(set.next_set_bit(6), 64);
    EXPECT_EQ(set.next_set_bit(65), 4096) << "the next block";
    EXPECT_EQ(set.next_set_bit(4097), 19'999);
    EXPECT_EQ(set.next_set_bit(20'000), -1) << "past the end";
    EXPECT_EQ(set.next_set_bit(19'999), 19'999);
}

TEST(SparseFixedBitSetTest, OrFromIteratorAddsEveryDocument) {
    SparseFixedBitSet set = make(1000);
    VectorDocIdSetIterator docs({1, 2, 700, 999});

    ASSERT_TRUE(set.or_from_iterator(docs).has_value());
    EXPECT_EQ(set.cardinality(), 4U);
    EXPECT_TRUE(set.get(1).value());
    EXPECT_TRUE(set.get(700).value());
    EXPECT_TRUE(set.get(999).value());
    EXPECT_EQ(set.next_set_bit(0), 1);
}

TEST(SparseFixedBitSetTest, OrFromIteratorRejectsOutOfRangeDocuments) {
    SparseFixedBitSet set = make(10);
    VectorDocIdSetIterator docs({3, 10});

    auto loaded = set.or_from_iterator(docs);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SparseFixedBitSetTest, ManyDocumentsStayConsistent) {
    // Every 4096th bit is a whole-word boundary case; cover a couple of blocks.
    SparseFixedBitSet set = make(3 * 4096);
    for (size_t doc = 0; doc < set.length(); doc += 37) {
        set.set(doc);
    }
    const size_t expected = (set.length() + 36) / 37;
    EXPECT_EQ(set.cardinality(), expected);

    size_t counted = 0;
    for (int64_t doc = set.next_set_bit(0); doc >= 0; doc = set.next_set_bit(static_cast<size_t>(doc) + 1)) {
        ++counted;
    }
    EXPECT_EQ(counted, expected) << "the walk must find exactly the set bits";
}
