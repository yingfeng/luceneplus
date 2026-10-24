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
#include <set>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.slow_codec_reader_wrapper;

import lucene_core;

using namespace lucene;

namespace {

/// A [`Terms`] that only reports its size.
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

/// A numeric cursor with one constant value per document.
class MarkerNumericDocValues final : public NumericDocValues {
public:
    MarkerNumericDocValues(int64_t value, int32_t max_doc) : value_(value), max_doc_(max_doc) {}

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

private:
    int64_t value_;
    int32_t max_doc_;
    int32_t doc_{-1};
};

class StubStoredFields final : public StoredFields {
public:
    explicit StubStoredFields(int32_t max_doc) : max_doc_(max_doc) {}

    [[nodiscard]] VoidResult prefetch(int32_t /*doc_id*/) override { return {}; }
    [[nodiscard]] VoidResult document_with_visitor(int32_t /*doc_id*/, StoredFieldVisitor & /*visitor*/) override {
        return {};
    }
    [[nodiscard]] Result<Document> document(int32_t doc_id) override {
        if (doc_id < 0 || doc_id >= max_doc_) {
            return std::unexpected(LuceneError::illegal_argument("document out of range"));
        }
        Document document;
        document.add(std::make_unique<Field>(StoredField::from_string("id", std::to_string(doc_id))));
        return document;
    }

private:
    int32_t max_doc_;
};

class StubTermVectors final : public TermVectors {
public:
    [[nodiscard]] VoidResult prefetch(int32_t /*doc_id*/) override { return {}; }
    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t /*doc*/) override {
        return std::unique_ptr<Fields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<Terms>> get_field_terms(int32_t /*doc*/, std::string_view field) override {
        if (field != "title") {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(new MarkerTerms("title", 2));
    }
};

/// A leaf reader with a couple of indexed / doc-values fields.
class StubLeafReader final : public LeafReader {
public:
    StubLeafReader(int32_t max_doc, int32_t num_docs) : max_doc_(max_doc), num_docs_(num_docs) {
        FieldInfosBuilder builder;
        FieldInfo indexed;
        indexed.name = "title";
        indexed.index_options = IndexOptions::DocsAndFreqs;
        indexed.store_term_vector = true;
        (void)builder.add_existing(indexed);
        FieldInfo numeric;
        numeric.name = "n";
        numeric.doc_values_type = DocValuesType::Numeric;
        (void)builder.add_existing(numeric);
        auto finished = builder.finish();
        EXPECT_TRUE(finished.has_value());
        if (finished.has_value()) {
            field_infos_ = std::move(finished.value());
        }
    }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>(new StubStoredFields(max_doc_));
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>(new StubTermVectors());
    }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) override {
        if (field != "title") {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(new MarkerTerms("title", 2));
    }
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric_doc_values(std::string_view field) override {
        if (field != "n") {
            return std::unique_ptr<NumericDocValues>{};
        }
        return std::unique_ptr<NumericDocValues>(new MarkerNumericDocValues(42, max_doc_));
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
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norm_values(std::string_view field) override {
        if (field != "title") {
            return std::unique_ptr<NumericDocValues>{};
        }
        return std::unique_ptr<NumericDocValues>(new MarkerNumericDocValues(7, max_doc_));
    }
    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_doc_values_skipper(std::string_view) override {
        return std::unique_ptr<DocValuesSkipper>{};
    }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &field_infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return nullptr; }

    [[nodiscard]] std::string to_string() const override { return "StubLeafReader"; }

private:
    int32_t max_doc_;
    int32_t num_docs_;
    FieldInfos field_infos_{FieldInfos::none()};
};

const FieldInfo *find_info(const FieldInfos &infos, std::string_view name) {
    const FieldInfo *info = infos.by_name(name);
    EXPECT_NE(info, nullptr) << name;
    return info;
}

} // namespace

TEST(SlowCodecReaderWrapperTest, ExposesEveryCodecReader) {
    auto leaf = std::make_shared<StubLeafReader>(2, 2);
    auto wrapper = SlowCodecReaderWrapper::wrap(leaf);
    ASSERT_TRUE(wrapper.has_value()) << (wrapper ? "" : wrapper.error().to_string());
    const auto &reader = wrapper.value();

    EXPECT_EQ(reader->max_doc().value(), 2);
    EXPECT_EQ(reader->num_docs().value(), 2);
    EXPECT_EQ(reader->field_infos().value(), leaf->field_infos().value());
    EXPECT_EQ(reader->to_string(), "SlowCodecReaderWrapper(StubLeafReader)");

    EXPECT_NE(reader->get_fields_reader(), nullptr);
    EXPECT_NE(reader->get_term_vectors_reader(), nullptr);
    EXPECT_NE(reader->get_norms_reader(), nullptr);
    EXPECT_NE(reader->get_doc_values_reader(), nullptr);
    EXPECT_NE(reader->get_postings_reader(), nullptr);
    EXPECT_NE(reader->get_points_reader(), nullptr);
}

TEST(SlowCodecReaderWrapperTest, StoredFieldsGoThroughTheCodecReader) {
    auto leaf = std::make_shared<StubLeafReader>(2, 2);
    auto wrapper = SlowCodecReaderWrapper::wrap(leaf);
    ASSERT_TRUE(wrapper.has_value()) << (wrapper ? "" : wrapper.error().to_string());

    auto stored = wrapper.value()->stored_fields();
    ASSERT_TRUE(stored.has_value()) << (stored ? "" : stored.error().to_string());
    auto document = stored.value()->document(1);
    ASSERT_TRUE(document.has_value()) << (document ? "" : document.error().to_string());
    EXPECT_EQ(document.value().get("id").value().value(), "1");

    // The codec reader checks the document range itself.
    auto out_of_range = stored.value()->document(5);
    EXPECT_FALSE(out_of_range.has_value());
}

