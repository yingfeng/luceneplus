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

export module lucene_core:upgrade_index_merge_policy;

import :types;
import std;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;

namespace lucene {

/// A [`MergePolicy`] that only rewrites segments written by an older version.
///
/// Port of `UpgradeIndexMergePolicy` (`index/upgrade_index_merge_policy.rs`).
///
/// Wrap any policy in this one and call `force_merge(1)`: instead of merging
/// everything, the forced merge only rewrites the segments that still carry an
/// older format, which is the cheap way to upgrade an index.  Everything else
/// (regular merges, delete-expunging merges, compound file decisions) is
/// delegated unchanged.
///
/// **Warning:** like the reference implementation, a partially upgraded index
/// may have its documents reordered by the merge.
///
/// **Deviation:** the reference implementation's `find_forced_merges` receives the
/// set of segments the caller allowed to merge and filters the old segments
/// against it; this port's [`MergePolicy::find_forced_merges`] has no such set
/// (the writer has no `force_merge` snapshot), so every old segment is a
/// candidate and the delegate is asked about all segments.
export class UpgradeIndexMergePolicy final : public MergePolicy {
public:
    /// Takes ownership of `delegate` and wraps it.
    explicit UpgradeIndexMergePolicy(std::unique_ptr<MergePolicy> delegate);

    /// `true` when `info` was written by a different version than
    /// [`LUCENE_CURRENT_VERSION`], i.e. when it needs an upgrade.
    [[nodiscard]] static bool should_upgrade_segment(const SegmentCommitInfo &info);

    [[nodiscard]] std::string to_string() const override { return "UpgradeIndexMergePolicy"; }

    /// Delegated unchanged: only *forced* merges are intercepted.
    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_merges(MergeTrigger merge_trigger, const SegmentInfos &infos,
                const MergeContext &context) const override;

    /// Asks the delegate for its forced merges, then adds one merge over the old
    /// segments the delegate did not claim.
    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_forced_merges(const SegmentInfos &infos, int32_t max_num_segments,
                       const MergeContext &context) const override;

    /// Delegated unchanged.
    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_forced_deletes_merges(const SegmentInfos &infos, const MergeContext &context) const override;

    /// Delegated unchanged.
    [[nodiscard]] bool use_compound_file(const SegmentInfos &infos, const SegmentCommitInfo &merged_info,
                                        const MergeContext &context) const override;

    /// Delegated unchanged.
    [[nodiscard]] int64_t max_full_flush_merge_size() const override;

    // ---- introspection ----

    [[nodiscard]] MergePolicy &delegate() noexcept { return *delegate_; }
    [[nodiscard]] const MergePolicy &delegate() const noexcept { return *delegate_; }

private:
    std::unique_ptr<MergePolicy> delegate_;
};

} // namespace lucene
