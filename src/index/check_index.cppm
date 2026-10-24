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

export module lucene_core:check_index;

import :types;
import std;

import :directory;
import :error;
import :index_commit;
import :segment_infos;

namespace lucene {

/// What [`CheckIndex`] found in one segment.
export class SegmentCheckResult {
public:
    /// Segment name, e.g. `_0`.
    std::string segment_name;
    int32_t max_doc{0};
    /// Live documents, i.e. `max_doc - delCount`.
    int32_t num_docs{0};
    /// Terms the postings check walked.
    int64_t term_count{0};
    /// Document entries the postings check enumerated.
    int64_t posting_count{0};
    /// What is wrong with this segment; empty means it is healthy.
    std::vector<std::string> failures;

    [[nodiscard]] bool clean() const noexcept { return failures.empty(); }
    [[nodiscard]] std::string to_string() const;
};

/// What [`CheckIndex`] found in an index.
///
/// Port of `Status` (`index/check_index.rs`), reduced to the checks this port can
/// run — see [`CheckIndex`].
export class CheckIndexResult {
public:
    /// `true` when the directory holds no `segments_N` at all.
    bool missing_segments_file{false};
    /// Generation of the checked commit.
    int64_t generation{0};
    std::string segments_file_name;
    /// Problems that are not tied to one segment.
    std::vector<std::string> failures;
    std::vector<SegmentCheckResult> segments;

    [[nodiscard]] bool clean() const noexcept;
    /// Live documents over all healthy segments.
    [[nodiscard]] int64_t total_docs() const noexcept;
    [[nodiscard]] std::string to_string() const;
};

/// Checks that an index on disk is consistent.
///
/// Port of `CheckIndex` (`index/check_index.rs`), reduced to the checks that do
/// not need the write path:
///
///  1. the newest commit (or a given commit) can be read;
///  2. every segment opens as a [`SegmentReader`](:segment_reader);
///  3. the codec readers report no corruption
///     ([`CodecReader::check_integrity`](:codec_reader));
///  4. `numDocs` matches `maxDoc - delCount`;
///  5. the live docs have the right length and delete count;
///  6. every field that claims norms or doc values can actually hand them out;
///  7. every indexed field walks its term dictionary and lets every term's
///     postings be enumerated: terms strictly increasing, doc ids increasing and
///     in range, `docFreq` and `totalTermFreq` matching what the postings report,
///     frequencies, positions and offsets monotonic, and the field's `size`,
///     `docCount`, `sumDocFreq` and `sumTotalTermFreq` matching the walk;
///  8. when a field stores term vectors, every term of every live document's
///     vector is looked up in the dictionary and its frequency compared with the
///     postings;
///  9. `Terms::intersect` agrees with a linear scan for the probe automata the
///     reference implementation uses (any term containing `a`..`e`, and every
///     non-empty term, each with several start terms);
/// 10. the stored fields of the first live document of a segment can be read.
///
/// **Not ported yet** (each needs machinery this port does not have): the points
/// space verification (only the presence of a point field is checked), the vector
/// checks, the index-sort check, the soft-deletes check, and the resurrection of
/// segments after a failed check.  Term-vector validation compares frequencies
/// only (the reference implementation also checks positions, offsets and payloads).  `CheckIndex` therefore reports
/// *fewer* checks than the reference implementation, never more.
///
/// This reads whole files, so it is for tools and tests, not for the hot path.
export class CheckIndex {
public:
    explicit CheckIndex(Directory &directory) : directory_(&directory) {}

    /// Checks the newest commit in the directory.
    ///
    /// A directory without any `segments_N` is not an error: the result then has
    /// `missing_segments_file` set.
    [[nodiscard]] Result<CheckIndexResult> check();

    /// Checks the commit `commit` stands for.
    [[nodiscard]] Result<CheckIndexResult> check(const IndexCommit &commit);

    /// The directory being checked.
    [[nodiscard]] Directory &directory() const noexcept { return *directory_; }

private:
    /// Checks every segment of `infos`.
    [[nodiscard]] Result<CheckIndexResult> check_infos(const SegmentInfos &infos);

    Directory *directory_;
};

} // namespace lucene
