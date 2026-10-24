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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.multi_doc_values;

import lucene_core;

using namespace lucene;

namespace {

// ---- per-document data of the stub leaves ----

struct NumericData {
    std::vector<std::optional<int64_t>> values;
};

struct BinaryData {
    std::vector<std::optional<std::string>> values;
};

struct SortedNumericData {
    std::vector<std::vector<int64_t>> values;
};

struct SortedData {
    /// `-1` marks a document without a value.
    std::vector<int32_t> ords;
    /// Already sorted, as the codecs guarantee.
    std::vector<std::string> dictionary;
};

struct SortedSetData {
    std::vector<std::vector<int32_t>> ords;
    std::vector<std::string> dictionary;
};

/// Marks the documents that have a value.
std::vector<bool> present_of(const std::vector<std::optional<int64_t>> &values) {
    std::vector<bool> present;
    present.reserve(values.size());
    for (const auto &value : values) {
        present.push_back(value.has_value());
    }
    return present;
}

Result<int32_t> next_present(const std::vector<bool> &present, int32_t &doc) {
    for (int32_t candidate = doc + 1; candidate < static_cast<int32_t>(present.size()); ++candidate) {
        if (present[static_cast<size_t>(candidate)]) {
            doc = candidate;
            return doc;
        }
    }
    doc = NO_MORE_DOCS;
    return doc;
}

Result<int32_t> advance_present(const std::vector<bool> &present, int32_t &doc, int32_t target) {
    for (int32_t candidate = std::max(doc + 1, target); candidate < static_cast<int32_t>(present.size());
         ++candidate) {
        if (present[static_cast<size_t>(candidate)]) {
            doc = candidate;
            return doc;
        }
    }
    doc = NO_MORE_DOCS;
    return doc;
}

int64_t cost_of(const std::vector<bool> &present) {
    int64_t cost = 0;
    for (const bool bit : present) {
        cost += bit ? 1 : 0;
    }
    return cost;
}

// ---- cursors over the data above ----

class VectorNumericDocValues final : public NumericDocValues {
public:
    explicit VectorNumericDocValues(std::shared_ptr<const NumericData> data) : data_(std::move(data)) {
        present_ = present_of(data_->values);
        cost_ = cost_of(present_);
    }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return next_present(present_, doc_); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        return advance_present(present_, doc_, target);
    }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }
    [[nodiscard]] Result<int64_t> long_value() override {
        if (doc_ < 0 || doc_ >= static_cast<int32_t>(data_->values.size()) ||
            !data_->values[static_cast<size_t>(doc_)].has_value()) {
            return std::unexpected(LuceneError::illegal_state("no value for this document"));
        }
        return data_->values[static_cast<size_t>(doc_)].value();
    }

private:
    std::shared_ptr<const NumericData> data_;
    std::vector<bool> present_;
    int32_t doc_{-1};
    int64_t cost_{0};
};

class VectorBinaryDocValues final : public BinaryDocValues {
public:
    explicit VectorBinaryDocValues(std::shared_ptr<const BinaryData> data) : data_(std::move(data)) {
        present_.reserve(data_->values.size());
        for (const auto &value : data_->values) {
            present_.push_back(value.has_value());
        }
        cost_ = cost_of(present_);
    }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return next_present(present_, doc_); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        return advance_present(present_, doc_, target);
    }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }
    [[nodiscard]] Result<BytesRef> binary_value() override {
        if (doc_ < 0 || !data_->values[static_cast<size_t>(doc_)].has_value()) {
            return std::unexpected(LuceneError::illegal_state("no value for this document"));
        }
        return BytesRef::from_string(data_->values[static_cast<size_t>(doc_)].value());
    }

private:
    std::shared_ptr<const BinaryData> data_;
    std::vector<bool> present_;
    int32_t doc_{-1};
    int64_t cost_{0};
};

