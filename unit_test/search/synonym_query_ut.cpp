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
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.synonym_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTitleField = "title";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

void add_text(Document &doc, std::string_view name, std::string value) {
    doc.add(std::make_unique<Field>(std::string(name), FieldValue{std::move(value)}, text_type()));
}

Document make_doc(std::string id, std::string title) {
    Document doc;
    add_text(doc, kIdField, std::move(id));
    add_text(doc, kTitleField, std::move(title));
    return doc;
}

/// Builds an index from `docs`, one commit, and returns a reader over it.
///
/// Variadic because [`Document`] is move-only, so it cannot go through a
/// `std::vector`.
template <class... Docs>
std::shared_ptr<DirectoryReader> index_docs(ByteBuffersDirectory &directory, Docs &&...docs) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    auto add = [&writer](Document &&doc) {
        EXPECT_TRUE(writer.value()->add_document(std::move(doc)).has_value());
    };
    (add(std::forward<Docs>(docs)), ...);
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->close().has_value());

    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

SynonymQuery synonym(std::string_view field, std::initializer_list<std::string_view> terms) {
    SynonymQuery::Builder builder;
    for (std::string_view term : terms) {
        EXPECT_TRUE(builder.add_term(Term(std::string(field), BytesRef::from_string(term))).has_value());
    }
    return builder.build();
}

} // namespace

TEST(SynonymQueryTest, MatchesAnyOfTheTerms) {
    ByteBuffersDirectory directory;
    auto reader = index_docs(directory, make_doc("1", "brown"), make_doc("2", "lazy"), make_doc("3", "quick"));
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const SynonymQuery query = synonym(kTitleField, {"brown", "lazy"});
    auto top = searcher.search(query, 10);
    ASSERT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    EXPECT_EQ(top.value().score_docs.size(), 2U) << "both terms of the group match";
    EXPECT_EQ(searcher.count(query).value(), 2);
}

TEST(SynonymQueryTest, ScoresTheSummedTermFrequency) {
    // The first document contains the term twice, so the group scores it higher
    // than the document that contains it once: the similarity sees one term with
    // frequency 2 instead of two separate terms.
    ByteBuffersDirectory directory;
    auto reader = index_docs(directory, make_doc("1", "brown brown"), make_doc("2", "brown"));
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    auto top = searcher.search(synonym(kTitleField, {"brown", "lazy"}), 10);
    ASSERT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    ASSERT_EQ(top.value().score_docs.size(), 2U);
    ASSERT_EQ(top.value().score_docs[0].doc, 0) << "the document with the higher frequency leads";
    EXPECT_GT(top.value().score_docs[0].score, top.value().score_docs[1].score);
}

TEST(SynonymQueryTest, MatchesWithoutScores) {
    // No scores means the rewrite to a boolean disjunction takes over; the
    // documents must be the same.
    ByteBuffersDirectory directory;
    auto reader = index_docs(directory, make_doc("1", "brown"), make_doc("2", "lazy"), make_doc("3", "quick"));
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const SynonymQuery query = synonym(kTitleField, {"brown", "lazy"});
    auto weight = query.create_weight(searcher, ScoreMode::CompleteNoScores, 1.0F);
    ASSERT_TRUE(weight.has_value()) << (weight ? "" : weight.error().to_string());
    EXPECT_EQ(weight.value()->get_query().to_string("").value(), query.to_string("").value());
}

TEST(SynonymQueryTest, PrintsTheGroup) {
    const SynonymQuery query = synonym(kTitleField, {"brown", "lazy"});
    auto printed = query.to_string("");
    ASSERT_TRUE(printed.has_value()) << (printed ? "" : printed.error().to_string());
    EXPECT_EQ(printed.value(), "Synonym(title:brown title:lazy)");

    SynonymQuery::Builder boosted;
    ASSERT_TRUE(boosted.add_term(Term(std::string(kTitleField), BytesRef::from_string("brown")), 2.0F).has_value());
    auto with_boost = boosted.build().to_string("");
    ASSERT_TRUE(with_boost.has_value());
    EXPECT_EQ(with_boost.value(), "Synonym(title:brown^2.0)");
}

