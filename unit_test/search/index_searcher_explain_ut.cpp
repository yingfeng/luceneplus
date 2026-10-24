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

#include <cmath>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

module lucene_core:ut.index_searcher_explain;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

/// Two documents: the first holds "hello" twice, the second once.
std::shared_ptr<DirectoryReader> build_index(ByteBuffersDirectory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const char *title : {"hello hello", "hello", "other"}) {
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

double as_double(const Explanation &explanation) {
    return std::visit([](auto value) { return static_cast<double>(value); }, explanation.value());
}

std::string describe(const Explanation &explanation) {
    std::string text = explanation.description();
    for (const Explanation &detail : explanation.details()) {
        text += " | " + describe(detail);
    }
    return text;
}

} // namespace

TEST(IndexSearcherExplainTest, ExplainsAMatchingDocument) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const TermQuery query(Term("title", BytesRef::from_string("hello")));
    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    ASSERT_EQ(top.value().score_docs.size(), 2U);

    const ScoreDoc &best = top.value().score_docs[0];
    auto explained = searcher.explain(query, best.doc);
    ASSERT_TRUE(explained.has_value()) << (explained ? "" : explained.error().to_string());
    EXPECT_TRUE(explained.value().is_match());
    EXPECT_NEAR(as_double(explained.value()), static_cast<double>(best.score), 1e-4)
            << "the explanation must agree with the score the search reported";
    EXPECT_FALSE(explained.value().description().empty()) << "an explanation carries a description";
    EXPECT_FALSE(describe(explained.value()).empty());
}

TEST(IndexSearcherExplainTest, ADocumentWithoutTheTermDoesNotMatch) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // Document 2 has "other" only.
    const TermQuery query(Term("title", BytesRef::from_string("hello")));
    auto explained = searcher.explain(query, 2);
    ASSERT_TRUE(explained.has_value()) << (explained ? "" : explained.error().to_string());
    EXPECT_FALSE(explained.value().is_match());
}

TEST(IndexSearcherExplainTest, RejectsAnOutOfBoundsDocument) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const TermQuery query(Term("title", BytesRef::from_string("hello")));
    auto too_high = searcher.explain(query, 3);
    ASSERT_FALSE(too_high.has_value());
    EXPECT_EQ(too_high.error().kind(), ErrorKind::kIllegalArgument);

    auto negative = searcher.explain(query, -1);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(IndexSearcherExplainTest, ExplainsAHit) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const TermQuery query(Term("title", BytesRef::from_string("hello")));
    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value());
    ASSERT_EQ(top.value().score_docs.size(), 2U);

    for (const ScoreDoc &hit : top.value().score_docs) {
        auto explained = searcher.explain(query, hit);
        ASSERT_TRUE(explained.has_value()) << (explained ? "" : explained.error().to_string());
        EXPECT_TRUE(explained.value().is_match());
        EXPECT_NEAR(as_double(explained.value()), static_cast<double>(hit.score), 1e-4);
    }

    // The document with two occurrences scores higher, and the explanation says so.
    auto better = searcher.explain(query, top.value().score_docs[0]);
    auto worse = searcher.explain(query, top.value().score_docs[1]);
    ASSERT_TRUE(better.has_value());
    ASSERT_TRUE(worse.has_value());
    EXPECT_GT(as_double(better.value()), as_double(worse.value()));
}

TEST(IndexSearcherExplainTest, ExplainsThroughARewrite) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // A single-term synonym group rewrites to a term query before the weight is
    // built, so this exercises the rewrite path of `explain`.
    SynonymQuery::Builder builder;
    ASSERT_TRUE(builder.add_term(Term("title", BytesRef::from_string("hello"))).has_value());
    const SynonymQuery query = builder.build();

    auto explained = searcher.explain(query, 0);
    ASSERT_TRUE(explained.has_value()) << (explained ? "" : explained.error().to_string());
    EXPECT_TRUE(explained.value().is_match());
    EXPECT_GT(as_double(explained.value()), 0.0);
}