class VectorSortedNumericDocValues final : public SortedNumericDocValues {
public:
    explicit VectorSortedNumericDocValues(std::shared_ptr<const SortedNumericData> data) : data_(std::move(data)) {
        for (const auto &values : data_->values) {
            present_.push_back(!values.empty());
        }
        cost_ = cost_of(present_);
    }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        value_cursor_ = 0;
        return next_present(present_, doc_);
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        value_cursor_ = 0;
        return advance_present(present_, doc_, target);
    }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        value_cursor_ = 0;
        doc_ = target;
        return target >= 0 && target < static_cast<int32_t>(present_.size()) &&
               present_[static_cast<size_t>(target)];
    }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }
    [[nodiscard]] Result<int64_t> next_value() override {
        const std::vector<int64_t> &values = data_->values[static_cast<size_t>(doc_)];
        if (value_cursor_ >= values.size()) {
            return std::unexpected(LuceneError::illegal_state("next_value() past the last value"));
        }
        return values[value_cursor_++];
    }
    [[nodiscard]] Result<int32_t> doc_value_count() override {
        return static_cast<int32_t>(data_->values[static_cast<size_t>(doc_)].size());
    }

private:
    std::shared_ptr<const SortedNumericData> data_;
    std::vector<bool> present_;
    int32_t doc_{-1};
    size_t value_cursor_{0};
    int64_t cost_{0};
};

class VectorSortedDocValues final : public SortedDocValues {
public:
    explicit VectorSortedDocValues(std::shared_ptr<const SortedData> data) : data_(std::move(data)) {
        for (const int32_t ord : data_->ords) {
            present_.push_back(ord >= 0);
        }
        cost_ = cost_of(present_);
    }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return next_present(present_, doc_); }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        return advance_present(present_, doc_, target);
    }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return target >= 0 && target < static_cast<int32_t>(present_.size()) &&
               present_[static_cast<size_t>(target)];
    }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }

    [[nodiscard]] Result<int32_t> ord_value() override {
        if (doc_ < 0 || doc_ >= static_cast<int32_t>(data_->ords.size())) {
            return std::unexpected(LuceneError::illegal_state("no current document"));
        }
        return data_->ords[static_cast<size_t>(doc_)];
    }
    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t ord) override {
        if (ord < 0 || ord >= static_cast<int32_t>(data_->dictionary.size())) {
            return std::unexpected(LuceneError::illegal_argument("ordinal out of range"));
        }
        return BytesRef::from_string(data_->dictionary[static_cast<size_t>(ord)]);
    }
    [[nodiscard]] Result<int32_t> get_value_count() const override {
        return static_cast<int32_t>(data_->dictionary.size());
    }

private:
    std::shared_ptr<const SortedData> data_;
    std::vector<bool> present_;
    int32_t doc_{-1};
    int64_t cost_{0};
};

class VectorSortedSetDocValues final : public SortedSetDocValues {
public:
    explicit VectorSortedSetDocValues(std::shared_ptr<const SortedSetData> data) : data_(std::move(data)) {
        for (const auto &ords : data_->ords) {
            present_.push_back(!ords.empty());
        }
        cost_ = cost_of(present_);
    }

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        ord_cursor_ = 0;
        return next_present(present_, doc_);
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        ord_cursor_ = 0;
        return advance_present(present_, doc_, target);
    }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        ord_cursor_ = 0;
        doc_ = target;
        return target >= 0 && target < static_cast<int32_t>(present_.size()) &&
               present_[static_cast<size_t>(target)];
    }
    [[nodiscard]] Result<int64_t> cost() const override { return cost_; }

    [[nodiscard]] Result<int64_t> next_ord() override {
        const std::vector<int32_t> &ords = data_->ords[static_cast<size_t>(doc_)];
        if (ord_cursor_ >= ords.size()) {
            return NO_MORE_ORDS;
        }
        return static_cast<int64_t>(ords[ord_cursor_++]);
    }
    [[nodiscard]] Result<int32_t> doc_value_count() override {
        return static_cast<int32_t>(data_->ords[static_cast<size_t>(doc_)].size());
    }
    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t ord) override {
        if (ord < 0 || ord >= static_cast<int64_t>(data_->dictionary.size())) {
            return std::unexpected(LuceneError::illegal_argument("ordinal out of range"));
        }
        return BytesRef::from_string(data_->dictionary[static_cast<size_t>(ord)]);
    }
    [[nodiscard]] Result<int64_t> get_value_count() const override {
        return static_cast<int64_t>(data_->dictionary.size());
    }

