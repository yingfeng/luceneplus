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
#include <utility>

module lucene_core:ut.index_searcher_rewrite;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

std::shared_ptr<DirectoryReader> build_index(ByteBuffersDirectory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const char *title : {"quick brown", "quick fox", "slow brown"}) {
        Document doc;
        doc.add(std::make_unique<Field>("title", FieldValue{std::string(title)}, text_type()));
        EXPECT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    }
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

} // namespace

TEST(IndexSearcherRewriteTest, AScoresRewriteIsThePlainRewrite) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const PrefixQuery query("title", "qui");
    auto plain = searcher.rewrite(query);
    ASSERT_TRUE(plain.has_value()) << (plain ? "" : plain.error().to_string());

    auto with_scores = searcher.rewrite_with_needs_scores(query, /*needs_scores=*/true);
    ASSERT_TRUE(with_scores.has_value()) << (with_scores ? "" : with_scores.error().to_string());
    ASSERT_NE(with_scores.value(), nullptr);
    ASSERT_NE(plain.value(), nullptr);
    EXPECT_EQ(with_scores.value()->to_string("").value(), plain.value()->to_string("").value());
}

TEST(IndexSearcherRewriteTest, AScorelessRewriteGoesThroughConstantScore) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const PrefixQuery query("title", "qui");
    auto plain = searcher.rewrite(query);
    ASSERT_TRUE(plain.has_value());

    auto scoreless = searcher.rewrite_with_needs_scores(query, /*needs_scores=*/false);
    ASSERT_TRUE(scoreless.has_value()) << (scoreless ? "" : scoreless.error().to_string());
    ASSERT_NE(scoreless.value(), nullptr) << "the wrapper is part of the answer when nothing rewrites";
    EXPECT_NE(scoreless.value()->to_string("").value(), plain.value()->to_string("").value())
            << "the scoreless form keeps the constant-score wrapper's rewrite rules";
}

TEST(IndexSearcherRewriteTest, ScorelessSearchStillMatchesTheSameDocuments) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // `count` runs a collector that does not need scores, so it takes the
    // scoreless rewrite; the matches must not change.
    const PrefixQuery query("title", "qui");
    auto count = searcher.count(query);
    ASSERT_TRUE(count.has_value()) << (count ? "" : count.error().to_string());
    EXPECT_EQ(count.value(), 2);

    // A search that needs scores keeps the plain rewrite.
    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    EXPECT_EQ(top.value().score_docs.size(), 2U);
}

TEST(IndexSearcherRewriteTest, AScoringQueryIsUnaffected) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // A term query rewrites to nothing, so the scoreless path answers the wrapper.
    const TermQuery query(Term("title", BytesRef::from_string("brown")));
    auto with_scores = searcher.rewrite_with_needs_scores(query, /*needs_scores=*/true);
    ASSERT_TRUE(with_scores.has_value());
    EXPECT_EQ(with_scores.value(), nullptr) << "a term query is already final";

    auto scoreless = searcher.rewrite_with_needs_scores(query, /*needs_scores=*/false);
    ASSERT_TRUE(scoreless.has_value()) << (scoreless ? "" : scoreless.error().to_string());
    ASSERT_NE(scoreless.value(), nullptr);
    EXPECT_NE(scoreless.value()->to_string("").value().find("ConstantScore"), std::string::npos);

    EXPECT_EQ(searcher.count(query).value(), 2);
}
