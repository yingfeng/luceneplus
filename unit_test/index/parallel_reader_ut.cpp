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
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.parallel_reader;

import lucene_core;

using namespace lucene;

namespace {

/// A [`Terms`] that only reports its size, enough to observe field routing.
class MarkerTerms final : public Terms {
public:
    MarkerTerms(std::string name, int64_t size) : name_(std::move(name)), size_(size) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>{};
    }
    [[nodiscard]] Result<int64_t> size() const override { return size_; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return 0; }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return 0; }
    [[nodiscard]] Result<int32_t> doc_count() const override { return static_cast<int32_t>(size_); }
    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }
    [[nodiscard]] Result<std::string> get_stats() const override { return name_; }

private:
    std::string name_;
    int64_t size_;
};

/// A numeric cursor with a single constant value per document.
class MarkerNumericDocValues final : public NumericDocValues {
public:
    explicit MarkerNumericDocValues(int64_t value) : value_(value) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override {
        doc_ = doc_ + 1 < max_doc_ ? doc_ + 1 : NO_MORE_DOCS;
        return doc_;
    }
    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (target <= doc_) {
            return std::unexpected(LuceneError::illegal_argument("advance must move forward"));
        }
        doc_ = target < max_doc_ ? target : NO_MORE_DOCS;
        return doc_;
    }
    [[nodiscard]] Result<int64_t> cost() const override { return max_doc_; }
    [[nodiscard]] Result<int64_t> long_value() override { return value_; }

    void set_max_doc(int32_t max_doc) { max_doc_ = max_doc; }

private:
    int64_t value_;
    int32_t doc_{-1};
    int32_t max_doc_{std::numeric_limits<int32_t>::max()};
};

/// Stored fields of one stub reader: every document has one field.
class StubStoredFields final : public StoredFields {
public:
    StubStoredFields(int32_t reader_id, int32_t max_doc) : reader_id_(reader_id), max_doc_(max_doc) {}

    [[nodiscard]] VoidResult prefetch(int32_t /*doc_id*/) override {
        ++prefetches;
        return {};
    }
    [[nodiscard]] VoidResult document_with_visitor(int32_t /*doc_id*/, StoredFieldVisitor & /*visitor*/) override {
        return {};
    }
    [[nodiscard]] Result<Document> document(int32_t doc_id) override {
        if (doc_id < 0 || doc_id >= max_doc_) {
            return std::unexpected(LuceneError::illegal_argument("document out of range"));
        }
        Document document;
        document.add(std::make_unique<Field>(StoredField::from_string("reader", std::to_string(reader_id_))));
        return document;
    }

    int prefetches{0};

private:
    int32_t reader_id_;
    int32_t max_doc_;
};

/// Term vectors of one stub reader, with a fixed set of fields.
class StubTermVectors final : public TermVectors {
public:
    explicit StubTermVectors(std::set<std::string> fields) : fields_(std::move(fields)) {}

    [[nodiscard]] VoidResult prefetch(int32_t /*doc_id*/) override {
        ++prefetches;
        return {};
    }
    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t /*doc*/) override {
        return std::unique_ptr<Fields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<Terms>> get_field_terms(int32_t /*doc*/, std::string_view field) override {
        if (fields_.find(std::string(field)) == fields_.end()) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(new MarkerTerms(std::string(field), 1));
    }

    int prefetches{0};

private:
    std::set<std::string> fields_;
};

/// A leaf reader that answers for a fixed set of fields.
class StubLeafReader final : public LeafReader {
public:
    StubLeafReader(int32_t max_doc, int32_t num_docs, int32_t id, std::vector<FieldInfo> fields,
                   std::set<std::string> term_vector_fields = {})
        : max_doc_(max_doc),
          num_docs_(num_docs),
          id_(id),
          term_vector_fields_(std::move(term_vector_fields)) {
        FieldInfosBuilder builder;
        for (FieldInfo &info : fields) {
            auto added = builder.add_existing(std::move(info));
            EXPECT_TRUE(added.has_value()) << (added ? "" : added.error().to_string());
            if (added.has_value()) {
                names_.insert(added.value()->name);
            }
        }
        auto finished = builder.finish();
        EXPECT_TRUE(finished.has_value());
        if (finished.has_value()) {
            field_infos_ = std::move(finished.value());
        }
        for (const FieldInfo &info : field_infos_.fields()) {
            if (info.index_options != IndexOptions::None) {
                indexed_.insert(info.name);
            }
        }
    }