private:
    std::shared_ptr<const SortedSetData> data_;
    std::vector<bool> present_;
    int32_t doc_{-1};
    size_t ord_cursor_{0};
    int64_t cost_{0};
};

/// A leaf reader handing out the doc values above.
class StubLeafReader final : public LeafReader {
public:
    StubLeafReader(int32_t max_doc) : max_doc_(max_doc), num_docs_(max_doc) {}

    void set_numeric(std::vector<std::optional<int64_t>> values) {
        numeric_ = std::make_shared<NumericData>(NumericData{std::move(values)});
        num_docs_ = max_doc_;
    }
    void set_norms(std::vector<std::optional<int64_t>> values) {
        norms_ = std::make_shared<NumericData>(NumericData{std::move(values)});
    }
    void set_binary(std::vector<std::optional<std::string>> values) {
        binary_ = std::make_shared<BinaryData>(BinaryData{std::move(values)});
    }
    void set_sorted_numeric(std::vector<std::vector<int64_t>> values) {
        sorted_numeric_ = std::make_shared<SortedNumericData>(SortedNumericData{std::move(values)});
    }
    void set_sorted(std::vector<int32_t> ords, std::vector<std::string> dictionary) {
        sorted_ = std::make_shared<SortedData>(SortedData{std::move(ords), std::move(dictionary)});
    }
    void set_sorted_set(std::vector<std::vector<int32_t>> ords, std::vector<std::string> dictionary) {
        sorted_set_ = std::make_shared<SortedSetData>(SortedSetData{std::move(ords), std::move(dictionary)});
    }

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }

    // ---- LeafReader ----

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view) override {
        return std::unique_ptr<Terms>{};
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view) override {
        if (numeric_ == nullptr) {
            return std::unique_ptr<NumericDocValues>{};
        }
        return std::unique_ptr<NumericDocValues>(new VectorNumericDocValues(numeric_));
    }
    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary_doc_values(std::string_view) override {
        if (binary_ == nullptr) {
            return std::unique_ptr<BinaryDocValues>{};
        }
        return std::unique_ptr<BinaryDocValues>(new VectorBinaryDocValues(binary_));
    }
    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted_doc_values(std::string_view) override {
        if (sorted_ == nullptr) {
            return std::unique_ptr<SortedDocValues>{};
        }
        return std::unique_ptr<SortedDocValues>(new VectorSortedDocValues(sorted_));
    }
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric_doc_values(
            std::string_view) override {
        if (sorted_numeric_ == nullptr) {
            return std::unique_ptr<SortedNumericDocValues>{};
        }
        return std::unique_ptr<SortedNumericDocValues>(new VectorSortedNumericDocValues(sorted_numeric_));
    }
    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set_doc_values(std::string_view) override {
        if (sorted_set_ == nullptr) {
            return std::unique_ptr<SortedSetDocValues>{};
        }
        return std::unique_ptr<SortedSetDocValues>(new VectorSortedSetDocValues(sorted_set_));
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view) override {
        if (norms_ == nullptr) {
            return std::unique_ptr<NumericDocValues>{};
        }
        return std::unique_ptr<NumericDocValues>(new VectorNumericDocValues(norms_));
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return nullptr; }

