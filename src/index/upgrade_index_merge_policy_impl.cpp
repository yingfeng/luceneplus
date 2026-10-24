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

module lucene_core:upgrade_index_merge_policy.impl;

import :upgrade_index_merge_policy;

import std;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;
import :types;
import :version;

namespace lucene {

UpgradeIndexMergePolicy::UpgradeIndexMergePolicy(std::unique_ptr<MergePolicy> delegate)
    : delegate_(std::move(delegate)) {}

bool UpgradeIndexMergePolicy::should_upgrade_segment(const SegmentCommitInfo &info) {
    return !(info.info.version.has_value() && info.info.version.value() == LUCENE_CURRENT_VERSION);
}

Result<std::optional<MergeSpecification>>
UpgradeIndexMergePolicy::find_merges(MergeTrigger merge_trigger, const SegmentInfos &infos,
                                    const MergeContext &context) const {
    return delegate_->find_merges(merge_trigger, infos, context);
}

Result<std::optional<MergeSpecification>>
UpgradeIndexMergePolicy::find_forced_merges(const SegmentInfos &infos, int32_t max_num_segments,
                                            const MergeContext &context) const {
    // Everything the delegate wants to merge stays as it is; this policy only
    // adds the segments that still need an upgrade and that the delegate left
    // alone.
    auto spec = delegate_->find_forced_merges(infos, max_num_segments, context);
    if (!spec.has_value()) {
        return std::unexpected(std::move(spec.error()));
    }

    std::set<std::string> old_segments;
    for (const SegmentCommitInfo &info : infos.segments) {
        if (should_upgrade_segment(info)) {
            old_segments.insert(info.info.name);
        }
    }
    if (old_segments.empty()) {
        return spec;
    }

    if (spec.value().has_value()) {
        for (const OneMerge &merge : spec.value()->merges()) {
            for (const SegmentCommitInfo &segment : merge.segments()) {
                old_segments.erase(segment.info.name);
            }
        }
    }
    if (old_segments.empty()) {
        return spec;
    }

    std::vector<SegmentCommitInfo> to_upgrade;
    to_upgrade.reserve(old_segments.size());
    for (const SegmentCommitInfo &info : infos.segments) {
        if (old_segments.find(info.info.name) != old_segments.end()) {
            to_upgrade.push_back(info);
        }
    }
    if (to_upgrade.empty()) {
        return spec;
    }

    MergeSpecification merged;
    if (spec.value().has_value()) {
        for (const OneMerge &merge : spec.value()->merges()) {
            merged.add(merge);
        }
    }
    merged.add(OneMerge(std::move(to_upgrade)));
    return std::optional<MergeSpecification>(std::move(merged));
}

Result<std::optional<MergeSpecification>>
UpgradeIndexMergePolicy::find_forced_deletes_merges(const SegmentInfos &infos, const MergeContext &context) const {
    return delegate_->find_forced_deletes_merges(infos, context);
}

bool UpgradeIndexMergePolicy::use_compound_file(const SegmentInfos &infos, const SegmentCommitInfo &merged_info,
                                               const MergeContext &context) const {
    return delegate_->use_compound_file(infos, merged_info, context);
}

int64_t UpgradeIndexMergePolicy::max_full_flush_merge_size() const {
    return delegate_->max_full_flush_merge_size();
}

} // namespace lucene
