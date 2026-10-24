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
#include <vector>

module lucene_core:ut.index_file_deleter;

import lucene_core;

using namespace lucene;

namespace {

SegmentCommitInfo make_segment(std::string name, std::vector<std::string> files, int32_t max_doc = 3) {
    SegmentCommitInfo info;
    info.info.name = std::move(name);
    info.info.id = std::vector<uint8_t>(16, 0x11);
    info.info.version = LUCENE_CURRENT_VERSION;
    info.info.min_version = LUCENE_CURRENT_VERSION;
    info.info.max_doc = max_doc;
    for (const std::string &file : files) {
        info.info.files.insert(file);
    }
    return info;
}

VoidResult write_file(Directory &directory, std::string_view name) {
    auto output = directory.create_output(name, io_context_default());
    if (!output.has_value()) {
        return std::unexpected(std::move(output.error()));
    }
    if (auto written = output.value()->write_byte(0x2A); !written.has_value()) {
        return written;
    }
    return output.value()->close();
}

/// Writes the files of `segments` and the commit file of `generation`.
///
/// The `.si` files are written by the codec, because the deleter reads every
/// commit it finds and the segment info format has to parse.
Result<SegmentInfos> write_commit(Directory &directory, int64_t generation,
                                 std::vector<SegmentCommitInfo> segments) {
    auto codec = Lucene101Codec::create();
    if (!codec.has_value()) {
        return std::unexpected(std::move(codec.error()));
    }
    for (SegmentCommitInfo &segment : segments) {
        segment.info.codec_name = std::string(Lucene101Codec::CODEC_NAME);
        if (auto written = codec.value()->segment_info_format().write(directory, segment.info, io_context_default());
            !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        for (const std::string &file : segment.info.files) {
            if (file.ends_with(".si")) {
                continue; // the codec wrote it
            }
            if (auto written = write_file(directory, file); !written.has_value()) {
                return std::unexpected(std::move(written.error()));
            }
        }
    }
    SegmentInfos infos;
    infos.generation = generation;
    infos.counter = generation;
    infos.id = std::vector<uint8_t>(16, 0x22);
    infos.segments = std::move(segments);
    if (auto written = infos.write_to(directory, io_context_default()); !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }
    return infos;
}

bool contains(const std::vector<std::string> &names, std::string_view name) {
    for (const std::string &candidate : names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> files_of(const Directory &directory) {
    auto names = directory.list_all();
    EXPECT_TRUE(names.has_value());
    return names.has_value() ? names.value() : std::vector<std::string>{};
}

} // namespace

TEST(IndexFileDeleterTest, RemovesFilesNothingReferencesOnInit) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_file(directory, "_9.fdt").has_value()) << "a leftover from a crashed writer";
    auto infos = write_commit(directory, 1, {make_segment("_0", {"_0.si", "_0.fdt"})});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    KeepOnlyLastCommitDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, infos.value());
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());

    const std::vector<std::string> remaining = files_of(directory);
    EXPECT_FALSE(contains(remaining, "_9.fdt")) << "the unreferenced file is gone";
    EXPECT_TRUE(contains(remaining, "segments_1"));
    EXPECT_TRUE(contains(remaining, "_0.si"));
    EXPECT_TRUE(contains(remaining, "_0.fdt"));
    EXPECT_FALSE(deleter.value()->starting_commit_deleted());

    ASSERT_EQ(deleter.value()->commits().size(), 1U);
    EXPECT_EQ(deleter.value()->commits()[0]->generation(), 1);
    EXPECT_EQ(deleter.value()->commits()[0]->segments_file_name(), "segments_1");
    EXPECT_EQ(deleter.value()->commits()[0]->segment_count(), 1U);
}

TEST(IndexFileDeleterTest, KeepOnlyLastCommitDeletesTheOlderCommits) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_commit(directory, 1, {make_segment("_0", {"_0.si", "_0.fdt"})}).has_value());
    auto infos = write_commit(directory, 2, {make_segment("_0", {"_0.si", "_0.fdt"}),
                                             make_segment("_1", {"_1.si", "_1.fdt"})});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    KeepOnlyLastCommitDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, infos.value());
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());

    const std::vector<std::string> remaining = files_of(directory);
    EXPECT_FALSE(contains(remaining, "segments_1")) << "the old commit is gone";
    EXPECT_TRUE(contains(remaining, "segments_2"));
    EXPECT_TRUE(contains(remaining, "_0.si")) << "still referenced by the surviving commit";
    EXPECT_TRUE(contains(remaining, "_1.si"));
    ASSERT_EQ(deleter.value()->commits().size(), 1U);
    EXPECT_EQ(deleter.value()->commits()[0]->generation(), 2);
}

