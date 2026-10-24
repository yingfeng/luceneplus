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
#include <string_view>
#include <vector>

module lucene_core:ut.multi_reader;

import lucene_core;

using namespace lucene;

namespace {

/// A [`Bits`] over a plain vector, so a test can script the live documents.
class VectorBits final : public Bits {
public:
    explicit VectorBits(std::vector<bool> bits) : bits_(std::move(bits)) {}

    [[nodiscard]] Result<bool> get(size_t index) const override {
        if (index >= bits_.size()) {
            return std::unexpected(LuceneError::array_index_out_of_bounds("index out of range"));
        }
        return bits_[index];
    }

    [[nodiscard]] size_t length() const override { return bits_.size(); }
    [[nodiscard]] std::string to_string() const override { return "VectorBits"; }

private:
    std::vector<bool> bits_;
};

/// A leaf reader with the bare minimum needed by the composite readers.
class StubLeafReader final : public LeafReader {
public:
    StubLeafReader(int32_t max_doc, int32_t num_docs) : max_doc_(max_doc), num_docs_(num_docs) {}

    /// Records the live documents of this leaf; `max_doc_` must match its size.
    void set_live_docs(std::vector<bool> bits) { live_docs_ = std::make_unique<VectorBits>(std::move(bits)); }

    void set_close_error(std::optional<LuceneError> error) { close_error_ = std::move(error); }

    [[nodiscard]] int32_t closes() const noexcept { return closes_; }

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }
    [[nodiscard]] Result<int32_t> doc_freq(const Term & /*term*/) override { return doc_freq_; }
    [[nodiscard]] Result<int64_t> total_term_freq(const Term & /*term*/) override { return total_term_freq_; }
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view /*field*/) override { return sum_doc_freq_; }
    [[nodiscard]] Result<int32_t> doc_count(std::string_view /*field*/) override { return doc_count_; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view /*field*/) override {
        return sum_total_term_freq_;
    }
    [[nodiscard]] VoidResult close() const override {
        ++closes_;
        if (close_error_.has_value()) {
            return std::unexpected(close_error_.value());
        }
        return {};
    }

    // ---- LeafReader ----

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view /*field*/) override {
        return std::unique_ptr<Terms>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view) override {
        return std::unique_ptr<BinaryDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view) override {
        return std::unique_ptr<SortedDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view) override {
        return std::unique_ptr<SortedNumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(std::string_view) override {
        return std::unique_ptr<SortedSetDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view) override {
        return std::unique_ptr<NumericDocValues>{};
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_docs_.get(); }

private:
    int32_t max_doc_;
    int32_t num_docs_;
    int32_t doc_freq_{0};
    int64_t total_term_freq_{0};
    int64_t sum_doc_freq_{0};
    int32_t doc_count_{0};
    int64_t sum_total_term_freq_{0};
    std::unique_ptr<VectorBits> live_docs_{};
    std::optional<LuceneError> close_error_{};
    mutable int32_t closes_{0};
    FieldInfos field_infos_{FieldInfos::none()};
};

std::shared_ptr<StubLeafReader> make_leaf(int32_t max_doc, int32_t num_docs = -1) {
    if (num_docs < 0) {
        num_docs = max_doc;
    }
    return std::make_shared<StubLeafReader>(max_doc, num_docs);
}

std::shared_ptr<MultiReader> make_multi(const std::vector<std::shared_ptr<IndexReader>> &subs) {
    auto reader = MultiReader::create(subs);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    if (!reader.has_value()) {
        return nullptr;
    }
    return reader.value();
}

} // namespace

// ---------------------------------------------------------------- MultiBits

