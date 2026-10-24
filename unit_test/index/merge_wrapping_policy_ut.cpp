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
#include <optional>
#include <string>
#include <vector>

module lucene_core:ut.merge_wrapping_policy;

import lucene_core;

using namespace lucene;

namespace {

SegmentCommitInfo make_segment(std::string name, int32_t max_doc,
                               std::optional<Version> version = LUCENE_CURRENT_VERSION) {
    SegmentCommitInfo info;
    info.info.name = std::move(name);
    info.info.id = std::vector<uint8_t>{0, 0, 0, 0};
    info.info.version = version;
    info.info.max_doc = max_doc;
    return info;
}

SegmentInfos make_infos(std::vector<SegmentCommitInfo> segments) {
    SegmentInfos infos;
    for (SegmentCommitInfo &segment : segments) {
        infos.segments.push_back(std::move(segment));
    }
    return infos;
}

SimpleMergeContext make_context() { return SimpleMergeContext(InfoStreamMT{}, nullptr); }

std::vector<std::string> segment_names(const OneMerge &merge) {
    std::vector<std::string> names;
    names.reserve(merge.segments().size());
    for (const SegmentCommitInfo &info : merge.segments()) {
        names.push_back(info.info.name);
    }
    return names;
}

/// A policy that returns whatever the test scripts.
class ScriptedMergePolicy final : public MergePolicy {
public:
    [[nodiscard]] std::string to_string() const override { return "ScriptedMergePolicy"; }

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_merges(MergeTrigger /*merge_trigger*/, const SegmentInfos & /*infos*/,
                const MergeContext & /*context*/) const override {
        ++find_merges_calls;
        if (failure.has_value()) {
            return std::unexpected(failure.value());
        }
        return spec;
    }

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_forced_merges(const SegmentInfos & /*infos*/, int32_t /*max_num_segments*/,
                       const MergeContext & /*context*/) const override {
        ++forced_calls;
        if (failure.has_value()) {
            return std::unexpected(failure.value());
        }
        return forced_spec;
    }

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_forced_deletes_merges(const SegmentInfos & /*infos*/, const MergeContext & /*context*/) const override {
        ++forced_deletes_calls;
        return spec;
    }

    [[nodiscard]] bool use_compound_file(const SegmentInfos & /*infos*/, const SegmentCommitInfo & /*merged_info*/,
                                        const MergeContext & /*context*/) const override {
        return compound_files;
    }

    [[nodiscard]] int64_t max_full_flush_merge_size() const override { return full_flush_size; }

    std::optional<MergeSpecification> spec;
    std::optional<MergeSpecification> forced_spec;
    std::optional<LuceneError> failure;
    bool compound_files{false};
    int64_t full_flush_size{0};
    mutable int find_merges_calls{0};
    mutable int forced_calls{0};
    mutable int forced_deletes_calls{0};
};

} // namespace

// ------------------------------------------- OneMergeWrappingMergePolicy

TEST(OneMergeWrappingMergePolicyTest, RewritesEveryMerge) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    auto *script = upstream.get();
    MergeSpecification spec;
    spec.add(OneMerge({make_segment("_0", 2), make_segment("_1", 3)}));
    spec.add(OneMerge({make_segment("_2", 4), make_segment("_3", 5)}));
    script->spec = spec;

    int calls = 0;
    OneMergeWrappingMergePolicy policy(
            std::move(upstream), [&calls](OneMerge merge) -> Result<OneMerge> {
                ++calls;
                merge.merged_info.name = "wrapped";
                return merge;
            });

    const SegmentInfos infos = make_infos({make_segment("_0", 2)});
    const SimpleMergeContext context = make_context();
    auto merged = policy.find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    ASSERT_TRUE(merged.value().has_value());
    ASSERT_EQ(merged.value()->size(), 2U);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(script->find_merges_calls, 1);
    for (const OneMerge &merge : merged.value()->merges()) {
        EXPECT_EQ(merge.merged_info.name, "wrapped");
    }
    EXPECT_EQ(segment_names(merged.value()->merges()[0]), (std::vector<std::string>{"_0", "_1"}));
    EXPECT_EQ(policy.to_string(), "OneMergeWrappingMergePolicy(ScriptedMergePolicy)");
}

