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

export module lucene_core:index_writer;

import :types;
import std;

import :directory;
import :document;
import :error;
import :index_writer_config;
import :segment_infos;
import :standard_directory_reader;
import :term;

namespace lucene {

/// Creates and updates an index.
///
/// Port of `IndexWriter` (`index/index_writer.rs`), the first working version.
/// It wires together everything the write path needs: a
/// [`DocumentsWriter`](:documents_writer) to buffer documents, the per-thread
/// writer and the indexing chain to turn them into segment files, and
/// [`SegmentInfos`] to commit the result so readers can open it.
///
/// Usage:
/// ```text
/// auto writer = IndexWriter::open(directory, config);
/// writer.value()->add_document(doc);
/// writer.value()->commit();
/// ```
///
/// **Deviations** (scope, see the project note):
///  * one thread writes at a time: no lock manager, no thread states, no
///    concurrent flush, and `add_document` is not re-entrant;
///  * merges run synchronously inside [`commit`] and [`force_merge`]: there is
///    no merge scheduler and no merge thread, so a merge blocks the caller;
///  * deletes are applied at commit time by rewriting the `.liv` of every
///    segment the term matches (the reference implementation applies them incrementally to the DWPTs
///    through its delete queue), and `delete_all` drops the segments;
///  * no `updateDocValues`, no soft deletes, no commit data, no listener, and
///    `rollback` simply removes the segments written since the last commit;
///  * [`get_reader`] flushes the buffered documents and applies the buffered
///    deletes before it opens its readers, and it reopens every segment instead of
///    reusing pooled readers (the reference implementation's `ReaderPool`), so it
///    is correct but not cheap.
export class IndexWriter {
public:
    /// Opens (or creates) the index in `directory`.
    [[nodiscard]] static Result<std::unique_ptr<IndexWriter>> open(Directory &directory,
                                                                   const IndexWriterConfig &config);

    IndexWriter(const IndexWriter &) = delete;
    IndexWriter &operator=(const IndexWriter &) = delete;
    IndexWriter(IndexWriter &&) noexcept = delete;
    IndexWriter &operator=(IndexWriter &&) noexcept = delete;
    ~IndexWriter();

    /// Adds a document; the buffer is flushed automatically when it is full.
    [[nodiscard]] VoidResult add_document(const Document &document);

    /// Deletes every document matching `term`, then adds `document`.
    [[nodiscard]] VoidResult update_document(const Term &term, const Document &document);

    /// Deletes every document matching `term`, once the index is committed.
    [[nodiscard]] VoidResult delete_documents(const Term &term);

    /// Deletes every document of the index.
    [[nodiscard]] VoidResult delete_all();

    /// Writes the buffered documents into new segments (no commit yet).
    [[nodiscard]] VoidResult flush();

    /// Flushes, applies the buffered deletes and writes the next `segments_N`.
    [[nodiscard]] VoidResult commit();

    /// Merges the index down to at most `max_num_segments` segments.
    ///
    /// The merge starts once the buffered documents are flushed and finishes
    /// before this call returns, like `forceMerge(1)` in the reference
    /// implementation when there is no merge scheduler.
    [[nodiscard]] VoidResult force_merge(int32_t max_num_segments);

    /// Rewrites the segments that have deletes, to expunge them.
    [[nodiscard]] VoidResult force_merge_deletes();

    /// Drops the documents added since the last commit and restores the last
    /// committed state of the index.
    [[nodiscard]] VoidResult rollback();

    /// [`commit`] followed by releasing the writer.
    [[nodiscard]] VoidResult close();

    /// A reader over the index as it is right now, without committing.
    ///
    /// Port of `IndexWriter::get_reader`, the near-real-time read: the buffered
    /// documents are flushed, the buffered deletes are applied, and the segments
    /// the writer currently holds are opened.  The result is a snapshot: writes
    /// that happen afterwards are not visible through it.
    [[nodiscard]] Result<std::shared_ptr<DirectoryReader>> get_reader();

    /// [`get_reader`](Self::get_reader) with the expert switches of the reference
    /// implementation's `open_with_writer_deletes`.
    ///
    /// `apply_all_deletes` decides whether the buffered deletes are applied (and
    /// written to the `.liv` files) before the reader opens; with `false` the
    /// reader still shows the documents a pending delete targets.  **Deviation:**
    /// `write_all_deletes` is accepted for compatibility but the port always
    /// writes the live docs when it applies deletes, because it never carries
    /// them in heap.
    [[nodiscard]] Result<std::shared_ptr<DirectoryReader>> get_reader(bool apply_all_deletes,
                                                                     bool write_all_deletes);

    /// Documents visible in the index: committed ones plus the buffered ones,
    /// minus the deletes that are still only buffered.
    [[nodiscard]] Result<int64_t> num_docs() const;

    /// Sum of the `max_doc` of every segment, buffered documents included.
    [[nodiscard]] Result<int64_t> max_doc() const;

    /// Whether a committed segment has deletions.
    [[nodiscard]] Result<bool> has_deletions() const;

    /// The commit state of the index as the writer currently sees it.
    [[nodiscard]] const SegmentInfos &segment_infos() const;

    [[nodiscard]] bool is_open() const noexcept;

private:
    explicit IndexWriter(Directory &directory, IndexWriterConfig config);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lucene