TEST(MultiBitsTest, LooksTheValuesUpInTheRightSubSet) {
    std::vector<std::shared_ptr<const Bits>> subs;
    subs.push_back(std::make_shared<VectorBits>(std::vector<bool>{true, false}));
    subs.emplace_back(); // no bits recorded for this reader
    subs.push_back(std::make_shared<VectorBits>(std::vector<bool>{false}));

    auto bits = MultiBits::create(subs, std::vector<int32_t>{0, 2, 3, 4}, true);
    ASSERT_TRUE(bits.has_value()) << (bits ? "" : bits.error().to_string());

    EXPECT_EQ(bits.value().length(), 4U);
    EXPECT_TRUE(bits.value().get(0).value());
    EXPECT_FALSE(bits.value().get(1).value());
    EXPECT_TRUE(bits.value().get(2).value()) << "a sub-set without bits falls back to the default";
    EXPECT_FALSE(bits.value().get(3).value());

    const std::string described = bits.value().to_string();
    EXPECT_NE(described.find("3 subs"), std::string::npos);
    EXPECT_NE(described.find("l=null"), std::string::npos);
    EXPECT_NE(described.find("end=4"), std::string::npos);
}

TEST(MultiBitsTest, TheDefaultValueAppliesToMissingSubSets) {
    std::vector<std::shared_ptr<const Bits>> subs;
    subs.emplace_back();
    subs.push_back(std::make_shared<VectorBits>(std::vector<bool>{true}));

    auto bits = MultiBits::create(subs, std::vector<int32_t>{0, 2, 3}, false);
    ASSERT_TRUE(bits.has_value()) << (bits ? "" : bits.error().to_string());

    EXPECT_FALSE(bits.value().default_value());
    EXPECT_FALSE(bits.value().get(0).value());
    EXPECT_FALSE(bits.value().get(1).value());
    EXPECT_TRUE(bits.value().get(2).value());
}

TEST(MultiBitsTest, RejectsOutOfRangeLookups) {
    auto bits = MultiBits::create({}, std::vector<int32_t>{0}, false);
    ASSERT_TRUE(bits.has_value()) << (bits ? "" : bits.error().to_string());

    auto value = bits.value().get(0);
    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().kind(), ErrorKind::kArrayIndexOutOfBounds);
    EXPECT_EQ(bits.value().length(), 0U);
}

TEST(MultiBitsTest, ValidatesTheShape) {
    auto wrong_size = MultiBits::create({}, std::vector<int32_t>{0, 1}, true);
    ASSERT_FALSE(wrong_size.has_value());
    EXPECT_EQ(wrong_size.error().kind(), ErrorKind::kIllegalArgument);

    std::vector<std::shared_ptr<const Bits>> subs;
    subs.push_back(std::make_shared<VectorBits>(std::vector<bool>{true}));
    auto decreasing = MultiBits::create(subs, std::vector<int32_t>{2, 1}, true);
    ASSERT_FALSE(decreasing.has_value());
    EXPECT_EQ(decreasing.error().kind(), ErrorKind::kIllegalArgument);
}

// ------------------------------------------------------------ live documents

TEST(MultiBitsTest, GetLiveDocsReturnsNullWithoutDeletions) {
    auto leaf = make_leaf(3);
    auto live = get_live_docs(*leaf);
    ASSERT_TRUE(live.has_value()) << (live ? "" : live.error().to_string());
    EXPECT_EQ(live.value(), nullptr);
}

TEST(MultiBitsTest, GetLiveDocsReturnsTheLeafBitsWhenItHasDeletions) {
    auto leaf = make_leaf(3, 2);
    leaf->set_live_docs({true, false, true});

    auto live = get_live_docs(*leaf);
    ASSERT_TRUE(live.has_value()) << (live ? "" : live.error().to_string());
    ASSERT_NE(live.value(), nullptr);
    EXPECT_TRUE(live.value()->get(0).value());
    EXPECT_FALSE(live.value()->get(1).value());
    EXPECT_TRUE(live.value()->get(2).value());
    EXPECT_EQ(live.value()->length(), 3U);
}