TEST(OneMergeWrappingMergePolicyTest, AnEmptySpecIsPassedThrough) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    int calls = 0;
    OneMergeWrappingMergePolicy policy(std::move(upstream), [&calls](OneMerge merge) -> Result<OneMerge> {
        ++calls;
        return merge;
    });

    const SegmentInfos infos = make_infos({make_segment("_0", 2)});
    const SimpleMergeContext context = make_context();
    auto merged = policy.find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value());
    EXPECT_FALSE(merged.value().has_value());
    EXPECT_EQ(calls, 0);
}

TEST(OneMergeWrappingMergePolicyTest, OperatorFailuresPropagate) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    MergeSpecification spec;
    spec.add(OneMerge({make_segment("_0", 2), make_segment("_1", 3)}));
    upstream->spec = spec;

    OneMergeWrappingMergePolicy policy(std::move(upstream), [](OneMerge) -> Result<OneMerge> {
        return std::unexpected(LuceneError::illegal_state("cannot wrap"));
    });

    const SegmentInfos infos = make_infos({make_segment("_0", 2)});
    const SimpleMergeContext context = make_context();
    auto merged = policy.find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_FALSE(merged.has_value());
    EXPECT_EQ(merged.error().kind(), ErrorKind::kIllegalState);
}

TEST(OneMergeWrappingMergePolicyTest, DelegateFailuresPropagate) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    upstream->failure = LuceneError::io("policy broke");
    OneMergeWrappingMergePolicy policy(std::move(upstream), [](OneMerge merge) -> Result<OneMerge> { return merge; });

    const SegmentInfos infos = make_infos({make_segment("_0", 2)});
    const SimpleMergeContext context = make_context();
    auto merged = policy.find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_FALSE(merged.has_value());
    EXPECT_EQ(merged.error().kind(), ErrorKind::kIo);
}

TEST(OneMergeWrappingMergePolicyTest, ForcedMergesAreWrappedToo) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    MergeSpecification spec;
    spec.add(OneMerge({make_segment("_0", 2), make_segment("_1", 3)}));
    upstream->forced_spec = spec;
    upstream->spec = spec;

    int calls = 0;
    OneMergeWrappingMergePolicy policy(std::move(upstream), [&calls](OneMerge merge) -> Result<OneMerge> {
        ++calls;
        merge.merged_info.name = "wrapped";
        return merge;
    });

    const SegmentInfos infos = make_infos({make_segment("_0", 2)});
    const SimpleMergeContext context = make_context();

    auto forced = policy.find_forced_merges(infos, 1, context);
    ASSERT_TRUE(forced.has_value()) << (forced ? "" : forced.error().to_string());
    ASSERT_TRUE(forced.value().has_value());
    EXPECT_EQ(forced.value()->merges()[0].merged_info.name, "wrapped");

    auto forced_deletes = policy.find_forced_deletes_merges(infos, context);
    ASSERT_TRUE(forced_deletes.has_value());
    ASSERT_TRUE(forced_deletes.value().has_value());
    EXPECT_EQ(forced_deletes.value()->merges()[0].merged_info.name, "wrapped");
    EXPECT_EQ(calls, 2);
}

TEST(OneMergeWrappingMergePolicyTest, DelegatesTheRemainingSettings) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    upstream->compound_files = true;
    upstream->full_flush_size = 42;
    OneMergeWrappingMergePolicy policy(std::move(upstream), [](OneMerge merge) -> Result<OneMerge> { return merge; });

    const SegmentInfos infos = make_infos({make_segment("_0", 2)});
    const SimpleMergeContext context = make_context();
    EXPECT_TRUE(policy.use_compound_file(infos, make_segment("_9", 1), context));
    EXPECT_EQ(policy.max_full_flush_merge_size(), 42);
    EXPECT_EQ(policy.delegate().to_string(), "ScriptedMergePolicy");
}

// ----------------------------------------------- UpgradeIndexMergePolicy

TEST(UpgradeIndexMergePolicyTest, DetectsSegmentsThatNeedAnUpgrade) {
    EXPECT_FALSE(UpgradeIndexMergePolicy::should_upgrade_segment(make_segment("_0", 1)));
    EXPECT_TRUE(UpgradeIndexMergePolicy::should_upgrade_segment(
            make_segment("_0", 1, Version{9, 0, 0})));
    EXPECT_TRUE(UpgradeIndexMergePolicy::should_upgrade_segment(make_segment("_0", 1, std::nullopt)));
}