TEST(IndexFileDeleterTest, NoDeletionPolicyKeepsEveryCommit) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_commit(directory, 1, {make_segment("_0", {"_0.si"})}).has_value());
    auto infos = write_commit(directory, 2, {make_segment("_0", {"_0.si"}), make_segment("_1", {"_1.si"})});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    NoDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, infos.value());
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());

    ASSERT_EQ(deleter.value()->commits().size(), 2U);
    EXPECT_EQ(deleter.value()->commits()[0]->generation(), 1) << "oldest first";
    EXPECT_EQ(deleter.value()->commits()[1]->generation(), 2);
    const std::vector<std::string> remaining = files_of(directory);
    EXPECT_TRUE(contains(remaining, "segments_1"));
    EXPECT_TRUE(contains(remaining, "segments_2"));
}

TEST(IndexFileDeleterTest, CheckpointProtectsNewFilesAndReleasesThePrevious) {
    ByteBuffersDirectory directory;
    auto infos = write_commit(directory, 1, {make_segment("_0", {"_0.si", "_0.fdt"})});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    NoDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, infos.value());
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());

    // The writer flushes a new segment: its files are written, then checkpointed.
    ASSERT_TRUE(write_file(directory, "_1.si").has_value());
    ASSERT_TRUE(write_file(directory, "_1.fdt").has_value());
    SegmentInfos with_new_segment = infos.value();
    with_new_segment.segments.push_back(make_segment("_1", {"_1.si", "_1.fdt"}));

    ASSERT_TRUE(deleter.value()->checkpoint(with_new_segment, false, policy).has_value());
    EXPECT_TRUE(deleter.value()->exists("_1.si"));

    // A rollback drops the new segment again: the next checkpoint releases it.
    ASSERT_TRUE(deleter.value()->checkpoint(infos.value(), false, policy).has_value());
    const std::vector<std::string> remaining = files_of(directory);
    EXPECT_FALSE(contains(remaining, "_1.si")) << "the released file is deleted";
    EXPECT_FALSE(contains(remaining, "_1.fdt"));
    EXPECT_TRUE(contains(remaining, "_0.si")) << "the committed files stay";
}

TEST(IndexFileDeleterTest, CheckpointAsCommitRegistersTheCommitAndPrunes) {
    ByteBuffersDirectory directory;
    auto infos = write_commit(directory, 1, {make_segment("_0", {"_0.si"})});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    KeepOnlyLastCommitDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, infos.value());
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());
    ASSERT_EQ(deleter.value()->commits().size(), 1U);

    ASSERT_TRUE(write_file(directory, "_1.si").has_value());
    auto next = write_commit(directory, 2, {make_segment("_0", {"_0.si"}), make_segment("_1", {"_1.si"})});
    ASSERT_TRUE(next.has_value()) << (next ? "" : next.error().to_string());

    ASSERT_TRUE(deleter.value()->checkpoint(next.value(), true, policy).has_value());
    ASSERT_EQ(deleter.value()->commits().size(), 1U) << "the policy kept only the last commit";
    EXPECT_EQ(deleter.value()->commits()[0]->generation(), 2);

    const std::vector<std::string> remaining = files_of(directory);
    EXPECT_FALSE(contains(remaining, "segments_1"));
    EXPECT_TRUE(contains(remaining, "segments_2"));
    EXPECT_TRUE(contains(remaining, "_0.si"));
    EXPECT_TRUE(contains(remaining, "_1.si"));
}