private:
    int32_t max_doc_;
    int32_t num_docs_;
    std::shared_ptr<const NumericData> numeric_;
    std::shared_ptr<const NumericData> norms_;
    std::shared_ptr<const BinaryData> binary_;
    std::shared_ptr<const SortedNumericData> sorted_numeric_;
    std::shared_ptr<const SortedData> sorted_;
    std::shared_ptr<const SortedSetData> sorted_set_;
    FieldInfos field_infos_{FieldInfos::none()};
};

std::shared_ptr<MultiReader> make_multi(const std::vector<std::shared_ptr<IndexReader>> &subs) {
    auto reader = MultiReader::create(subs);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    if (!reader.has_value()) {
        return nullptr;
    }
    return reader.value();
}

std::string text_of(const BytesRef &bytes) {
    auto text = bytes.utf8_to_string();
    return text.has_value() ? text.value() : std::string("<invalid utf8>");
}

} // namespace

// ------------------------------------------------------- GlobalOrdinalMap

TEST(GlobalOrdinalMapTest, MergeSortsTheSegmentDictionaries) {
    std::vector<std::vector<BytesRef>> dictionaries;
    dictionaries.push_back({BytesRef::from_string("a"), BytesRef::from_string("c")});
    dictionaries.push_back({BytesRef::from_string("b"), BytesRef::from_string("c")});

    auto map = GlobalOrdinalMap::build(std::move(dictionaries));
    ASSERT_TRUE(map.has_value()) << (map ? "" : map.error().to_string());

    EXPECT_EQ(map.value().value_count(), 3);
    ASSERT_NE(map.value().term(0), nullptr);
    EXPECT_EQ(text_of(*map.value().term(0)), "a");
    EXPECT_EQ(text_of(*map.value().term(1)), "b");
    EXPECT_EQ(text_of(*map.value().term(2)), "c");
    EXPECT_EQ(map.value().term(3), nullptr);

    EXPECT_EQ(map.value().global_ord(0, 0), 0);
    EXPECT_EQ(map.value().global_ord(0, 1), 2);
    EXPECT_EQ(map.value().global_ord(1, 0), 1);
    EXPECT_EQ(map.value().global_ord(1, 1), 2);
    EXPECT_EQ(map.value().global_ord(0, 5), -1) << "out of range segment ordinals map to -1";

    // "c" first appears in segment 0, at ordinal 1.
    auto first = map.value().first_segment_of(2);
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    EXPECT_EQ(first.value().first, 0U);
    EXPECT_EQ(first.value().second, 1);

    auto missing = map.value().first_segment_of(3);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(GlobalOrdinalMapTest, RejectsAnUnsortedDictionary) {
    std::vector<std::vector<BytesRef>> dictionaries;
    dictionaries.push_back({BytesRef::from_string("b"), BytesRef::from_string("a")});

    auto map = GlobalOrdinalMap::build(std::move(dictionaries));
    ASSERT_FALSE(map.has_value());
    EXPECT_EQ(map.error().kind(), ErrorKind::kIllegalArgument);
}

// ---------------------------------------------------------------- numeric

TEST(MultiDocValuesTest, NumericValuesAreMergedOverTheLeaves) {
    auto first = std::make_shared<StubLeafReader>(2);
    first->set_numeric({5, std::nullopt});
    auto second = std::make_shared<StubLeafReader>(2);
    second->set_numeric({std::nullopt, 7});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);

    auto values = MultiDocValues::get_numeric_values(*reader, "n");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_NE(dynamic_cast<MultiNumericDocValues *>(values.value().get()), nullptr);

    EXPECT_EQ(values.value()->next_doc().value(), 0);
    EXPECT_EQ(values.value()->long_value().value(), 5);
    // The leaf-0 document 1 has no value; the next one is leaf 1's doc 0.
    EXPECT_EQ(values.value()->next_doc().value(), 3);
    EXPECT_EQ(values.value()->long_value().value(), 7);
    EXPECT_EQ(values.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(values.value()->cost().value(), 0);
}

TEST(MultiDocValuesTest, NumericAdvanceSkipsLeaves) {
    auto first = std::make_shared<StubLeafReader>(2);
    first->set_numeric({5, std::nullopt});
    auto second = std::make_shared<StubLeafReader>(2);
    second->set_numeric({std::nullopt, 7});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);
    auto values = MultiDocValues::get_numeric_values(*reader, "n");
    ASSERT_TRUE(values.has_value());

    EXPECT_EQ(values.value()->advance(1).value(), 3);
    EXPECT_EQ(values.value()->long_value().value(), 7);

    auto backwards = values.value()->advance(3);
    ASSERT_FALSE(backwards.has_value());
    EXPECT_EQ(backwards.error().kind(), ErrorKind::kIllegalArgument);

    EXPECT_EQ(values.value()->advance(4).value(), NO_MORE_DOCS);
}

