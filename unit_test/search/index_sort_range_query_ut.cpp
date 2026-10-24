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
#include <utility>
#include <vector>

module lucene_core:ut.index_sort_range_query;

import lucene_core;

using namespace lucene;

namespace {


/// A query that always matches the same fixed documents; it stands in for the
/// plain range query a caller would hand in as the fallback.
class FixedDocsDisi final : public DocIdSetIterator {
public:
    explicit FixedDocsDisi(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

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
        while (doc_id() < target && doc_id() != NO_MORE_DOCS) {
            auto moved = next_doc();
            if (!moved.has_value()) {
                return moved;
            }
        }
        return doc_id();
    }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    std::vector<int32_t> docs_;
    size_t next_{0};
    int32_t doc_{-1};
};

class FixedDocsQuery;

class FixedDocsWeight final : public Weight {
public:
    FixedDocsWeight(const Query &parent, std::vector<int32_t> docs, float score)
        : parent_(&parent), docs_(std::move(docs)), score_(score) {}

    [[nodiscard]] Result<std::unique_ptr<Scorer>> scorer(const LeafReaderContext & /*context*/,
                                                         const IndexSearcher & /*searcher*/) override {
        return ConstantScoreScorer::from_disi(score_, ScoreMode::Complete,
                                              std::make_unique<FixedDocsDisi>(docs_));
    }

    [[nodiscard]] Result<std::unique_ptr<ScorerSupplier>> scorer_supplier(const LeafReaderContext & /*context*/,
                                                                          const IndexSearcher & /*searcher*/) override {
        return std::unique_ptr<ScorerSupplier>(new FixedDocsSupplier(docs_, score_));
    }

    [[nodiscard]] Result<int32_t> count(const LeafReaderContext & /*context*/,
                                        const IndexSearcher & /*searcher*/) override {
        return static_cast<int32_t>(docs_.size());
    }

    [[nodiscard]] Result<Explanation> explain(const LeafReaderContext & /*context*/, int32_t doc,
                                              const IndexSearcher & /*searcher*/) const override {
        const bool matches = std::ranges::find(docs_, doc) != docs_.end();
        if (!matches) {
            return Explanation::no_match_no_details("not a fixed document");
        }
        return Explanation::match_no_details(score_, "fixed document");
    }

    [[nodiscard]] const Query &get_query() const override { return *parent_; }
    [[nodiscard]] Result<bool> is_cacheable(const LeafReaderContext & /*context*/) const override { return true; }

private:
    class FixedDocsSupplier final : public ScorerSupplier {
    public:
        FixedDocsSupplier(std::vector<int32_t> docs, float score) : docs_(std::move(docs)), score_(score) {}

        [[nodiscard]] Result<std::unique_ptr<Scorer>> get(int64_t /*lead_cost*/, const LeafReaderContext & /*context*/,
                                                          const IndexSearcher & /*searcher*/) override {
            return ConstantScoreScorer::from_disi(score_, ScoreMode::Complete,
                                                  std::make_unique<FixedDocsDisi>(docs_));
        }
        [[nodiscard]] Result<int64_t> cost(const LeafReaderContext & /*context*/,
                                           const IndexSearcher & /*searcher*/) override {
            return static_cast<int64_t>(docs_.size());
        }

    private:
        std::vector<int32_t> docs_;
        float score_;
    };

    const Query *parent_;
    std::vector<int32_t> docs_;
    float score_;
};

class FixedDocsQuery final : public Query {
public:
    FixedDocsQuery(std::vector<int32_t> docs, float score = 1.0F) : docs_(std::move(docs)), score_(score) {}

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher & /*searcher*/,
                                                                ScoreMode /*score_mode*/,
                                                                float /*boost*/) const override {
        return std::unique_ptr<Weight>(new FixedDocsWeight(*this, docs_, score_));
    }

