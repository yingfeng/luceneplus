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

module lucene_core:leaf_metadata.impl;

import :leaf_metadata;

import std;

import :error;
import :types;
import :version;

namespace lucene {

Result<LeafMetaData> LeafMetaData::create(int32_t created_version_major, std::optional<Version> min_version,
                                          std::shared_ptr<const Sort> sort, bool has_blocks) {
    if (created_version_major > LUCENE_CURRENT_VERSION.major) {
        return err(LuceneError::illegal_argument("created_version_major is in the future: " +
                                                 std::to_string(created_version_major)));
    }
    if (created_version_major < 6) {
        return err(LuceneError::illegal_argument("created_version_major must be >= 6, got: " +
                                                 std::to_string(created_version_major)));
    }
    if (created_version_major >= 7 && !min_version.has_value()) {
        return err(LuceneError::illegal_argument("min_version must be set when created_version_major is >= 7"));
    }
    return LeafMetaData(created_version_major, std::move(min_version), std::move(sort), has_blocks);
}

const LeafMetaData &LeafMetaData::unknown() {
    static const LeafMetaData fallback(LUCENE_MIN_SUPPORTED_VERSION.major, LUCENE_MIN_SUPPORTED_VERSION,
                                       std::shared_ptr<const Sort>(), false);
    return fallback;
}

std::string LeafMetaData::to_string() const {
    std::string result = "LeafMetaData(created_version_major=" + std::to_string(created_version_major_);
    if (min_version_.has_value()) {
        result += ", min_version=" + min_version_->to_string();
    }
    if (sort_ != nullptr) {
        result += ", sort=set";
    }
    if (has_blocks_) {
        result += ", has_blocks";
    }
    result += ")";
    return result;
}

} // namespace lucene
