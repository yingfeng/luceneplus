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
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

module lucene_core:ut.spatial_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kPointField = "p";

/// Encodes one dimension of a point.
BytesRef pack_value(int32_t value) {
    std::vector<uint8_t> bytes(4, 0);
    NumericUtils::int_to_sortable_bytes(value, bytes, 0);
    return BytesRef(std::move(bytes));
}

/// The one-dimensional point values of the fake leaf: one point per document.
class FakePointValues final : public PointValues {
public:
    explicit FakePointValues(std::vector<int32_t> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override {
        int32_t min = values_.front();
        for (const int32_t value : values_) {
            min = std::min(min, value);
        }
        return std::optional<BytesRef>{pack_value(min)};
    }

    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override {
        int32_t max = values_.front();
        for (const int32_t value : values_) {
            max = std::max(max, value);
        }
        return std::optional<BytesRef>{pack_value(max)};
    }

    [[nodiscard]] Result<size_t> get_num_dimensions() const override { return 1; }
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override { return 1; }
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override { return 4; }
    [[nodiscard]] Result<size_t> size() const override { return values_.size(); }
    [[nodiscard]] Result<int32_t> get_doc_count() const override { return static_cast<int32_t>(values_.size()); }
    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override {
        return std::unique_ptr<PointTree>{};
    }

    /// A deliberately simple walk: the cell that covers everything is compared
    /// once, and every point below it is then offered to the visitor.
    [[nodiscard]] VoidResult intersect(IntersectVisitor &visitor) const override {
        auto min = get_min_packed_value();
        auto max = get_max_packed_value();
        if (!min.has_value() || !max.has_value()) {
            return err(LuceneError::illegal_state("no bounds"));
        }
        const BytesRef min_ref = min.value().value();
        const BytesRef max_ref = max.value().value();
        auto relation = visitor.compare(min_ref.as_slice(), max_ref.as_slice());
        if (!relation.has_value()) {
            return std::unexpected(std::move(relation.error()));
        }
        if (relation.value() == Relation::kCellOutsideQuery) {
            return {};
        }
        for (size_t doc = 0; doc < values_.size(); ++doc) {
            if (relation.value() == Relation::kCellInsideQuery) {
                if (auto visited = visitor.visit(static_cast<int32_t>(doc)); !visited.has_value()) {
                    return visited;
                }
            } else {
                const BytesRef packed = pack_value(values_[doc]);
                if (auto visited = visitor.visit_with_packed_value(static_cast<int32_t>(doc), packed.as_slice());
                    !visited.has_value()) {
                    return visited;
                }
            }
        }
        return {};
    }

private:
    std::vector<int32_t> values_;
};

/// A codec reader with a single one-dimensional point field.
class FakeCodecReader final : public CodecReader {
public:
    explicit FakeCodecReader(std::shared_ptr<FakePointValues> values) : values_(std::move(values)) {
        std::vector<FieldInfo> fields;
        FieldInfo field;
        field.number = 0;
        field.name = std::string(kPointField);
        field.point_dimension_count = 1;
        field.point_index_dimension_count = 1;
        field.point_num_bytes = 4;
        fields.push_back(field);
        auto built = FieldInfos::of(std::move(fields));
        EXPECT_TRUE(built.has_value()) << (built ? "" : built.error().to_string());
        infos_ = std::make_shared<FieldInfos>(std::move(built.value()));
    }

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return nullptr; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return nullptr; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return nullptr; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return nullptr; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return nullptr; }
    [[nodiscard]] PointsReader *get_points_reader() override { return nullptr; }

    [[nodiscard]] Result<std::shared_ptr<PointValues>> point_values(std::string_view field) override {
        if (field != kPointField) {
            return std::shared_ptr<PointValues>{};
        }
        return std::static_pointer_cast<PointValues>(values_);
    }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return nullptr; }
    [[nodiscard]] Result<int32_t> max_doc() const override { return values_->get_doc_count().value(); }
    [[nodiscard]] Result<int32_t> num_docs() const override { return values_->get_doc_count().value(); }

    [[nodiscard]] std::string to_string() const override { return "FakeCodecReader"; }

private:
    std::shared_ptr<FakePointValues> values_;
    std::shared_ptr<FieldInfos> infos_;
};