    [[nodiscard]] VoidResult visit(QueryVisitor & /*visitor*/) const override { return {}; }
    [[nodiscard]] Result<std::string> to_string(std::string_view /*field*/) const override {
        return std::string("FixedDocs");
    }
    [[nodiscard]] bool equals(const Query &other) const override {
        const auto *fixed = dynamic_cast<const FixedDocsQuery *>(&other);
        return fixed != nullptr && fixed->docs_ == docs_;
    }
    [[nodiscard]] size_t hash() const override { return docs_.size(); }

private:
    std::vector<int32_t> docs_;
    float score_;
};

/// `SortedNumericDocValues` over one ascending value per document.
class VectorSortedNumericDocValues final : public SortedNumericDocValues {
public:
    explicit VectorSortedNumericDocValues(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override {
        if (index_ < 0) {
            return -1;
        }
        return index_ < size() ? static_cast<int32_t>(index_) : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++index_;
        return doc_id();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_id() < target && doc_id() != NO_MORE_DOCS) {
            ++index_;
        }
        return doc_id();
    }

    /// Random access, which is what the range query's binary search needs.
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        if (target < 0) {
            return err(LuceneError::illegal_argument("a document id cannot be negative"));
        }
        if (static_cast<size_t>(target) >= size()) {
            index_ = static_cast<int64_t>(size());
            return false;
        }
        index_ = target;
        return true;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(size()); }

    [[nodiscard]] Result<int32_t> doc_value_count() override { return doc_id() >= 0 ? 1 : 0; }

    [[nodiscard]] Result<int64_t> next_value() override {
        if (doc_id() < 0 || doc_id() == NO_MORE_DOCS) {
            return err(LuceneError::illegal_state("the values are not positioned"));
        }
        return values_[static_cast<size_t>(doc_id())];
    }

    [[nodiscard]] bool is_single_valued() const override { return true; }

private:
    [[nodiscard]] size_t size() const noexcept { return values_.size(); }

    std::vector<int64_t> values_;
    int64_t index_{-1};
};

/// A leaf with one ascending numeric column, optionally claiming an index sort.
class RangeStubLeaf final : public LeafReader {
public:
    RangeStubLeaf(std::vector<int64_t> values, bool index_sorted) : values_(std::move(values)) {
        if (index_sorted) {
            auto sort_field = SortField::create("ts", SortFieldType::kLong);
            EXPECT_TRUE(sort_field.has_value());
            std::vector<SortField> fields;
            fields.push_back(sort_field.value());
            auto sort = Sort::with_fields(std::move(fields));
            EXPECT_TRUE(sort.has_value());
            auto metadata = LeafMetaData::create(LUCENE_CURRENT_VERSION.major, LUCENE_CURRENT_VERSION,
                                                 std::make_shared<const Sort>(sort.value()), false);
            EXPECT_TRUE(metadata.has_value());
            if (metadata.has_value()) {
                leaf_meta_data_ = std::move(metadata.value());
            }
        }
    }

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return static_cast<int32_t>(values_.size()); }
    [[nodiscard]] Result<int32_t> num_docs() const override { return static_cast<int32_t>(values_.size()); }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override {
        return std::unique_ptr<StoredFields>{};
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override {
        return std::unique_ptr<TermVectors>{};
    }
    [[nodiscard]] VoidResult close() const override { return {}; }

    // ---- LeafReader ----

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view) override {
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
    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>>
    get_sorted_numeric_doc_values(std::string_view field) override {
        if (field != "ts") {
            return std::unique_ptr<SortedNumericDocValues>{};
        }
        return std::unique_ptr<SortedNumericDocValues>(new VectorSortedNumericDocValues(values_));
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

    [[nodiscard]] const LeafMetaData &leaf_meta_data() const override { return leaf_meta_data_; }

    [[nodiscard]] std::string to_string() const override { return "RangeStubLeaf"; }

private:
    std::vector<int64_t> values_;
    FieldInfos field_infos_{FieldInfos::none()};
    const Bits *live_docs_{nullptr};
    LeafMetaData leaf_meta_data_{LeafMetaData::unknown()};
};

/// Builds a searcher over a stub leaf whose column is `values`.
std::pair<std::shared_ptr<SingleLeafCompositeReader>, std::shared_ptr<RangeStubLeaf>>
make_reader(std::vector<int64_t> values, bool index_sorted) {
    auto leaf = std::make_shared<RangeStubLeaf>(std::move(values), index_sorted);
    auto reader = SingleLeafCompositeReader::create(leaf);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return {reader.has_value() ? reader.value() : nullptr, leaf};
}

std::vector<int32_t> hits_of(IndexSearcher &searcher, const Query &query) {
    std::vector<int32_t> docs;
    auto top = searcher.search(query, 10);
    EXPECT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    if (!top.has_value()) {
        return docs;
    }
    for (const ScoreDoc &hit : top.value().score_docs) {
        docs.push_back(hit.doc);
    }
    std::sort(docs.begin(), docs.end());
    return docs;
}

} // namespace

