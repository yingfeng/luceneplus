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

export module lucene_core:one_merge_wrapping_merge_policy;

import :types;
import std;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;

namespace lucene {

/// A [`MergePolicy`] that rewrites every [`OneMerge`] its delegate returns.
///
/// Port of `OneMergeWrappingMergePolicy` (`index/one_merge_wrapping_merge_policy.rs`).
///
/// This is the hook the reference implementation uses for its soft-delete
/// retention and point-in-time policies: the delegate decides *which* segments
/// to merge, the operator decides *what* the merge is (which documents it keeps,
/// for instance).  Here the operator is a plain `std::function` instead of the
/// reference implementation's enum of operators, because [`OneMerge`] is a value type in this
/// port.
///
/// **Experimental**, like the reference implementation: the API may change.
///
/// **Deviation:** [`MergePolicy::set_verbose`] and
/// [`MergePolicy::set_no_cfs_ratio`] only affect this wrapper; configure the
/// delegate through [`delegate`] instead, which is where its own state lives.
export class OneMergeWrappingMergePolicy final : public MergePolicy {
public:
    /// Turns the merge the delegate asked for into the merge to run.
    using OneMergeOperator = std::function<Result<OneMerge>(OneMerge)>;

    /// Takes ownership of `delegate`.
    OneMergeWrappingMergePolicy(std::unique_ptr<MergePolicy> delegate, OneMergeOperator wrap_one_merge);

    [[nodiscard]] std::string to_string() const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_merges(MergeTrigger merge_trigger, const SegmentInfos &infos,
                const MergeContext &context) const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_forced_merges(const SegmentInfos &infos, int32_t max_num_segments,
                       const MergeContext &context) const override;

    [[nodiscard]] Result<std::optional<MergeSpecification>>
    find_forced_deletes_merges(const SegmentInfos &infos, const MergeContext &context) const override;

    [[nodiscard]] bool use_compound_file(const SegmentInfos &infos, const SegmentCommitInfo &merged_info,
                                        const MergeContext &context) const override;

    [[nodiscard]] int64_t max_full_flush_merge_size() const override;

    // ---- introspection ----

    [[nodiscard]] MergePolicy &delegate() noexcept { return *delegate_; }
    [[nodiscard]] const MergePolicy &delegate() const noexcept { return *delegate_; }

private:
    /// Applies the operator to every merge of `spec`.
    [[nodiscard]] Result<std::optional<MergeSpecification>>
    wrap_spec(Result<std::optional<MergeSpecification>> spec) const;

    std::unique_ptr<MergePolicy> delegate_;
    OneMergeOperator wrap_one_merge_;
};

} // namespace lucene
