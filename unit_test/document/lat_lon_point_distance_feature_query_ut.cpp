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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.lat_lon_point_distance_feature_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kPointField = "loc";

/// Indexes one location per entry, as both a point and a doc value (which is what
/// the feature query needs), and opens the reader.
std::shared_ptr<DirectoryReader> index_locations(const std::vector<std::pair<double, double>> &locations,
                                                 bool with_missing) {
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }

    for (const auto &[lat, lon] : locations) {
        Document doc;
        auto point = LatLonPoint::create(std::string(kPointField), lat, lon);
        EXPECT_TRUE(point.has_value()) << (point ? "" : point.error().to_string());
        doc.add(std::make_unique<Field>(std::move(point.value())));

        auto values = LatLonDocValuesField::create(std::string(kPointField), lat, lon);
        EXPECT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
        doc.add(std::make_unique<Field>(std::move(values.value())));
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }
    if (with_missing) {
        Document doc;
        EXPECT_TRUE(writer.value()->add_document(doc).has_value());
    }

    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(*dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(LatLonDistanceFeatureQueryTest, ValidatesItsArguments) {
    auto no_field = LatLonPointDistanceFeatureQuery::create("", 0.0, 0.0, 1000.0);
    ASSERT_FALSE(no_field.has_value());
    EXPECT_EQ(no_field.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lat = LatLonPointDistanceFeatureQuery::create(std::string(kPointField), 91.0, 0.0, 1000.0);
    ASSERT_FALSE(bad_lat.has_value());
    EXPECT_EQ(bad_lat.error().kind(), ErrorKind::kIllegalArgument);

    auto bad_lon = LatLonPointDistanceFeatureQuery::create(std::string(kPointField), 0.0, 181.0, 1000.0);
    ASSERT_FALSE(bad_lon.has_value());
    EXPECT_EQ(bad_lon.error().kind(), ErrorKind::kIllegalArgument);

    auto zero_pivot = LatLonPointDistanceFeatureQuery::create(std::string(kPointField), 0.0, 0.0, 0.0);
    ASSERT_FALSE(zero_pivot.has_value());
    EXPECT_EQ(zero_pivot.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(LatLonDistanceFeatureQueryTest, DescribesItself) {
    auto query = LatLonPointDistanceFeatureQuery::create(std::string(kPointField), 12.5, -30.25, 1000.0);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());

    auto text = query.value()->to_string("");
    ASSERT_TRUE(text.has_value()) << (text ? "" : text.error().to_string());
    EXPECT_EQ(text.value(),
              "LatLonPointDistanceFeatureQuery(field=loc,originLat=12.5,originLon=-30.25,pivotDistance=1000)");

    auto same = LatLonPointDistanceFeatureQuery::create(std::string(kPointField), 12.5, -30.25, 1000.0);
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(query.value()->equals(*same.value()));
    EXPECT_EQ(query.value()->hash(), same.value()->hash());

    auto other = LatLonPointDistanceFeatureQuery::create(std::string(kPointField), 12.5, -30.25, 2000.0);
    ASSERT_TRUE(other.has_value());
    EXPECT_FALSE(query.value()->equals(*other.value()));
}

TEST(LatLonDistanceFeatureQueryTest, ScoresTheDocumentsByDistance) {
    // One document at the origin, one about 1.1 km north, one far away.
    auto reader = index_locations({{0.0, 0.0}, {0.01, 0.0}, {10.0, 10.0}}, true);
    ASSERT_NE(reader, nullptr);

    auto query = LatLonPointDistanceFeatureQuery::create(std::string(kPointField), 0.0, 0.0, 1000.0);
    ASSERT_TRUE(query.has_value()) << (query ? "" : query.error().to_string());

    IndexSearcher searcher(reader);
    auto hits = searcher.search(*query.value(), 10);
    ASSERT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());

    // The empty document does not match; the others are ordered by distance.
    ASSERT_EQ(hits.value().score_docs.size(), 3U);
    EXPECT_FLOAT_EQ(hits.value().score_docs[0].score, 1.0F);
    // 0.01 degrees of latitude is about 1111.9 m, so the score is
    // 1000 / (1000 + 1111.9).
    EXPECT_NEAR(hits.value().score_docs[1].score, 1000.0 / 2111.9, 1e-3);
    EXPECT_LT(hits.value().score_docs[2].score, hits.value().score_docs[1].score);
    EXPECT_GT(hits.value().score_docs[2].score, 0.0F);
}

TEST(LatLonDistanceFeatureQueryTest, BoostsTheScoreWithTheWeight) {
    auto reader = index_locations({{0.0, 0.0}, {0.01, 0.0}}, false);
    ASSERT_NE(reader, nullptr);

    // The facade wraps a weight other than 1 in a boost query.
    auto boosted = LatLonPoint::new_distance_feature_query(std::string(kPointField), 2.0F, 0.0, 0.0, 1000.0);
    ASSERT_TRUE(boosted.has_value()) << (boosted ? "" : boosted.error().to_string());

    IndexSearcher searcher(reader);
    auto hits = searcher.search(*boosted.value(), 10);
    ASSERT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    ASSERT_EQ(hits.value().score_docs.size(), 2U);
    EXPECT_FLOAT_EQ(hits.value().score_docs[0].score, 2.0F);
}

TEST(LatLonDistanceFeatureQueryTest, FailsWhenTheFieldHasNoPoints) {
    // A doc values only field: the query needs the point index to exist.
    auto dir = std::make_shared<ByteBuffersDirectory>();
    WhitespaceAnalyzer analyzer;
    auto writer = IndexWriter::open(*dir, IndexWriterConfig(analyzer));
    ASSERT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    Document doc;
    auto values = LatLonDocValuesField::create(std::string(kPointField), 0.0, 0.0);
    ASSERT_TRUE(values.has_value());
    doc.add(std::make_unique<Field>(std::move(values.value())));
    ASSERT_TRUE(writer.value()->add_document(doc).has_value());
    ASSERT_TRUE(writer.value()->commit().has_value());

    auto reader = DirectoryReader::open(*dir);
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    auto query = LatLonPointDistanceFeatureQuery::create(std::string(kPointField), 0.0, 0.0, 1000.0);
    ASSERT_TRUE(query.has_value());

    IndexSearcher searcher(reader.value());
    auto hits = searcher.search(*query.value(), 10);
    ASSERT_TRUE(hits.has_value()) << (hits ? "" : hits.error().to_string());
    EXPECT_TRUE(hits.value().score_docs.empty());
}