TEST(MultiDocValuesTest, MissingFieldsYieldNoCursor) {
    auto reader = make_multi({std::make_shared<StubLeafReader>(2), std::make_shared<StubLeafReader>(2)});
    ASSERT_NE(reader, nullptr);

    auto numeric = MultiDocValues::get_numeric_values(*reader, "absent");
    ASSERT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
    EXPECT_EQ(numeric.value(), nullptr);

    auto binary = MultiDocValues::get_binary_values(*reader, "absent");
    ASSERT_TRUE(binary.has_value());
    EXPECT_EQ(binary.value(), nullptr);

    auto sorted = MultiDocValues::get_sorted_values(*reader, "absent");
    ASSERT_TRUE(sorted.has_value());
    EXPECT_EQ(sorted.value(), nullptr);

    auto sorted_set = MultiDocValues::get_sorted_set_values(*reader, "absent");
    ASSERT_TRUE(sorted_set.has_value());
    EXPECT_EQ(sorted_set.value(), nullptr);

    auto sorted_numeric = MultiDocValues::get_sorted_numeric_values(*reader, "absent");
    ASSERT_TRUE(sorted_numeric.has_value());
    EXPECT_EQ(sorted_numeric.value(), nullptr);
}

TEST(MultiDocValuesTest, ASingleLeafReturnsItsOwnCursor) {
    auto leaf = std::make_shared<StubLeafReader>(2);
    leaf->set_numeric({1, 2});
    auto reader = make_multi({leaf});
    ASSERT_NE(reader, nullptr);

    auto values = MultiDocValues::get_numeric_values(*reader, "n");
    ASSERT_TRUE(values.has_value());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_EQ(dynamic_cast<MultiNumericDocValues *>(values.value().get()), nullptr)
            << "the single-leaf case wraps nothing";
    EXPECT_EQ(values.value()->next_doc().value(), 0);
    EXPECT_EQ(values.value()->long_value().value(), 1);
}

TEST(MultiDocValuesTest, NormsAreMergedLikeNumericValues) {
    auto first = std::make_shared<StubLeafReader>(1);
    first->set_norms({11});
    auto second = std::make_shared<StubLeafReader>(1);
    second->set_norms({22});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);

    auto norms = MultiDocValues::get_norm_values(*reader, "n");
    ASSERT_TRUE(norms.has_value()) << (norms ? "" : norms.error().to_string());
    ASSERT_NE(norms.value(), nullptr);
    EXPECT_NE(dynamic_cast<MultiNumericDocValues *>(norms.value().get()), nullptr);

    EXPECT_EQ(norms.value()->next_doc().value(), 0);
    EXPECT_EQ(norms.value()->long_value().value(), 11);
    EXPECT_EQ(norms.value()->next_doc().value(), 1);
    EXPECT_EQ(norms.value()->long_value().value(), 22);
}

// ----------------------------------------------------------------- binary