TEST(UpgradeIndexMergePolicyTest, DelegatesWhenNothingIsOld) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    upstream->forced_spec = std::nullopt;
    UpgradeIndexMergePolicy policy(std::move(upstream));
    EXPECT_EQ(policy.to_string(), "UpgradeIndexMergePolicy");

    const SegmentInfos infos = make_infos({make_segment("_0", 2), make_segment("_1", 3)});
    const SimpleMergeContext context = make_context();
    auto merged = policy.find_forced_merges(infos, 1, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    EXPECT_FALSE(merged.value().has_value());
}

TEST(UpgradeIndexMergePolicyTest, AddsAMergeOverTheOldSegments) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    auto *script = upstream.get();
    UpgradeIndexMergePolicy policy(std::move(upstream));

    const SegmentInfos infos = make_infos({make_segment("_0", 2), make_segment("_1", 3, Version{9, 0, 0}),
                                           make_segment("_2", 4, Version{8, 0, 0})});
    const SimpleMergeContext context = make_context();
    auto merged = policy.find_forced_merges(infos, 1, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    ASSERT_TRUE(merged.value().has_value());
    ASSERT_EQ(merged.value()->size(), 1U);
    EXPECT_EQ(segment_names(merged.value()->merges()[0]), (std::vector<std::string>{"_1", "_2"}));
    EXPECT_EQ(script->forced_calls, 1);
}

TEST(UpgradeIndexMergePolicyTest, OnlyAddsTheSegmentsTheDelegateLeftAlone) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    MergeSpecification spec;
    spec.add(OneMerge({make_segment("_1", 3, Version{9, 0, 0})}));
    upstream->forced_spec = spec;
    UpgradeIndexMergePolicy policy(std::move(upstream));

    const SegmentInfos infos = make_infos({make_segment("_1", 3, Version{9, 0, 0}),
                                           make_segment("_2", 4, Version{8, 0, 0})});
    const SimpleMergeContext context = make_context();
    auto merged = policy.find_forced_merges(infos, 1, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    ASSERT_TRUE(merged.value().has_value());
    ASSERT_EQ(merged.value()->size(), 2U);
    EXPECT_EQ(segment_names(merged.value()->merges()[0]), (std::vector<std::string>{"_1"}))
            << "the delegate's merges come first";
    EXPECT_EQ(segment_names(merged.value()->merges()[1]), (std::vector<std::string>{"_2"}));
}

TEST(UpgradeIndexMergePolicyTest, InterceptsOnlyForcedMerges) {
    auto upstream = std::make_unique<ScriptedMergePolicy>();
    auto *script = upstream.get();
    MergeSpecification spec;
    spec.add(OneMerge({make_segment("_0", 2), make_segment("_1", 3)}));
    script->spec = spec;
    UpgradeIndexMergePolicy policy(std::move(upstream));

    const SegmentInfos infos = make_infos({make_segment("_0", 2), make_segment("_1", 3, Version{9, 0, 0})});
    const SimpleMergeContext context = make_context();

    auto merged = policy.find_merges(MergeTrigger::SegmentFlush, infos, context);
    ASSERT_TRUE(merged.has_value()) << (merged ? "" : merged.error().to_string());
    ASSERT_TRUE(merged.value().has_value());
    EXPECT_EQ(merged.value()->size(), 1U);
    EXPECT_EQ(script->find_merges_calls, 1);

    auto forced_deletes = policy.find_forced_deletes_merges(infos, context);
    ASSERT_TRUE(forced_deletes.has_value());
    EXPECT_EQ(script->forced_deletes_calls, 1);

    // `_1` is old and the delegate has no forced merge of its own, so the only
    // merge is the upgrade one.
    auto forced = policy.find_forced_merges(infos, 1, context);
    ASSERT_TRUE(forced.has_value()) << (forced ? "" : forced.error().to_string());
    ASSERT_TRUE(forced.value().has_value());
    ASSERT_EQ(forced.value()->size(), 1U);
    EXPECT_EQ(segment_names(forced.value()->merges()[0]), (std::vector<std::string>{"_1"}));
    EXPECT_EQ(policy.delegate().to_string(), "ScriptedMergePolicy");
}