/// A spatial visitor over one dimension: the query is "the value equals `target`".
class FakeSpatialVisitor final : public SpatialVisitor {
public:
    explicit FakeSpatialVisitor(int32_t target) : target_(target) {}

    [[nodiscard]] Result<Relation> relate(std::span<const uint8_t> min_packed_value,
                                          std::span<const uint8_t> max_packed_value) const override {
        const int32_t min = NumericUtils::sortable_bytes_to_int(min_packed_value, 0);
        const int32_t max = NumericUtils::sortable_bytes_to_int(max_packed_value, 0);
        if (min == target_ && max == target_) {
            return Relation::kCellInsideQuery;
        }
        if (target_ < min || target_ > max) {
            return Relation::kCellOutsideQuery;
        }
        return Relation::kCellCrossesQuery;
    }

    [[nodiscard]] Result<bool> intersects(std::span<const uint8_t> packed_value) const override {
        return NumericUtils::sortable_bytes_to_int(packed_value, 0) == target_;
    }

    [[nodiscard]] Result<bool> within(std::span<const uint8_t> packed_value) const override {
        return NumericUtils::sortable_bytes_to_int(packed_value, 0) == target_;
    }

    [[nodiscard]] Result<WithinRelation> contains(std::span<const uint8_t> packed_value) const override {
        if (NumericUtils::sortable_bytes_to_int(packed_value, 0) == target_) {
            return WithinRelation::Candidate;
        }
        return WithinRelation::Disjoint;
    }

private:
    int32_t target_;
};

/// A placeholder geometry: the fake visitor does the geometry work itself.
class FakeGeometry final : public LatLonGeometry {
public:
    [[nodiscard]] Result<std::shared_ptr<Component2D>> to_component2d() const override {
        return std::shared_ptr<Component2D>(std::make_shared<Point2D>(0.0, 0.0));
    }
};

/// Hands out the fake visitor.
class FakeSpatialQueryBase final : public SpatialQueryBase {
public:
    explicit FakeSpatialQueryBase(int32_t target) : target_(target) {}

    [[nodiscard]] Result<std::unique_ptr<SpatialVisitor>> spatial_visitor() const override {
        return std::unique_ptr<SpatialVisitor>(std::make_unique<FakeSpatialVisitor>(target_));
    }

private:
    int32_t target_;
};

/// Searches with `query` and returns the document ids it matches, in order.
std::vector<int32_t> search_docs(const SpatialQuery &query) {
    auto values = std::make_shared<FakePointValues>(std::vector<int32_t>{10, 20, 30, 40});
    std::shared_ptr<CodecReader> leaf(new FakeCodecReader(values));
    std::shared_ptr<IndexReader> reader = leaf;
    IndexSearcher searcher(reader);

    auto weight = query.create_weight(searcher, ScoreMode::TopScores, 1.0F);
    EXPECT_TRUE(weight.has_value()) << (weight ? "" : weight.error().to_string());
    if (!weight.has_value()) {
        return {};
    }
    auto leaves = reader->leaves();
    EXPECT_TRUE(leaves.has_value());
    if (!leaves.has_value() || leaves.value().empty()) {
        return {};
    }
    auto supplier = weight.value()->scorer_supplier(leaves.value()[0], searcher);
    EXPECT_TRUE(supplier.has_value()) << (supplier ? "" : supplier.error().to_string());
    if (!supplier.has_value() || supplier.value() == nullptr) {
        return {};
    }
    auto scorer = supplier.value()->get(std::numeric_limits<int64_t>::max(), leaves.value()[0], searcher);
    EXPECT_TRUE(scorer.has_value()) << (scorer ? "" : scorer.error().to_string());
    if (!scorer.has_value()) {
        return {};
    }

    std::vector<int32_t> docs;
    while (true) {
        auto doc = scorer.value()->next_doc();
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

std::unique_ptr<SpatialQuery> make_query(QueryRelation relation, int32_t target,
                                         std::shared_ptr<const Geometry> geometry = nullptr) {
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(geometry == nullptr ? std::make_shared<FakeGeometry>() : std::move(geometry));
    auto query = SpatialQuery::create(std::string(kPointField), relation, std::move(geometries),
                                     std::make_shared<FakeSpatialQueryBase>(target));
    EXPECT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());
    return query.has_value() ? std::move(query.value()) : nullptr;
}

} // namespace