TEST(MultiDocValuesTest, BinaryValuesAreMergedOverTheLeaves) {
    auto first = std::make_shared<StubLeafReader>(2);
    first->set_binary({std::string("a"), std::string("b")});
    auto second = std::make_shared<StubLeafReader>(1);
    second->set_binary({std::string("c")});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);

    auto values = MultiDocValues::get_binary_values(*reader, "b");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);

    EXPECT_EQ(values.value()->next_doc().value(), 0);
    EXPECT_EQ(text_of(values.value()->binary_value().value()), "a");
    EXPECT_EQ(values.value()->advance(2).value(), 2);
    EXPECT_EQ(text_of(values.value()->binary_value().value()), "c");
    EXPECT_EQ(values.value()->next_doc().value(), NO_MORE_DOCS);
}

// --------------------------------------------------------- sorted numeric

TEST(MultiDocValuesTest, SortedNumericValuesAreMergedOverTheLeaves) {
    auto first = std::make_shared<StubLeafReader>(2);
    first->set_sorted_numeric({{1, 2}, {}});
    auto second = std::make_shared<StubLeafReader>(1);
    second->set_sorted_numeric({{3}});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);

    auto values = MultiDocValues::get_sorted_numeric_values(*reader, "sn");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);

    EXPECT_EQ(values.value()->next_doc().value(), 0);
    EXPECT_EQ(values.value()->doc_value_count().value(), 2);
    EXPECT_EQ(values.value()->next_value().value(), 1);
    EXPECT_EQ(values.value()->next_value().value(), 2);

    // The second document of the first leaf has no value at all.
    EXPECT_FALSE(values.value()->advance_exact(1).value());
    EXPECT_TRUE(values.value()->advance_exact(2).value());
    EXPECT_EQ(values.value()->doc_value_count().value(), 1);
    EXPECT_EQ(values.value()->next_value().value(), 3);

    auto out_of_range = values.value()->advance_exact(3);
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().kind(), ErrorKind::kIllegalArgument);
}

// ---------------------------------------------------------------- sorted

TEST(MultiDocValuesTest, SortedValuesUseGlobalOrdinals) {
    auto first = std::make_shared<StubLeafReader>(2);
    first->set_sorted({0, -1}, {"apple", "cherry"});
    auto second = std::make_shared<StubLeafReader>(2);
    second->set_sorted({0, 1}, {"banana", "cherry"});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);

    auto values = MultiDocValues::get_sorted_values(*reader, "s");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_EQ(values.value()->get_value_count().value(), 3) << "apple, banana, cherry";

    EXPECT_EQ(values.value()->next_doc().value(), 0);
    EXPECT_EQ(values.value()->ord_value().value(), 0); // apple
    EXPECT_EQ(values.value()->next_doc().value(), 2);
    EXPECT_EQ(values.value()->ord_value().value(), 1); // banana
    EXPECT_EQ(values.value()->next_doc().value(), 3);
    EXPECT_EQ(values.value()->ord_value().value(), 2); // cherry, shared by both leaves
    EXPECT_EQ(values.value()->next_doc().value(), NO_MORE_DOCS);

    EXPECT_EQ(text_of(values.value()->lookup_ord(0).value()), "apple");
    EXPECT_EQ(text_of(values.value()->lookup_ord(1).value()), "banana");
    EXPECT_EQ(text_of(values.value()->lookup_ord(2).value()), "cherry");

    auto missing = values.value()->lookup_ord(3);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind(), ErrorKind::kIllegalArgument);

    // The inherited binary search over the global dictionary.
    EXPECT_EQ(values.value()->lookup_term(BytesRef::from_string("banana").as_slice()).value(), 1);
    // "blueberry" sorts between "banana" and "cherry": -(insertion point) - 1.
    EXPECT_EQ(values.value()->lookup_term(BytesRef::from_string("blueberry").as_slice()).value(), -3);
}