    void set_live_docs(const Bits *bits) { live_docs_ = bits; }
    void set_leaf_meta_data(LeafMetaData metadata) { leaf_meta_data_ = std::move(metadata); }

    [[nodiscard]] const LeafMetaData &leaf_meta_data() const override { return leaf_meta_data_; }
    void set_close_error(std::optional<LuceneError> error) { close_error_ = std::move(error); }

    [[nodiscard]] int32_t closes() const noexcept { return closes_; }
    [[nodiscard]] int32_t id() const noexcept { return id_; }

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>(new StubStoredFields(id_, max_doc_));
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>(new StubTermVectors(term_vector_fields_));
    }
    [[nodiscard]] VoidResult close() const override {
        ++closes_;
        if (close_error_.has_value()) {
            return std::unexpected(close_error_.value());
        }
        return {};
    }

    // ---- LeafReader ----

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (indexed_.find(std::string(field)) == indexed_.end()) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(new MarkerTerms(std::string(field), id_));
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view field) override {
        if (names_.find(std::string(field)) == names_.end()) {
            return std::unique_ptr<NumericDocValues>{};
        }
        auto values = std::make_unique<MarkerNumericDocValues>(id_);
        values->set_max_doc(max_doc_);
        return std::unique_ptr<NumericDocValues>(std::move(values));
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
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_docs_; }

    [[nodiscard]] std::string to_string() const override { return "StubLeafReader(" + std::to_string(id_) + ")"; }

private:
    int32_t max_doc_;
    int32_t num_docs_;
    int32_t id_;
    std::set<std::string> names_;
    std::set<std::string> indexed_;
    std::set<std::string> term_vector_fields_;
    std::optional<LuceneError> close_error_;
    const Bits *live_docs_{nullptr};
    mutable int32_t closes_{0};
    LeafMetaData leaf_meta_data_{LeafMetaData::unknown()};
    FieldInfos field_infos_{FieldInfos::none()};
};

FieldInfo field(std::string name, IndexOptions options = IndexOptions::DocsAndFreqs) {
    FieldInfo info;
    info.name = std::move(name);
    info.index_options = options;
    info.doc_values_type = DocValuesType::Numeric;
    return info;
}

FieldInfo term_vector_field(std::string name) {
    FieldInfo info = field(std::move(name));
    info.store_term_vector = true;
    return info;
}

} // namespace

// --------------------------------------------------------- ParallelLeafReader

TEST(ParallelLeafReaderTest, RoutesEachFieldToTheReaderThatHasIt) {
    auto first = std::make_shared<StubLeafReader>(2, 2, 1, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(2, 2, 2, std::vector<FieldInfo>{field("b")});

    auto reader = ParallelLeafReader::create({first, second});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    const auto &parallel = reader.value();

    EXPECT_EQ(parallel->max_doc().value(), 2);
    EXPECT_EQ(parallel->num_docs().value(), 2);
    EXPECT_EQ(parallel->parallel_readers().size(), 2U);
    EXPECT_EQ(parallel->merged_field_infos().size(), 2U);

    auto a = parallel->get_numeric_doc_values("a");
    ASSERT_TRUE(a.has_value()) << (a ? "" : a.error().to_string());
    ASSERT_NE(a.value(), nullptr);
    ASSERT_TRUE(a.value()->next_doc().has_value());
    EXPECT_EQ(a.value()->long_value().value(), 1) << "served by the first reader";

    auto b = parallel->get_numeric_doc_values("b");
    ASSERT_TRUE(b.has_value());
    ASSERT_NE(b.value(), nullptr);
    ASSERT_TRUE(b.value()->next_doc().has_value());
    EXPECT_EQ(b.value()->long_value().value(), 2) << "served by the second reader";

    auto missing = parallel->get_numeric_doc_values("c");
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing.value(), nullptr);

    // Only indexed fields have terms.
    EXPECT_NE(parallel->terms("a").value(), nullptr);
    auto unindexed = std::make_shared<StubLeafReader>(2, 2, 3, std::vector<FieldInfo>{field("stored", IndexOptions::None)});
    auto with_unindexed = ParallelLeafReader::create({unindexed});
    ASSERT_TRUE(with_unindexed.has_value());
    EXPECT_EQ(with_unindexed.value()->terms("stored").value(), nullptr);

    EXPECT_NE(parallel->to_string().find("ParallelLeafReader(StubLeafReader(1), StubLeafReader(2))"),
              std::string::npos);
}

