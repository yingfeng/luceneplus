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
#include <vector>

module lucene_core:ut.multi_collector_manager;

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqs).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

/// A two-document index whose `title` holds "hello" twice.
std::shared_ptr<DirectoryReader> build_index(ByteBuffersDirectory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return nullptr;
    }
    for (const char *title : {"hello world", "hello"}) {
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

std::shared_ptr<CollectorManager> manager_for(size_t num_hits) {
    auto manager = TopScoreDocCollectorManager::create(num_hits, /*total_hits_threshold=*/0);
    EXPECT_TRUE(manager.has_value()) << (manager ? "" : manager.error().to_string());
    if (!manager.has_value()) {
        return nullptr;
    }
    return std::shared_ptr<CollectorManager>(std::move(manager.value()));
}

} // namespace

TEST(MultiCollectorManagerTest, NeedsAtLeastOneManager) {
    auto empty = MultiCollectorManager::create({});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind(), ErrorKind::kIllegalArgument);

    auto null_manager = MultiCollectorManager::create({nullptr});
    ASSERT_FALSE(null_manager.has_value());
    EXPECT_EQ(null_manager.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(MultiCollectorManagerTest, CreatesOneCollectorPerManager) {
    auto multi = MultiCollectorManager::create({manager_for(1), manager_for(2)});
    ASSERT_TRUE(multi.has_value()) << (multi ? "" : multi.error().to_string());
    EXPECT_EQ(multi.value()->size(), 2U);

    auto collector = multi.value()->new_collector();
    ASSERT_TRUE(collector.has_value()) << (collector ? "" : collector.error().to_string());
    EXPECT_NE(collector.value(), nullptr);
}

TEST(MultiCollectorManagerTest, ReducesOneResultPerManager) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    auto multi = MultiCollectorManager::create({manager_for(1), manager_for(10)});
    ASSERT_TRUE(multi.has_value()) << (multi ? "" : multi.error().to_string());

    auto collector = multi.value()->new_collector();
    ASSERT_TRUE(collector.has_value());

    const TermQuery query(Term("title", BytesRef::from_string("hello")));
    ASSERT_TRUE(searcher.search_with_collector(query, *collector.value()).has_value());

    std::vector<std::unique_ptr<Collector>> collected;
    collected.push_back(std::move(collector.value()));
    auto results = multi.value()->reduce(std::move(collected));
    ASSERT_TRUE(results.has_value()) << (results ? "" : results.error().to_string());
    ASSERT_EQ(results.value().size(), 2U) << "one result per wrapped manager";

    // The first manager keeps one hit, the second keeps them all.
    EXPECT_EQ(results.value()[0].score_docs.size(), 1U);
    EXPECT_EQ(results.value()[0].total_hits.value, 2) << "the total hit count is still the full one";
    EXPECT_EQ(results.value()[1].score_docs.size(), 2U);
}

TEST(MultiCollectorManagerTest, DrivesTheSearcherAndReturnsTheFirstResult) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    IndexSearcher searcher(reader);

    auto multi = MultiCollectorManager::create({manager_for(1), manager_for(10)});
    ASSERT_TRUE(multi.has_value()) << (multi ? "" : multi.error().to_string());

    const TermQuery query(Term("title", BytesRef::from_string("hello")));
    auto top = searcher.search_with_collector_manager(query, *multi.value());
    ASSERT_TRUE(top.has_value()) << (top ? "" : top.error().to_string());
    EXPECT_EQ(top.value().score_docs.size(), 1U) << "the first manager's result comes back";
}

TEST(MultiCollectorManagerTest, RejectsCollectorsFromElsewhere) {
    auto multi = MultiCollectorManager::create({manager_for(1)});
    ASSERT_TRUE(multi.has_value());

    // A plain collector of another manager cannot be split.
    auto other = manager_for(1);
    ASSERT_NE(other, nullptr);
    auto foreign = other->new_collector();
    ASSERT_TRUE(foreign.has_value());

    std::vector<std::unique_ptr<Collector>> collected;
    collected.push_back(std::move(foreign.value()));
    auto results = multi.value()->reduce(std::move(collected));
    ASSERT_FALSE(results.has_value());
    EXPECT_EQ(results.error().kind(), ErrorKind::kIllegalState);
}