TEST(IndexFileDeleterTest, RefreshRemovesNewlyCreatedUnreferencedFiles) {
    ByteBuffersDirectory directory;
    auto infos = write_commit(directory, 1, {make_segment("_0", {"_0.si"})});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    KeepOnlyLastCommitDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, infos.value());
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());

    ASSERT_TRUE(write_file(directory, "_5.fdt").has_value());
    ASSERT_TRUE(write_file(directory, "write.lock").has_value());
    ASSERT_TRUE(deleter.value()->refresh().has_value());

    const std::vector<std::string> remaining = files_of(directory);
    EXPECT_FALSE(contains(remaining, "_5.fdt"));
    EXPECT_TRUE(contains(remaining, "write.lock")) << "the lock is never touched";
    EXPECT_TRUE(contains(remaining, "_0.si"));
}

TEST(IndexFileDeleterTest, CloseReleasesTheLastCheckpoint) {
    ByteBuffersDirectory directory;
    auto infos = write_commit(directory, 1, {make_segment("_0", {"_0.si"})});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    NoDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, infos.value());
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());

    // The commit still references `_0.si`, so closing the deleter keeps it.
    ASSERT_TRUE(deleter.value()->close().has_value());
    EXPECT_TRUE(contains(files_of(directory), "_0.si"));
}

TEST(IndexFileDeleterTest, RevisitPolicyAppliesThePolicyAgain) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_commit(directory, 1, {make_segment("_0", {"_0.si"})}).has_value());
    auto infos = write_commit(directory, 2, {make_segment("_0", {"_0.si"}), make_segment("_1", {"_1.si"})});
    ASSERT_TRUE(infos.has_value()) << (infos ? "" : infos.error().to_string());

    NoDeletionPolicy policy;
    auto deleter = create_index_file_deleter(directory, policy, infos.value());
    ASSERT_TRUE(deleter.has_value()) << (deleter ? "" : deleter.error().to_string());
    ASSERT_EQ(deleter.value()->commits().size(), 2U);

    // Swapping the policy for the default one prunes the old commit on the next
    // revisit, which is what `IndexWriter.deleteUnusedFiles` does.
    KeepOnlyLastCommitDeletionPolicy pruner;
    ASSERT_TRUE(deleter.value()->revisit_policy(pruner).has_value());
    ASSERT_EQ(deleter.value()->commits().size(), 1U);
    EXPECT_FALSE(contains(files_of(directory), "segments_1"));
}

TEST(IndexFileDeleterTest, InflateGensPushesTheCountersPastTheDirectory) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_file(directory, "segments_5").has_value());
    ASSERT_TRUE(write_file(directory, "_0_7.liv").has_value());

    SegmentInfos infos;
    infos.generation = 0;
    infos.counter = 0;
    infos.segments.push_back(make_segment("_0", {"_0.si"}));

    std::vector<std::string> files = files_of(directory);
    ASSERT_TRUE(inflate_gens(infos, files, get_default_info_stream()).has_value());

    EXPECT_EQ(infos.generation, 5) << "the next commit cannot reuse segments_5";
    EXPECT_EQ(infos.counter, 1) << "segment _0 exists";
    ASSERT_EQ(infos.segments.size(), 1U);
    EXPECT_EQ(infos.segments[0].del_gen, 7) << "_0_7.liv is generation 7";
    EXPECT_EQ(infos.segments[0].field_infos_gen, 7);
    EXPECT_EQ(infos.segments[0].doc_values_gen, 7);
}

TEST(IndexFileDeleterTest, ClassifiesFilesAndCollectsTheCommitFiles) {
    EXPECT_TRUE(is_codec_file("_0.si"));
    EXPECT_TRUE(is_codec_file("_12_3.liv"));
    EXPECT_FALSE(is_codec_file("segments_1"));
    EXPECT_FALSE(is_codec_file("write.lock"));

    SegmentInfos infos;
    infos.generation = 4;
    SegmentCommitInfo segment = make_segment("_0", {"_0.si", "_0.fdt"});
    segment.field_infos_files.insert("_0_1.fnm");
    segment.doc_values_updates_files[2] = {"_0_2.dvd"};
    infos.segments.push_back(segment);

    const std::vector<std::string> all = commit_files(infos);
    EXPECT_EQ(all.size(), 5U); // segments_4 + .si + .fdt + .fnm + .dvd
    EXPECT_EQ(all[0], "segments_4");
    EXPECT_FALSE(contains(segment_files(infos), "segments_4"));
    EXPECT_EQ(segment_files(infos).size(), 4U);
}
