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

export module lucene_core:standard_directory_reader;

import :types;
import std;

import :base_composite_reader;
import :directory;
import :error;
import :identity;
import :index_commit;
import :index_reader;
import :io_context;
import :leaf_reader;
import :leaf_reader_context;
import :segment_infos;
import :segment_reader;
import :stored_fields;
import :term;
import :term_vectors;

namespace lucene {

/// A composite reader over the segments of one commit.
///
/// Port of `StandardDirectoryReader` (`index/standard_directory_reader.rs`),
/// minus the writer-ownership half: this port has no `IndexWriter` yet, so a
/// reader is always opened from a committed `segments_N` rather than from a
/// writer's in-memory state.
///
/// **Deviations**:
///  * NRT and `open_if_changed` against a writer are not ported; [`open_if_changed`]
///    here re-reads `segments_N` from the directory and returns null when the
///    generation did not move;
///  * sub-readers are not sorted (`MultiReader`'s `sub_reader_sorter`), which
///    the reference implementation only needs when the writer hands over its own
///    segments;
///  * the cache helper / closed-listener protocol is not ported.
export class DirectoryReader final : public IndexReader {
public:
    ~DirectoryReader() override = default;

    /// Opens the newest commit in `dir`.
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>>
    open(Directory &dir, const IOContext &context = io_context_default());

    /// Opens the segments listed by `infos`; `infos` is kept for
    /// [`segment_infos`] and [`is_current`].
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>>
    open_infos(Directory &dir, SegmentInfos infos, const IOContext &context = io_context_default());

    /// Opens a reader over `segment_readers`, which must describe the segments of
    /// `infos` in the same order.
    ///
    /// This is how a writer hands out a near-real-time reader: the readers come
    /// from its [`ReaderPool`](:reader_pool) instead of being opened here.
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>>
    open_with_readers(Directory &dir, SegmentInfos infos,
                      std::vector<std::shared_ptr<SegmentReader>> segment_readers);

    /// Compares two leaves, so [`open_with_sorter`](Self::open_with_sorter) can
    /// order the segments of an index.
    ///
    /// Port of `LeafSorter`: negative when `a` sorts first, zero when they are
    /// equal.  It may fail, which aborts the open.
    using LeafSorter = std::function<Result<int32_t>(const LeafReader &a, const LeafReader &b)>;

    /// Opens the newest commit in `dir` with its segments ordered by `sorter`.
    ///
    /// Port of `DirectoryReader::open_with_sorter`: an index whose segments are
    /// ordered by the sort a caller queries with can stop early while it walks
    /// the segments, so ordering the leaves is worth it for time based indices.
    /// The document order of the reader follows the sorter, not the commit.
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>>
    open_with_sorter(Directory &dir, const LeafSorter &sorter, const IOContext &context = io_context_default());

    /// Opens a reader over `leaf_readers`, which describe the leaves of `infos` in
    /// document order.
    ///
    /// The count may differ from `infos.size()`, because a wrapper is allowed to
    /// drop leaves; `infos` is kept for [`segment_infos`](Self::segment_infos) and
    /// [`is_current`](Self::is_current) only.
    ///
    /// This is what a reader wrapper uses: the leaves of another reader, possibly
    /// wrapped again (soft deletes keep a "hard" leaf under a filtered one).
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>>
    open_with_leaves(Directory &dir, SegmentInfos infos, std::vector<std::shared_ptr<LeafReader>> leaf_readers);

    /// Every commit in `dir`, oldest first.
    ///
    /// Port of `DirectoryReader::list_commits`.  The commits are read-only views
    /// of the `segments_N` files; [`IndexCommit::delete_commit`] fails on them
    /// with `unsupported_operation`, because only a writer deletes commits.
    [[nodiscard]] static Result<std::vector<std::shared_ptr<IndexCommit>>> list_commits(Directory &dir);

