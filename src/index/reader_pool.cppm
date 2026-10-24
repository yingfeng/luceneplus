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

export module lucene_core:reader_pool;

import :types;
import std;

import :directory;
import :error;
import :io_context;
import :segment_commit_info;
import :segment_infos;
import :segment_reader;

namespace lucene {

/// Keeps the [`SegmentReader`]s of a writer alive, so that consecutive near-real-time
/// reads do not reopen every segment.
///
/// Port of `ReaderPool` (`index/reader_pool.rs`), reduced to the part this port
/// needs: a cache of `SegmentReader`s keyed by segment name, which reopens a
/// cached reader when its segment moved on.
///
/// **Deviations** (they cost speed or bookkeeping, never correctness):
///  * no reference counting and no `ReadersAndUpdates`: a reader handed out is
///    shared, and the pool reopens a segment when its `delGen` or `maxDoc`
///    changed instead of merging live docs in memory;
///  * no soft deletes and no doc-values updates, which the writer does not have.
export class ReaderPool final {
public:
    /// A pool that opens readers from `directory`.
    explicit ReaderPool(Directory &directory) : directory_(&directory) {}

    ~ReaderPool() = default;

    /// A reader for `info`, reusing the cached one when it is still current.
    ///
    /// A cached reader is stale when the segment's `delGen` or `maxDoc` changed,
    /// e.g. after a commit that applied deletions; it is dropped and reopened.
    [[nodiscard]] Result<std::shared_ptr<SegmentReader>> get(SegmentCommitInfo &info, int32_t created_version_major,
                                                             const IOContext &context);

    /// The cached reader of the segment called `segment_name`, or null.
    [[nodiscard]] std::shared_ptr<SegmentReader> find(std::string_view segment_name) const;

    /// Whether a reader for `segment_name` is cached.
    [[nodiscard]] bool is_cached(std::string_view segment_name) const { return find(segment_name) != nullptr; }

    /// Forgets the reader of `info` (e.g. after a merge dropped the segment).
    void drop(const SegmentCommitInfo &info);
    /// Forgets every reader.
    void drop_all();

    /// Forgets the readers of segments `infos` does not list.
    ///
    /// A commit that applies deletions (or a merge) replaces a segment with one
    /// that has a **new name**, so a cache keyed by name would otherwise keep the
    /// reader of a segment that no longer exists.
    void retain_only(const SegmentInfos &infos);

    /// Closes and forgets every cached reader; idempotent.
    [[nodiscard]] VoidResult close();

    [[nodiscard]] size_t num_cached() const noexcept { return readers_.size(); }
    [[nodiscard]] bool is_closed() const noexcept { return closed_; }

private:
    Directory *directory_;
    std::map<std::string, std::shared_ptr<SegmentReader>, std::less<>> readers_;
    bool closed_{false};
};

} // namespace lucene