TEST(MultiBitsTest, GetLiveDocsMergesTheLeavesOfACompositeReader) {
    auto first = make_leaf(2, 1); // one deletion
    first->set_live_docs({true, false});
    auto second = make_leaf(3); // no deletions
    auto third = make_leaf(2, 1); // one deletion
    third->set_live_docs({false, true});

    auto reader = make_multi({first, second, third});
    ASSERT_NE(reader, nullptr);

    auto live = get_live_docs(*reader);
    ASSERT_TRUE(live.has_value()) << (live ? "" : live.error().to_string());
    ASSERT_NE(live.value(), nullptr);

    // 2 + 3 + 2 documents, bases 0, 2, 5.
    EXPECT_EQ(live.value()->length(), 7U);
    EXPECT_TRUE(live.value()->get(0).value());
    EXPECT_FALSE(live.value()->get(1).value());
    EXPECT_TRUE(live.value()->get(2).value()) << "a leaf without deletions is all live";
    EXPECT_TRUE(live.value()->get(3).value());
    EXPECT_TRUE(live.value()->get(4).value());
    EXPECT_FALSE(live.value()->get(5).value());
    EXPECT_TRUE(live.value()->get(6).value());
}

// --------------------------------------------------------------- MultiReader

TEST(MultiReaderTest, ConcatenatesTheSubReaders) {
    auto first = make_leaf(2);
    auto second = make_leaf(3);
    auto third = make_leaf(1);

    auto reader = make_multi({first, second, third});
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(reader->sub_readers().size(), 3U);
    EXPECT_EQ(reader->max_doc().value(), 6);
    EXPECT_EQ(reader->num_docs().value(), 6);
    EXPECT_FALSE(reader->has_deletions().value());
    EXPECT_EQ(reader->to_string(), "MultiReader(sub_readers=3, max_doc=6)");

    auto leaves = reader->leaves();
    ASSERT_TRUE(leaves.has_value()) << (leaves ? "" : leaves.error().to_string());
    const std::span<const LeafReaderContext> contexts = leaves.value();
    ASSERT_EQ(contexts.size(), 3U);
    EXPECT_EQ(contexts[0].doc_base(), 0);
    EXPECT_EQ(contexts[1].doc_base(), 2);
    EXPECT_EQ(contexts[2].doc_base(), 5);
    EXPECT_EQ(contexts[0].ord(), 0);
    EXPECT_EQ(contexts[1].ord(), 1);
    EXPECT_EQ(contexts[2].ord(), 2);
    EXPECT_EQ(contexts[0].reader(), first.get());
    EXPECT_EQ(contexts[2].reader(), third.get());

    // Every leaf of one tree shares the top-level metadata.
    EXPECT_EQ(contexts[0].top_parent().leaves_num, 3);
    EXPECT_EQ(contexts[0].top_parent().max_doc, 6);
    EXPECT_TRUE(contexts[2].top_parent().id.is_same(reader->identity()));
}

TEST(MultiReaderTest, FlattensNestedCompositeReaders) {
    auto a = make_leaf(2);
    auto b = make_leaf(3);
    auto c = make_leaf(1);
    auto inner = make_multi({a, b});
    ASSERT_NE(inner, nullptr);

    auto outer = make_multi({inner, c});
    ASSERT_NE(outer, nullptr);

    EXPECT_EQ(outer->max_doc().value(), 6);
    auto leaves = outer->leaves();
    ASSERT_TRUE(leaves.has_value()) << (leaves ? "" : leaves.error().to_string());
    const std::span<const LeafReaderContext> contexts = leaves.value();
    ASSERT_EQ(contexts.size(), 3U);
    EXPECT_EQ(contexts[0].doc_base(), 0);
    EXPECT_EQ(contexts[1].doc_base(), 2);
    EXPECT_EQ(contexts[2].doc_base(), 5) << "the inner reader's documents are offset by the outer base";
    EXPECT_EQ(contexts[0].ord(), 0);
    EXPECT_EQ(contexts[1].ord(), 1);
    EXPECT_EQ(contexts[2].ord(), 2);
    EXPECT_EQ(contexts[2].reader(), c.get());
    EXPECT_EQ(contexts[0].top_parent().leaves_num, 3);
    EXPECT_TRUE(contexts[0].top_parent().id.is_same(outer->identity()));
}