TEST(ParallelLeafReaderTest, TheFirstReaderWinsForASharedFieldName) {
    auto first = std::make_shared<StubLeafReader>(1, 1, 7, std::vector<FieldInfo>{field("shared")});
    auto second = std::make_shared<StubLeafReader>(1, 1, 9, std::vector<FieldInfo>{field("shared")});

    auto reader = ParallelLeafReader::create({first, second});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    auto values = reader.value()->get_numeric_doc_values("shared");
    ASSERT_TRUE(values.has_value());
    ASSERT_NE(values.value(), nullptr);
    ASSERT_TRUE(values.value()->next_doc().has_value());
    EXPECT_EQ(values.value()->long_value().value(), 7);
    EXPECT_EQ(reader.value()->merged_field_infos().size(), 1U);
}

TEST(ParallelLeafReaderTest, RejectsReadersWithDifferentDocumentCounts) {
    auto first = std::make_shared<StubLeafReader>(2, 2, 1, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(3, 3, 2, std::vector<FieldInfo>{field("b")});

    auto reader = ParallelLeafReader::create({first, second});
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(reader.error().message().find("same maxDoc"), std::string::npos);
}

TEST(ParallelLeafReaderTest, ConcatenatesTheStoredFields) {
    auto first = std::make_shared<StubLeafReader>(1, 1, 5, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(1, 1, 6, std::vector<FieldInfo>{field("b")});
    auto reader = ParallelLeafReader::create({first, second});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    auto stored = reader.value()->stored_fields();
    ASSERT_TRUE(stored.has_value()) << (stored ? "" : stored.error().to_string());
    auto document = stored.value()->document(0);
    ASSERT_TRUE(document.has_value()) << (document ? "" : document.error().to_string());
    auto values = document.value().get_values("reader");
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values.value(), (std::vector<std::string>{"5", "6"}));

    ASSERT_TRUE(stored.value()->prefetch(0).has_value());
}

TEST(ParallelLeafReaderTest, MergesTheTermVectors) {
    auto first = std::make_shared<StubLeafReader>(1, 1, 1, std::vector<FieldInfo>{term_vector_field("title")},
                                                  std::set<std::string>{"title"});
    auto second = std::make_shared<StubLeafReader>(1, 1, 2, std::vector<FieldInfo>{term_vector_field("body")},
                                                   std::set<std::string>{"body"});
    auto reader = ParallelLeafReader::create({first, second});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    auto vectors = reader.value()->term_vectors();
    ASSERT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());

    auto fields = vectors.value()->get(0);
    ASSERT_TRUE(fields.has_value()) << (fields ? "" : fields.error().to_string());
    ASSERT_NE(fields.value(), nullptr);
    EXPECT_EQ(fields.value()->size().value(), 2);

    auto names = fields.value()->iterator();
    ASSERT_TRUE(names.has_value());
    std::set<std::string> seen;
    while (true) {
        auto name = names.value()->next();
        ASSERT_TRUE(name.has_value());
        if (name.value() == nullptr) {
            break;
        }
        seen.insert(*name.value());
    }
    EXPECT_EQ(seen, (std::set<std::string>{"body", "title"}));

    auto title = fields.value()->terms("title");
    ASSERT_TRUE(title.has_value()) << (title ? "" : title.error().to_string());
    ASSERT_NE(title.value(), nullptr);
    EXPECT_EQ(title.value()->get_stats().value(), "title");

    EXPECT_EQ(fields.value()->terms("missing").value(), nullptr);
    EXPECT_TRUE(vectors.value()->prefetch(0).has_value());
}