TEST(SlowCodecReaderWrapperTest, TermVectorsGoThroughTheCodecReader) {
    auto leaf = std::make_shared<StubLeafReader>(2, 2);
    auto wrapper = SlowCodecReaderWrapper::wrap(leaf);
    ASSERT_TRUE(wrapper.has_value()) << (wrapper ? "" : wrapper.error().to_string());

    auto vectors = wrapper.value()->term_vectors();
    ASSERT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
    auto terms = vectors.value()->get_field_terms(0, "title");
    ASSERT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    ASSERT_NE(terms.value(), nullptr);
    EXPECT_EQ(terms.value()->size().value(), 2);

    auto absent = vectors.value()->get_field_terms(0, "nope");
    ASSERT_TRUE(absent.has_value());
    EXPECT_EQ(absent.value(), nullptr);
}

TEST(SlowCodecReaderWrapperTest, DocValuesAndNormsGoThroughTheProducers) {
    auto leaf = std::make_shared<StubLeafReader>(2, 2);
    auto wrapper = SlowCodecReaderWrapper::wrap(leaf);
    ASSERT_TRUE(wrapper.has_value()) << (wrapper ? "" : wrapper.error().to_string());
    const FieldInfos &infos = *leaf->field_infos().value();

    auto numeric = wrapper.value()->get_numeric_doc_values("n");
    ASSERT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
    ASSERT_NE(numeric.value(), nullptr);
    ASSERT_TRUE(numeric.value()->next_doc().has_value());
    EXPECT_EQ(numeric.value()->long_value().value(), 42);

    auto bimodal = wrapper.value()->get_binary_doc_values("n");
    ASSERT_TRUE(bimodal.has_value());
    EXPECT_EQ(bimodal.value(), nullptr) << "a field without binary values yields nothing";

    auto norms = wrapper.value()->get_norm_values("title");
    ASSERT_TRUE(norms.has_value()) << (norms ? "" : norms.error().to_string());
    ASSERT_NE(norms.value(), nullptr);
    ASSERT_TRUE(norms.value()->next_doc().has_value());
    EXPECT_EQ(norms.value()->long_value().value(), 7);

    // Asking the producer for a field the leaf cannot serve is an error.
    const FieldInfo *missing = find_info(infos, "n");
    ASSERT_NE(missing, nullptr);
    auto absent = wrapper.value()->get_doc_values_reader()->get_sorted(*missing);
    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().kind(), ErrorKind::kIllegalState);
}

TEST(SlowCodecReaderWrapperTest, PostingsExposeOnlyTheIndexedFields) {
    auto leaf = std::make_shared<StubLeafReader>(2, 2);
    auto wrapper = SlowCodecReaderWrapper::wrap(leaf);
    ASSERT_TRUE(wrapper.has_value()) << (wrapper ? "" : wrapper.error().to_string());

    FieldsProducer *postings = wrapper.value()->get_postings_reader();
    ASSERT_NE(postings, nullptr);
    EXPECT_EQ(postings->size().value(), 1) << "only `title` is indexed";

    auto terms = postings->terms("title");
    ASSERT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    EXPECT_NE(terms.value(), nullptr);

    auto numeric_field = postings->terms("n");
    ASSERT_TRUE(numeric_field.has_value());
    EXPECT_EQ(numeric_field.value(), nullptr);

    auto names = postings->iterator();
    ASSERT_TRUE(names.has_value());
    auto name = names.value()->next();
    ASSERT_TRUE(name.has_value());
    ASSERT_NE(name.value(), nullptr);
    EXPECT_EQ(*name.value(), "title");
    EXPECT_EQ(names.value()->next().value(), nullptr);

    // And through the CodecReader surface, which also checks the field infos.
    EXPECT_NE(wrapper.value()->terms("title").value(), nullptr);
    EXPECT_EQ(wrapper.value()->terms("n").value(), nullptr);
}

TEST(SlowCodecReaderWrapperTest, CheckIntegrityVisitsEveryReader) {
    auto leaf = std::make_shared<StubLeafReader>(1, 1);
    auto wrapper = SlowCodecReaderWrapper::wrap(leaf);
    ASSERT_TRUE(wrapper.has_value()) << (wrapper ? "" : wrapper.error().to_string());
    EXPECT_TRUE(wrapper.value()->check_integrity().has_value());
}

TEST(SlowCodecReaderWrapperTest, ClonesAreIndependent) {
    auto leaf = std::make_shared<StubLeafReader>(2, 2);
    auto wrapper = SlowCodecReaderWrapper::wrap(leaf);
    ASSERT_TRUE(wrapper.has_value()) << (wrapper ? "" : wrapper.error().to_string());

    auto *reader = wrapper.value()->get_fields_reader();
    ASSERT_NE(reader, nullptr);
    auto cloned = reader->clone();
    ASSERT_TRUE(cloned.has_value()) << (cloned ? "" : cloned.error().to_string());
    EXPECT_NE(cloned.value().get(), reader);
    auto document = cloned.value()->document(0);
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(document.value().get("id").value().value(), "0");
}

TEST(SlowCodecReaderWrapperTest, RejectsANullReader) {
    auto wrapper = SlowCodecReaderWrapper::wrap(nullptr);
    ASSERT_FALSE(wrapper.has_value());
    EXPECT_EQ(wrapper.error().kind(), ErrorKind::kIllegalArgument);
}