    /// Whether `dir` holds an index, i.e. at least one `segments_N` file.
    ///
    /// Port of `DirectoryReader::index_exists`, which errs towards `true`: a
    /// corrupt or half-written commit still counts as an index, so a caller does
    /// not mistake it for an empty directory and overwrite it.
    [[nodiscard]] static Result<bool> index_exists(Directory &dir);

    /// Opens the index as `commit` recorded it.
    ///
    /// Port of `DirectoryReader::open_from_commit`: it reads the commit's
    /// `segments_N` again, so a caller can read an older point in time (a
    /// snapshot, a backup) rather than the newest one.
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>> open_commit(const IndexCommit &commit);

    /// Opens `commit`, requiring the index to have been created with at least
    /// `min_supported_major_version`.
    ///
    /// Port of `DirectoryReader::open_with_version`.
    [[nodiscard]] static Result<std::shared_ptr<DirectoryReader>>
    open_with_version(const IndexCommit &commit, int32_t min_supported_major_version);

    /// Reopens the index when `dir` has a newer commit; null otherwise.
    [[nodiscard]] Result<std::shared_ptr<DirectoryReader>> open_if_changed() const;

    /// Reopens the index as `commit` recorded it, unless this reader already is
    /// that commit; null otherwise.
    ///
    /// Port of `DirectoryReader::open_if_changed_with_commit`.
    [[nodiscard]] Result<std::shared_ptr<DirectoryReader>> open_if_changed_with_commit(const IndexCommit &commit) const;

    // ---- IndexReader ----

    [[nodiscard]] Result<int32_t> max_doc() const override { return base_.max_doc(); }
    [[nodiscard]] Result<int32_t> num_docs() const override { return base_.num_docs(); }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override;
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override;
    [[nodiscard]] Result<int32_t> doc_freq(const Term &term) override { return base_.doc_freq(term); }
    [[nodiscard]] Result<int64_t> total_term_freq(const Term &term) override { return base_.total_term_freq(term); }
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view field) override { return base_.sum_doc_freq(field); }
    [[nodiscard]] Result<int32_t> doc_count(std::string_view field) override { return base_.doc_count(field); }
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view field) override {
        return base_.sum_total_term_freq(field);
    }
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override;
    [[nodiscard]] const Identity &identity() const override { return identity_; }

    // ---- commit accessors ----

    /// The directory the commit lives in.
    [[nodiscard]] Directory &directory() const {
        return *dir_;
    }

    /// The commit this reader was opened from.
    [[nodiscard]] const SegmentInfos &segment_infos() const noexcept { return infos_; }
    /// Index version of the commit (bumped by every commit).
    [[nodiscard]] int64_t version() const noexcept { return infos_.version; }
    /// The leaves, in document order; these are the readers
    /// [`leaves`](Self::leaves) hands out, kept alive by this reader.
    [[nodiscard]] const std::vector<std::shared_ptr<LeafReader>> &leaf_readers() const noexcept {
        return leaf_readers_;
    }

    /// The segment readers, in document order.
    ///
    /// Empty when the reader was opened over generic leaves (see
    /// [`open_with_leaves`](Self::open_with_leaves)): a wrapped leaf is not a
    /// [`SegmentReader`](:segment_reader).
    [[nodiscard]] const std::vector<std::shared_ptr<SegmentReader>> &segment_readers() const noexcept {
        return segment_readers_;
    }
    /// `true` when `dir` still holds the commit this reader was opened from.
    [[nodiscard]] Result<bool> is_current() const;

private:
    DirectoryReader() = default;

    Directory *dir_{nullptr};
    SegmentInfos infos_;
    std::vector<std::shared_ptr<SegmentReader>> segment_readers_;
    /// Every leaf, wrapped or not, in document order.
    std::vector<std::shared_ptr<LeafReader>> leaf_readers_;
    CompositeReaderBase base_;
    mutable std::vector<LeafReaderContext> leaves_;
    Identity identity_{};
};

} // namespace lucene