TEST(SynonymQueryTest, TheBuilderKeepsTheGroupOnOneField) {
    SynonymQuery::Builder builder;
    ASSERT_TRUE(builder.add_term(Term(std::string(kTitleField), BytesRef::from_string("brown"))).has_value());

    auto mixed = builder.add_term(Term(std::string(kIdField), BytesRef::from_string("1")));
    ASSERT_FALSE(mixed.has_value());
    EXPECT_EQ(mixed.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SynonymQueryTest, RewriteRules) {
    ByteBuffersDirectory directory;
    auto reader = index_docs(directory, make_doc("1", "brown"));
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    // An empty group matches nothing.
    auto empty = synonym(kTitleField, {}).rewrite(searcher);
    ASSERT_TRUE(empty.has_value());
    ASSERT_NE(empty.value(), nullptr);
    EXPECT_NE(dynamic_cast<const BooleanQuery *>(empty.value().get()), nullptr);

    // One unboosted term is just that term.
    auto single = synonym(kTitleField, {"brown"}).rewrite(searcher);
    ASSERT_TRUE(single.has_value());
    ASSERT_NE(single.value(), nullptr);
    EXPECT_NE(dynamic_cast<const TermQuery *>(single.value().get()), nullptr);

    // Several terms are already final.
    auto group = synonym(kTitleField, {"brown", "lazy"}).rewrite(searcher);
    ASSERT_TRUE(group.has_value());
    EXPECT_EQ(group.value(), nullptr);
}

TEST(SynonymQueryTest, EqualsAndHashFollowTheGroup) {
    const SynonymQuery first = synonym(kTitleField, {"brown", "lazy"});
    const SynonymQuery same = synonym(kTitleField, {"brown", "lazy"});
    const SynonymQuery other_terms = synonym(kTitleField, {"brown", "quick"});
    const SynonymQuery other_field = synonym(kIdField, {"brown", "lazy"});

    EXPECT_TRUE(first.equals(same));
    EXPECT_EQ(first.hash(), same.hash());
    EXPECT_FALSE(first.equals(other_terms));
    EXPECT_FALSE(first.equals(other_field));
    EXPECT_FALSE(first.equals(TermQuery(Term(std::string(kTitleField), BytesRef::from_string("brown")))));
}

TEST(SynonymQueryTest, ExplainReportsTheSummedFrequency) {
    ByteBuffersDirectory directory;
    auto reader = index_docs(directory, make_doc("1", "brown brown"), make_doc("2", "brown"));
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    const SynonymQuery query = synonym(kTitleField, {"brown", "lazy"});
    auto weight = query.create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(weight.has_value()) << (weight ? "" : weight.error().to_string());

    auto leaves = reader->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_FALSE(leaves.value().empty());

    auto explained = weight.value()->explain(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(explained.has_value()) << (explained ? "" : explained.error().to_string());
    EXPECT_TRUE(explained.value().is_match());
    const double score = std::visit([](auto value) { return static_cast<double>(value); }, explained.value().value());
    EXPECT_GT(score, 0.0) << "the group scores a matching document";

    // A document without any term of the group does not match.
    SynonymQuery::Builder unrelated;
    ASSERT_TRUE(unrelated.add_term(Term(std::string(kTitleField), BytesRef::from_string("zzz"))).has_value());
    auto missing_weight = unrelated.build().create_weight(searcher, ScoreMode::Complete, 1.0F);
    ASSERT_TRUE(missing_weight.has_value());
    auto no_match = missing_weight.value()->explain(leaves.value()[0], 0, searcher);
    ASSERT_TRUE(no_match.has_value());
    EXPECT_FALSE(no_match.value().is_match());
}