TEST(SpatialQueryTest, TransposesTheRelations) {
    EXPECT_EQ(transpose_relation(Relation::kCellInsideQuery), Relation::kCellOutsideQuery);
    EXPECT_EQ(transpose_relation(Relation::kCellOutsideQuery), Relation::kCellInsideQuery);
    EXPECT_EQ(transpose_relation(Relation::kCellCrossesQuery), Relation::kCellCrossesQuery);
}

TEST(SpatialQueryTest, ValidatesItsArguments) {
    std::vector<std::shared_ptr<const Geometry>> geometries;
    geometries.push_back(std::make_shared<FakeGeometry>());
    auto sub = std::make_shared<FakeSpatialQueryBase>(10);

    auto no_field = SpatialQuery::create("", QueryRelation::Intersects, geometries, sub);
    ASSERT_FALSE(no_field.has_value());
    EXPECT_EQ(no_field.error().kind(), ErrorKind::kIllegalArgument);

    auto no_geometries = SpatialQuery::create(std::string(kPointField), QueryRelation::Intersects, {}, sub);
    ASSERT_FALSE(no_geometries.has_value());
    EXPECT_EQ(no_geometries.error().kind(), ErrorKind::kIllegalArgument);

    auto no_sub = SpatialQuery::create(std::string(kPointField), QueryRelation::Intersects, geometries, nullptr);
    ASSERT_FALSE(no_sub.has_value());
    EXPECT_EQ(no_sub.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SpatialQueryTest, MatchesTheDocumentsThatIntersect) {
    // Only the document whose value is 20.
    auto query = make_query(QueryRelation::Intersects, 20);
    ASSERT_NE(query, nullptr);
    EXPECT_EQ(search_docs(*query), (std::vector<int32_t>{1}));

    // A value that no document has matches nothing.
    auto missing = make_query(QueryRelation::Intersects, 25);
    ASSERT_NE(missing, nullptr);
    EXPECT_TRUE(search_docs(*missing).empty());
}

TEST(SpatialQueryTest, MatchesTheDocumentsThatAreDisjoint) {
    // Everything but the document whose value is 20.
    auto query = make_query(QueryRelation::Disjoint, 20);
    ASSERT_NE(query, nullptr);
    EXPECT_EQ(search_docs(*query), (std::vector<int32_t>{0, 2, 3}));
}

TEST(SpatialQueryTest, MatchesTheDocumentsThatAreWithin) {
    auto query = make_query(QueryRelation::Within, 30);
    ASSERT_NE(query, nullptr);
    EXPECT_EQ(search_docs(*query), (std::vector<int32_t>{2}));
}

TEST(SpatialQueryTest, MatchesTheDocumentsThatContain) {
    auto query = make_query(QueryRelation::Contains, 10);
    ASSERT_NE(query, nullptr);
    EXPECT_EQ(search_docs(*query), (std::vector<int32_t>{0}));
}

TEST(SpatialQueryTest, DescribesItself) {
    auto query = make_query(QueryRelation::Within, 20);
    ASSERT_NE(query, nullptr);

    // The field is printed when the caller passes a different one.
    auto text = query->to_string("");
    ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().to_string());
    EXPECT_EQ(text.value(), "SpatialQuery: field=p:[Within, geometries=1]");

    // A query over the same field prints just the relation and the shapes.
    auto with_field = query->to_string(std::string(kPointField));
    ASSERT_TRUE(with_field.has_value());
    EXPECT_EQ(with_field.value(), "SpatialQuery:[Within, geometries=1]");

    // Geometries are compared by identity (see the deviation), so the second
    // query has to share the shape of the first one.
    auto same = make_query(QueryRelation::Within, 20, query->geometries()[0]);
    ASSERT_NE(same, nullptr);
    EXPECT_TRUE(query->equals(*same));
    EXPECT_EQ(query->hash(), same->hash());

    auto other = make_query(QueryRelation::Intersects, 20);
    ASSERT_NE(other, nullptr);
    EXPECT_FALSE(query->equals(*other));
}

TEST(SpatialQueryTest, VisitsTheField) {
    auto query = make_query(QueryRelation::Intersects, 20);
    ASSERT_NE(query, nullptr);

    // The default visitor accepts every field, so the leaf is visited.
    QueryVisitor visitor;
    EXPECT_TRUE(query->visit(visitor).has_value());
}