TEST(MultiReaderTest, AnEmptyReaderIsAnEmptyIndex) {
    auto reader = make_multi({});
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(reader->max_doc().value(), 0);
    EXPECT_EQ(reader->num_docs().value(), 0);
    auto leaves = reader->leaves();
    ASSERT_TRUE(leaves.has_value()) << (leaves ? "" : leaves.error().to_string());
    EXPECT_TRUE(leaves.value().empty());
    EXPECT_FALSE(reader->has_deletions().value());
}

TEST(MultiReaderTest, CountsTheDeletionsOfEverySubReader) {
    auto first = make_leaf(3, 2);
    auto second = make_leaf(4, 4);
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);

    EXPECT_EQ(reader->max_doc().value(), 7);
    EXPECT_EQ(reader->num_docs().value(), 6);
    EXPECT_EQ(reader->num_deleted_docs().value(), 1);
    EXPECT_TRUE(reader->has_deletions().value());
}

TEST(MultiReaderTest, RejectsNullSubReaders) {
    std::vector<std::shared_ptr<IndexReader>> subs;
    subs.emplace_back();
    auto reader = MultiReader::create(subs);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MultiReaderTest, ProvidesCursorsOverAllSubReaders) {
    auto reader = make_multi({make_leaf(2), make_leaf(3)});
    ASSERT_NE(reader, nullptr);

    auto stored = reader->stored_fields();
    ASSERT_TRUE(stored.has_value()) << (stored ? "" : stored.error().to_string());
    EXPECT_NE(stored.value(), nullptr);

    auto vectors = reader->term_vectors();
    ASSERT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
    EXPECT_NE(vectors.value(), nullptr);
}

TEST(MultiReaderTest, CloseClosesTheSubReaders) {
    auto first = make_leaf(1);
    auto second = make_leaf(1);
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);

    ASSERT_TRUE(reader->close().has_value());
    EXPECT_EQ(first->closes(), 1);
    EXPECT_EQ(second->closes(), 1);

    // Idempotent.
    ASSERT_TRUE(reader->close().has_value());
    EXPECT_EQ(first->closes(), 1);
}

TEST(MultiReaderTest, CloseCanLeaveTheSubReadersOpen) {
    auto first = make_leaf(1);
    auto second = make_leaf(1);
    auto reader = MultiReader::create_with_close_sub_readers(false, {first, second});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_FALSE(reader.value()->close_sub_readers());

    ASSERT_TRUE(reader.value()->close().has_value());
    EXPECT_EQ(first->closes(), 0);
    EXPECT_EQ(second->closes(), 0);
}

TEST(MultiReaderTest, CloseReportsTheFirstIoErrorButClosesEveryReader) {
    auto first = make_leaf(1);
    first->set_close_error(LuceneError::io("disk went away"));
    auto second = make_leaf(1);
    auto third = make_leaf(1);
    third->set_close_error(LuceneError::io("disk is still away"));
    auto reader = make_multi({first, second, third});
    ASSERT_NE(reader, nullptr);

    auto closed = reader->close();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().kind(), ErrorKind::kIo);
    EXPECT_EQ(closed.error().message(), "disk went away");
    EXPECT_EQ(first->closes(), 1);
    EXPECT_EQ(second->closes(), 1) << "an I/O error does not abort the loop";
    EXPECT_EQ(third->closes(), 1);
}

TEST(MultiReaderTest, CloseReturnsANonIoErrorImmediately) {
    auto first = make_leaf(1);
    first->set_close_error(LuceneError::illegal_state("already closed"));
    auto second = make_leaf(1);
    auto third = make_leaf(1);
    auto reader = make_multi({first, second, third});
    ASSERT_NE(reader, nullptr);

    auto closed = reader->close();
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().kind(), ErrorKind::kIllegalState);
    EXPECT_EQ(first->closes(), 1);
    EXPECT_EQ(second->closes(), 0) << "a misuse stops the loop";
    EXPECT_EQ(third->closes(), 0);
}
