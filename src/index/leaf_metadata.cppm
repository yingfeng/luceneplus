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

export module lucene_core:leaf_metadata;

import :types;
import std;

import :error;
import :version;

namespace lucene {

/// Forward declaration only: `:sort_field` depends on [`LeafReader`](:leaf_reader),
/// so this module may not depend on it.  A metadata never constructs a `Sort`, it
/// only carries one.
export class Sort;

/// How a leaf was written: the versions that created it, whether it is index
/// sorted, and whether it holds document blocks.
///
/// Port of `LeafMetaData` (`index/leaf_metadata.rs`).
///
/// **Deviation:** the port cannot index-sort yet (`IndexWriterConfig` has no
/// `index_sort`), so [`sort`] is always null; the accessor is here so that the
/// readers and the queries that ask for it do not have to change when index
/// sorting arrives.
export class LeafMetaData {
public:
    /// Builds a metadata, validating the version constraints.
    ///
    /// Fails with `illegal_argument` when `created_version_major` is in the
    /// future, is below 6, or when it is 7 or later without a `min_version`.
    [[nodiscard]] static Result<LeafMetaData> create(int32_t created_version_major,
                                                     std::optional<Version> min_version,
                                                     std::shared_ptr<const Sort> sort, bool has_blocks);

    /// The metadata of a reader that does not track one.
    ///
    /// It reports the oldest version this port can read and no index sort, so a
    /// caller that only uses the version fields gets a sane answer instead of an
    /// error.
    [[nodiscard]] static const LeafMetaData &unknown();

    /// The major version of the Lucene format that created the segment.
    [[nodiscard]] int32_t created_version_major() const noexcept { return created_version_major_; }
    /// The oldest version that contributed a document.
    [[nodiscard]] const std::optional<Version> &min_version() const noexcept { return min_version_; }
    /// The sort order of the segment, or null (this port never sorts at index
    /// time yet).
    [[nodiscard]] const std::shared_ptr<const Sort> &sort() const noexcept { return sort_; }
    /// Whether the segment holds documents written as blocks.
    [[nodiscard]] bool has_blocks() const noexcept { return has_blocks_; }

    [[nodiscard]] std::string to_string() const;

private:
    LeafMetaData(int32_t created_version_major, std::optional<Version> min_version,
                 std::shared_ptr<const Sort> sort, bool has_blocks)
        : created_version_major_(created_version_major),
          min_version_(std::move(min_version)),
          sort_(std::move(sort)),
          has_blocks_(has_blocks) {}

    int32_t created_version_major_;
    std::optional<Version> min_version_;
    std::shared_ptr<const Sort> sort_;
    bool has_blocks_;
};

} // namespace lucene