TEST(IndexSortRangeQueryTest, UsesTheSortedRun) {
    // 10, 20, 30, 40, 50: the range 20..40 is the contiguous run 1..3.
    auto [reader, leaf] = make_reader({10, 20, 30, 40, 50}, /*index_sorted=*/true);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const IndexSortSortedNumericDocValuesRangeQuery query(
            "ts", 20, 40, std::make_shared<FixedDocsQuery>(std::vector<int32_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(hits_of(searcher, query), (std::vector<int32_t>{1, 2, 3})) << "the sorted run wins over the fallback";
    EXPECT_EQ(searcher.count(query).value(), 3) << "the count comes from the run size";
}

TEST(IndexSortRangeQueryTest, AnEmptyRunMatchesNothing) {
    auto [reader, leaf] = make_reader({10, 20, 30, 40, 50}, /*index_sorted=*/true);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // A fallback that matches everything would hide a broken run, so the fallback
    // is a fixed set here.
    const IndexSortSortedNumericDocValuesRangeQuery above(
            "ts", 100, 200, std::make_shared<FixedDocsQuery>(std::vector<int32_t>{0, 1, 2, 3, 4}));
    EXPECT_TRUE(hits_of(searcher, above).empty()) << "no document is in [100, 200]";
    EXPECT_EQ(searcher.count(above).value(), 0);

    const IndexSortSortedNumericDocValuesRangeQuery inverted(
            "ts", 40, 20, std::make_shared<FixedDocsQuery>(std::vector<int32_t>{0, 1, 2, 3, 4}));
    EXPECT_TRUE(hits_of(searcher, inverted).empty()) << "an inverted range cannot match";
    EXPECT_EQ(searcher.count(inverted).value(), 0);

    // A range that ends before the first value is empty too.
    const IndexSortSortedNumericDocValuesRangeQuery below(
            "ts", 1, 5, std::make_shared<FixedDocsQuery>(std::vector<int32_t>{0, 1, 2, 3, 4}));
    EXPECT_TRUE(hits_of(searcher, below).empty());
}

TEST(IndexSortRangeQueryTest, FallsBackWithoutAnIndexSort) {
    // The same column, but the segment does not claim a sort: the fallback answers.
    auto [reader, leaf] = make_reader({10, 20, 30, 40, 50}, /*index_sorted=*/false);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const IndexSortSortedNumericDocValuesRangeQuery query(
            "ts", 20, 40, std::make_shared<FixedDocsQuery>(std::vector<int32_t>{0, 4}));
    EXPECT_EQ(hits_of(searcher, query), (std::vector<int32_t>{0, 4})) << "the fallback answers";
    EXPECT_EQ(searcher.count(query).value(), 2);
}

TEST(IndexSortRangeQueryTest, FallsBackForAReverseSortedColumn) {
    // Descending values cannot be binary searched, so the shortcut must bail out
    // and the fallback has to answer.
    auto [reader, leaf] = make_reader({50, 40, 30, 20, 10}, /*index_sorted=*/true);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const IndexSortSortedNumericDocValuesRangeQuery query(
            "ts", 20, 40, std::make_shared<FixedDocsQuery>(std::vector<int32_t>{0, 4}));
    EXPECT_EQ(hits_of(searcher, query), (std::vector<int32_t>{0, 4}))
            << "a descending column cannot be binary searched, so the fallback answers";
}

TEST(IndexSortRangeQueryTest, ExplainsAMatchingAndANonMatchingDocument) {
    auto [reader, leaf] = make_reader({10, 20, 30, 40, 50}, /*index_sorted=*/true);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const IndexSortSortedNumericDocValuesRangeQuery query(
            "ts", 20, 40, std::make_shared<FixedDocsQuery>(std::vector<int32_t>{0, 1, 2, 3, 4}));
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 2.0F);
    ASSERT_TRUE(weight.has_value()) << (weight ? "" : weight.error().to_string());

    auto leaves = reader->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_FALSE(leaves.value().empty());

    auto inside = weight.value()->explain(leaves.value()[0], 2, searcher);
    ASSERT_TRUE(inside.has_value()) << (inside ? "" : inside.error().to_string());
    EXPECT_TRUE(inside.value().is_match());

    auto outside = weight.value()->explain(leaves.value()[0], 4, searcher);
    ASSERT_TRUE(outside.has_value());
    EXPECT_FALSE(outside.value().is_match());
}

TEST(IndexSortRangeQueryTest, PrintsAndCompares) {
    const IndexSortSortedNumericDocValuesRangeQuery query("ts", 20, 40, std::make_shared<MatchAllDocsQuery>());
    auto printed = query.to_string("");
    ASSERT_TRUE(printed.has_value());
    EXPECT_EQ(printed.value(), "ts:[20 TO 40]");

    auto in_field = query.to_string("ts");
    ASSERT_TRUE(in_field.has_value());
    EXPECT_EQ(in_field.value(), "[20 TO 40]");

    const IndexSortSortedNumericDocValuesRangeQuery same("ts", 20, 40, std::make_shared<MatchAllDocsQuery>());
    const IndexSortSortedNumericDocValuesRangeQuery other_bounds("ts", 20, 41, std::make_shared<MatchAllDocsQuery>());
    EXPECT_TRUE(query.equals(same));
    EXPECT_EQ(query.hash(), same.hash());
    EXPECT_FALSE(query.equals(other_bounds));
}

TEST(IndexSortRangeQueryTest, ARewrittenFallbackIsCarriedOver) {
    auto [reader, leaf] = make_reader({10, 20, 30}, /*index_sorted=*/false);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // A fallback that rewrites to match-all takes the whole query with it.
    const IndexSortSortedNumericDocValuesRangeQuery match_all("ts", 20, 40, std::make_shared<MatchAllDocsQuery>());
    auto rewritten = match_all.rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << (rewritten ? "" : rewritten.error().to_string());
    ASSERT_NE(rewritten.value(), nullptr);
    EXPECT_NE(dynamic_cast<const MatchAllDocsQuery *>(rewritten.value().get()), nullptr);

    // The full range becomes a field-exists query.
    const IndexSortSortedNumericDocValuesRangeQuery everything("ts", std::numeric_limits<int64_t>::min(),
                                                              std::numeric_limits<int64_t>::max(),
                                                              std::make_shared<MatchAllDocsQuery>());
    auto exists = everything.rewrite(searcher);
    ASSERT_TRUE(exists.has_value());
    ASSERT_NE(exists.value(), nullptr);
    EXPECT_NE(dynamic_cast<const FieldExistsQuery *>(exists.value().get()), nullptr);
}