TEST(ParallelLeafReaderTest, DeletionsComeFromTheFirstReader) {
    auto first = std::make_shared<StubLeafReader>(2, 1, 1, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(2, 2, 2, std::vector<FieldInfo>{field("b")});

    auto reader = ParallelLeafReader::create({first, second});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_TRUE(reader.value()->has_deletions().value());

    auto live = reader.value()->live_docs();
    ASSERT_TRUE(live.has_value());
    // The stub has no live docs of its own, but the call is routed to the first
    // reader (a leaf without deletions would be a null pointer either way).
    EXPECT_EQ(live.value(), nullptr);
}

TEST(ParallelLeafReaderTest, CloseClosesEachDistinctSubReaderOnce) {
    auto first = std::make_shared<StubLeafReader>(1, 1, 1, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(1, 1, 2, std::vector<FieldInfo>{field("b")});

    auto reader = ParallelLeafReader::create({first, second, first});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    ASSERT_TRUE(reader.value()->close().has_value());
    EXPECT_EQ(first->closes(), 1);
    EXPECT_EQ(second->closes(), 1);

    ASSERT_TRUE(reader.value()->close().has_value());
    EXPECT_EQ(first->closes(), 1) << "closing twice is a no-op";
}

TEST(ParallelLeafReaderTest, CloseCanLeaveTheSubReadersOpen) {
    auto first = std::make_shared<StubLeafReader>(1, 1, 1, std::vector<FieldInfo>{field("a")});
    auto reader = ParallelLeafReader::create_with_close_sub_readers(false, {first});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    ASSERT_TRUE(reader.value()->close().has_value());
    EXPECT_EQ(first->closes(), 0);
}

// ---------------------------------------------------- ParallelCompositeReader

TEST(ParallelCompositeReaderTest, GroupsTheCorrespondingLeaves) {
    auto first = std::make_shared<StubLeafReader>(2, 2, 1, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(2, 2, 2, std::vector<FieldInfo>{field("b")});
    auto left = MultiReader::create({first, second});
    ASSERT_TRUE(left.has_value()) << (left ? "" : left.error().to_string());

    auto third = std::make_shared<StubLeafReader>(2, 2, 3, std::vector<FieldInfo>{field("c")});
    auto fourth = std::make_shared<StubLeafReader>(2, 2, 4, std::vector<FieldInfo>{field("d")});
    auto right = MultiReader::create({third, fourth});
    ASSERT_TRUE(right.has_value()) << (right ? "" : right.error().to_string());

    auto reader = ParallelCompositeReader::create({left.value(), right.value()});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    EXPECT_EQ(reader.value()->max_doc().value(), 4);
    EXPECT_EQ(reader.value()->num_docs().value(), 4);
    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value()) << (leaves ? "" : leaves.error().to_string());
    ASSERT_EQ(leaves.value().size(), 2U);
    EXPECT_EQ(leaves.value()[0].doc_base(), 0);
    EXPECT_EQ(leaves.value()[1].doc_base(), 2);

    // Each leaf is the parallel combination of one leaf from each reader.
    const auto &leaf_readers = reader.value()->leaf_readers();
    ASSERT_EQ(leaf_readers.size(), 2U);
    EXPECT_EQ(leaf_readers[0]->parallel_readers()[0].get(), first.get());
    EXPECT_EQ(leaf_readers[0]->parallel_readers()[1].get(), third.get());
    EXPECT_EQ(leaf_readers[1]->parallel_readers()[0].get(), second.get());
    EXPECT_EQ(leaf_readers[1]->parallel_readers()[1].get(), fourth.get());
    EXPECT_EQ(leaf_readers[0]->merged_field_infos().size(), 2U);
}

TEST(ParallelCompositeReaderTest, RejectsReadersWithDifferentShapes) {
    auto a = std::make_shared<StubLeafReader>(2, 2, 1, std::vector<FieldInfo>{field("a")});
    auto b = std::make_shared<StubLeafReader>(3, 3, 2, std::vector<FieldInfo>{field("b")});
    auto taller = MultiReader::create({b});
    ASSERT_TRUE(taller.has_value());

    auto first = std::make_shared<StubLeafReader>(2, 2, 1, std::vector<FieldInfo>{field("a")});
    auto shorter = MultiReader::create({a});
    auto reader = ParallelCompositeReader::create({shorter.value(), taller.value()});
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kIllegalArgument);

    // Different number of leaves is rejected too.
    auto two_leaves = MultiReader::create({first, a});
    ASSERT_TRUE(two_leaves.has_value());
    auto mismatch = ParallelCompositeReader::create({shorter.value(), two_leaves.value()});
    ASSERT_FALSE(mismatch.has_value());
    EXPECT_EQ(mismatch.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(ParallelCompositeReaderTest, CloseClosesTheSubReadersOnce) {
    auto a = std::make_shared<StubLeafReader>(1, 1, 1, std::vector<FieldInfo>{field("a")});
    auto b = std::make_shared<StubLeafReader>(1, 1, 2, std::vector<FieldInfo>{field("b")});
    auto left = MultiReader::create_with_close_sub_readers(false, {a});
    auto right = MultiReader::create_with_close_sub_readers(false, {b});
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());

    auto reader = ParallelCompositeReader::create({left.value(), right.value()});
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    ASSERT_TRUE(reader.value()->close().has_value());
    EXPECT_EQ(left.value()->sub_readers().size(), 1U);
    // Closing the composite closes the composite sub-readers; those were created
    // with `close_sub_readers == false`, so the stub leaves stay open.
    EXPECT_EQ(a->closes(), 0);
    EXPECT_EQ(b->closes(), 0);

    ASSERT_TRUE(reader.value()->close().has_value());
    EXPECT_EQ(a->closes(), 0);
}

TEST(ParallelCompositeReaderTest, ToStringListsTheSyntheticLeaves) {
    auto a = std::make_shared<StubLeafReader>(1, 1, 1, std::vector<FieldInfo>{field("a")});
    auto left = MultiReader::create({a});
    ASSERT_TRUE(left.has_value());
    auto reader = ParallelCompositeReader::create({left.value()});
    ASSERT_TRUE(reader.has_value());

    const std::string described = reader.value()->to_string();
    EXPECT_EQ(described.rfind("ParallelCompositeReader(", 0), 0U);
    EXPECT_NE(described.find("ParallelLeafReader(StubLeafReader(1))"), std::string::npos);
}

namespace {

/// A `Sort` over one long doc-values field, for the metadata tests.
Sort long_sort(std::string field) {
    auto sort_field = SortField::create(field, SortFieldType::kLong);
    EXPECT_TRUE(sort_field.has_value()) << (sort_field ? "" : sort_field.error().to_string());
    std::vector<SortField> fields;
    fields.push_back(sort_field.value());
    auto sort = Sort::with_fields(std::move(fields));
    EXPECT_TRUE(sort.has_value()) << (sort ? "" : sort.error().to_string());
    return sort.value();
}

} // namespace

TEST(ParallelLeafReaderTest, RejectsDifferentCreationVersions) {
    auto first = std::make_shared<StubLeafReader>(2, 2, 1, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(2, 2, 2, std::vector<FieldInfo>{field("b")});
    first->set_leaf_meta_data(LeafMetaData::create(9, Version(7, 0, 0), nullptr, false).value());
    second->set_leaf_meta_data(LeafMetaData::create(8, Version(7, 0, 0), nullptr, false).value());

    auto combined = ParallelLeafReader::create({first, second});
    ASSERT_FALSE(combined.has_value());
    EXPECT_EQ(combined.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(combined.error().message().find("different creation versions"), std::string::npos);
}

TEST(ParallelLeafReaderTest, RejectsDifferentIndexSorts) {
    auto first = std::make_shared<StubLeafReader>(2, 2, 1, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(2, 2, 2, std::vector<FieldInfo>{field("b")});
    first->set_leaf_meta_data(
            LeafMetaData::create(9, Version(7, 0, 0), std::make_shared<const Sort>(long_sort("ts")), false).value());
    // A reader without a sort may join a sorted one; only two *different* sorts
    // are rejected (the reference implementation's rule).
    second->set_leaf_meta_data(
            LeafMetaData::create(9, Version(7, 0, 0), std::make_shared<const Sort>(long_sort("other")), false).value());

    auto combined = ParallelLeafReader::create({first, second});
    ASSERT_FALSE(combined.has_value());
    EXPECT_EQ(combined.error().kind(), ErrorKind::kIllegalArgument);
    EXPECT_NE(combined.error().message().find("different index sorts"), std::string::npos);
}

TEST(ParallelLeafReaderTest, MergesTheMetadata) {
    auto first = std::make_shared<StubLeafReader>(2, 2, 1, std::vector<FieldInfo>{field("a")});
    auto second = std::make_shared<StubLeafReader>(2, 2, 2, std::vector<FieldInfo>{field("b")});
    const Sort shared = long_sort("ts");
    first->set_leaf_meta_data(
            LeafMetaData::create(9, Version(8, 0, 0), std::make_shared<const Sort>(shared), true).value());
    second->set_leaf_meta_data(
            LeafMetaData::create(9, Version(7, 0, 0), std::make_shared<const Sort>(shared), false).value());

    auto combined = ParallelLeafReader::create({first, second});
    ASSERT_TRUE(combined.has_value()) << (combined ? "" : combined.error().to_string());

    const LeafMetaData &metadata = combined.value()->leaf_meta_data();
    EXPECT_EQ(metadata.created_version_major(), 9);
    ASSERT_TRUE(metadata.min_version().has_value());
    EXPECT_EQ(metadata.min_version()->major, 7) << "the oldest contributing version wins";
    ASSERT_NE(metadata.sort(), nullptr);
    EXPECT_TRUE(metadata.sort()->equals(shared));
    EXPECT_TRUE(metadata.has_blocks()) << "true when any sub-reader holds blocks";
}