TEST(MultiDocValuesTest, SortedValuesAdvanceExactly) {
    auto first = std::make_shared<StubLeafReader>(2);
    first->set_sorted({0, -1}, {"apple", "cherry"});
    auto second = std::make_shared<StubLeafReader>(2);
    second->set_sorted({0, 1}, {"banana", "cherry"});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);
    auto values = MultiDocValues::get_sorted_values(*reader, "s");
    ASSERT_TRUE(values.has_value());

    EXPECT_FALSE(values.value()->advance_exact(1).value()) << "no value in the first leaf";
    EXPECT_TRUE(values.value()->advance_exact(3).value());
    EXPECT_EQ(values.value()->ord_value().value(), 2);
    EXPECT_EQ(values.value()->doc_id(), 3);

    auto out_of_range = values.value()->advance_exact(4);
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().kind(), ErrorKind::kIllegalArgument);

    auto backwards = values.value()->advance_exact(2);
    ASSERT_FALSE(backwards.has_value());
    EXPECT_EQ(backwards.error().kind(), ErrorKind::kIllegalArgument);
}

// ------------------------------------------------------------ sorted set

TEST(MultiDocValuesTest, SortedSetValuesUseGlobalOrdinals) {
    auto first = std::make_shared<StubLeafReader>(2);
    first->set_sorted_set({{0, 1}, {}}, {"a", "c"});
    auto second = std::make_shared<StubLeafReader>(2);
    second->set_sorted_set({{0}, {}}, {"b"});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);

    auto values = MultiDocValues::get_sorted_set_values(*reader, "ss");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);
    EXPECT_EQ(values.value()->get_value_count().value(), 3) << "a, b, c";

    EXPECT_EQ(values.value()->next_doc().value(), 0);
    EXPECT_EQ(values.value()->doc_value_count().value(), 2);
    EXPECT_EQ(values.value()->next_ord().value(), 0); // a
    EXPECT_EQ(values.value()->next_ord().value(), 2); // c
    EXPECT_EQ(values.value()->next_ord().value(), NO_MORE_ORDS);

    EXPECT_EQ(values.value()->next_doc().value(), 2);
    EXPECT_EQ(values.value()->doc_value_count().value(), 1);
    EXPECT_EQ(values.value()->next_ord().value(), 1); // b
    EXPECT_EQ(values.value()->next_ord().value(), NO_MORE_ORDS);

    EXPECT_EQ(values.value()->next_doc().value(), NO_MORE_DOCS);

    EXPECT_EQ(text_of(values.value()->lookup_ord(0).value()), "a");
    EXPECT_EQ(text_of(values.value()->lookup_ord(1).value()), "b");
    EXPECT_EQ(text_of(values.value()->lookup_ord(2).value()), "c");

    auto out_of_range = values.value()->lookup_ord(3);
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MultiDocValuesTest, SortedSetValuesAdvanceExactly) {
    auto first = std::make_shared<StubLeafReader>(2);
    first->set_sorted_set({{0, 1}, {}}, {"a", "c"});
    auto second = std::make_shared<StubLeafReader>(2);
    second->set_sorted_set({{0}, {}}, {"b"});
    auto reader = make_multi({first, second});
    ASSERT_NE(reader, nullptr);
    auto values = MultiDocValues::get_sorted_set_values(*reader, "ss");
    ASSERT_TRUE(values.has_value());

    EXPECT_FALSE(values.value()->advance_exact(1).value());
    EXPECT_TRUE(values.value()->advance_exact(2).value());
    EXPECT_EQ(values.value()->doc_value_count().value(), 1);
    EXPECT_EQ(values.value()->next_ord().value(), 1);
    EXPECT_FALSE(values.value()->advance_exact(3).value());
}

TEST(MultiDocValuesTest, AnEmptyReaderHasNoDocValues) {
    auto reader = make_multi({});
    ASSERT_NE(reader, nullptr);

    auto values = MultiDocValues::get_numeric_values(*reader, "n");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    EXPECT_EQ(values.value(), nullptr);
}
