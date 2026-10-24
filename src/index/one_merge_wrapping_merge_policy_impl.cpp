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

module lucene_core:one_merge_wrapping_merge_policy.impl;

import :one_merge_wrapping_merge_policy;

import std;

import :error;
import :merge_policy;
import :merge_trigger;
import :segment_commit_info;
import :segment_infos;
import :types;

namespace lucene {

OneMergeWrappingMergePolicy::OneMergeWrappingMergePolicy(std::unique_ptr<MergePolicy> delegate,
                                                         OneMergeOperator wrap_one_merge)
    : delegate_(std::move(delegate)), wrap_one_merge_(std::move(wrap_one_merge)) {}

Result<std::optional<MergeSpecification>>
OneMergeWrappingMergePolicy::wrap_spec(Result<std::optional<MergeSpecification>> spec) const {
    if (!spec.has_value()) {
        return std::unexpected(std::move(spec.error()));
    }
    if (!spec.value().has_value()) {
        return std::optional<MergeSpecification>{};
    }
    MergeSpecification wrapped;
    for (const OneMerge &merge : spec.value()->merges()) {
        auto replacement = wrap_one_merge_(merge);
        if (!replacement.has_value()) {
            return std::unexpected(std::move(replacement.error()));
        }
        wrapped.add(std::move(replacement.value()));
    }
    return std::optional<MergeSpecification>(std::move(wrapped));
}

Result<std::optional<MergeSpecification>>
OneMergeWrappingMergePolicy::find_merges(MergeTrigger merge_trigger, const SegmentInfos &infos,
                                        const MergeContext &context) const {
    return wrap_spec(delegate_->find_merges(merge_trigger, infos, context));
}

Result<std::optional<MergeSpecification>>
OneMergeWrappingMergePolicy::find_forced_merges(const SegmentInfos &infos, int32_t max_num_segments,
                                               const MergeContext &context) const {
    return wrap_spec(delegate_->find_forced_merges(infos, max_num_segments, context));
}

Result<std::optional<MergeSpecification>>
OneMergeWrappingMergePolicy::find_forced_deletes_merges(const SegmentInfos &infos,
                                                       const MergeContext &context) const {
    return wrap_spec(delegate_->find_forced_deletes_merges(infos, context));
}

bool OneMergeWrappingMergePolicy::use_compound_file(const SegmentInfos &infos, const SegmentCommitInfo &merged_info,
                                                   const MergeContext &context) const {
    return delegate_->use_compound_file(infos, merged_info, context);
}

int64_t OneMergeWrappingMergePolicy::max_full_flush_merge_size() const {
    return delegate_->max_full_flush_merge_size();
}

std::string OneMergeWrappingMergePolicy::to_string() const {
    return "OneMergeWrappingMergePolicy(" + delegate_->to_string() + ")";
}

} // namespace lucene
