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

module lucene_core:index_writer.impl;

import :index_writer;

import :bits;
import :codec;
import :directory;
import :documents_writer;
import :document;
import :error;
import :field_infos;
import :fixed_bit_set;
import :index_reader;
import :index_searcher;
import :index_writer_config;
import :io_context;
import :leaf_reader;
import :live_docs_format;
import :merge_policy;
import :merge_scheduler;
import :merge_trigger;
import :reader_pool;
import :segment_commit_info;
import :segment_infos;
import :standard_directory_reader;
import :segment_merger;
import :segment_reader;
import :sort_field;
import :string_helper;
import :term;
import :term_query;
import :top_docs;
import :types;
import :version;

import std;

namespace lucene {

namespace {

/// A file belongs to `segment` when it is `<segment>.<ext>` or
/// `<segment>_<something>.<ext>`.
[[nodiscard]] bool is_segment_file(std::string_view file, std::string_view segment) {
    if (file.size() <= segment.size() || file.substr(0, segment.size()) != segment) {
        return false;
    }
    const char separator = file[segment.size()];
    return separator == '.' || separator == '_';
}

/// Files a segment owns, so a rolled back writer can delete them.
std::set<std::string> files_of(const SegmentInfos &infos) {
    std::set<std::string> files;
    for (const SegmentCommitInfo &segment : infos.segments) {
        files.insert(segment.info.files.begin(), segment.info.files.end());
    }
    return files;
}

} // namespace

struct IndexWriter::Impl : MergeSource {
    Impl(Directory &directory, IndexWriterConfig config)
        : directory_(directory),
          reader_pool_(std::make_unique<ReaderPool>(directory)),
          config_(std::move(config)) {}

    /// The merge context the policy sees: deletes come from the commit infos, the
    /// segments being merged are the queued ones, and sizes are measured against
    /// the writer's directory.
    class WriterMergeContext final : public MergeContext {
    public:
        explicit WriterMergeContext(const Impl &writer) : writer_(writer) {}

        [[nodiscard]] Result<int32_t> num_deletes_to_merge(const SegmentCommitInfo &info) const override {
            return static_cast<int32_t>(info.total_del_count());
        }

        [[nodiscard]] Result<int32_t> num_deleted_docs(const SegmentCommitInfo &info) const override {
            return static_cast<int32_t>(info.total_del_count());
        }

        [[nodiscard]] InfoStreamMT get_info_stream() const override { return get_default_info_stream(); }

        [[nodiscard]] std::set<std::string> get_merging_segments() const override {
            std::set<std::string> names;
            for (const OneMerge &one_merge : writer_.pending_merges_) {
                for (const SegmentCommitInfo &segment : one_merge.segments()) {
                    names.insert(segment.info.name);
                }
            }
            return names;
        }

        [[nodiscard]] Directory *directory() const override { return &writer_.directory_; }

    private:
        const Impl &writer_;
    };

    [[nodiscard]] VoidResult drain_flushed_segments() {
        for (SegmentCommitInfo &segment : documents_writer_->take_flushed_segments()) {
            if (auto added = infos_.add(std::move(segment)); !added.has_value()) {
                return added;
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult flush() {
        auto flushed = documents_writer_->flush_all();
        if (!flushed.has_value()) {
            return std::unexpected(std::move(flushed.error()));
        }
        for (SegmentCommitInfo &segment : flushed.value()) {
            if (auto added = infos_.add(std::move(segment)); !added.has_value()) {
                return added;
            }
        }
        return {};
    }

    /// Marks every document matching a buffered delete as deleted.
    ///
    /// The reference implementation applies the deletes of the delete queue to each DWPT as they
    /// arrive; this port rewrites the live docs of the affected segments when
    /// the index is committed.
    [[nodiscard]] VoidResult apply_deletes() {
        const std::vector<std::pair<Term, int64_t>> &terms = documents_writer_->buffered_delete_terms();
        const bool delete_all = documents_writer_->delete_all_pending();
        if (terms.empty() && !delete_all) {
            return {};
        }
        int64_t doc_base = 0;
        for (SegmentCommitInfo &segment : infos_.segments) {
            if (auto applied = apply_deletes_to(segment, doc_base, terms, delete_all); !applied.has_value()) {
                return applied;
            }
            doc_base += segment.max_doc();
        }
        return {};
    }

    /// Total documents the index holds, buffered ones included.
    ///
    /// A delete only applies to the documents that existed when it was issued,
    /// which is how `update_document` keeps the document it adds afterwards.
    [[nodiscard]] int64_t total_docs() const {
        int64_t total = documents_writer_->pending_doc_count();
        for (const SegmentCommitInfo &segment : infos_.segments) {
            total += segment.max_doc();
        }
        return total;
    }

    [[nodiscard]] VoidResult apply_deletes_to(SegmentCommitInfo &segment, int64_t doc_base,
                                             const std::vector<std::pair<Term, int64_t>> &terms,
                                             bool delete_all) {
        const IOContext &context = io_context_default();
        FixedBitSet live_docs(static_cast<size_t>(segment.max_doc()));
        if (segment.has_deletions()) {
            auto existing = config_.codec().live_docs_format().read_live_docs(directory_, segment, context);
            if (!existing.has_value()) {
                return std::unexpected(std::move(existing.error()));
            }
            live_docs = std::move(existing.value());
        } else if (auto set = live_docs.set_with_range(0, static_cast<size_t>(segment.max_doc()));
                   !set.has_value()) {
            return set;
        }

        if (delete_all) {
            live_docs.clear_all();
        } else {
            auto reader = SegmentReader::open(directory_, segment, config_.index_created_version_major(),
                                              context);
            if (!reader.has_value()) {
                return std::unexpected(std::move(reader.error()));
            }
            std::shared_ptr<IndexReader> index_reader = reader.value();
            // The searcher only reads, so the shared implementation of the
            // configured similarity can be handed over as a mutable pointer.
            std::shared_ptr<Similarity> similarity =
                    std::const_pointer_cast<Similarity>(config_.similarity());
            IndexSearcher searcher(std::move(index_reader), std::move(similarity));
            for (const auto &[term, doc_upto] : terms) {
                TermQuery query(term);
                auto hits = searcher.search(query, segment.max_doc());
                if (!hits.has_value()) {
                    return std::unexpected(std::move(hits.error()));
                }
                for (const ScoreDoc &hit : hits.value().score_docs) {
                    if (doc_base + hit.doc >= doc_upto) {
                        // Indexed after the delete was issued.
                        continue;
                    }
                    live_docs.clear(static_cast<size_t>(hit.doc));
                }
            }
        }

        const int32_t new_del_count =
                segment.max_doc() - static_cast<int32_t>(live_docs.cardinality());
        if (new_del_count == segment.del_count) {
            return {};
        }
        if (new_del_count == 0) {
            // Nothing is deleted any more: there is nothing to rewrite.
            segment.del_count = 0;
            return {};
        }
        if (auto written = config_.codec().live_docs_format().write_live_docs(live_docs, directory_, segment,
                                                                             new_del_count, context);
            !written.has_value()) {
            return written;
        }
        segment.del_gen = segment.next_del_gen();
        segment.del_count = new_del_count;
        return {};
    }

    /// Bytes the merge reads and writes, used for the merge `IOContext`.
    [[nodiscard]] Result<int64_t> estimated_merge_bytes(const OneMerge &merge) const {
        int64_t total = 0;
        for (const SegmentCommitInfo &segment : merge.segments()) {
            for (const std::string &file : segment.info.files) {
                auto length = directory_.file_length(file);
                if (!length.has_value()) {
                    return std::unexpected(std::move(length.error()));
                }
                total += static_cast<int64_t>(length.value());
            }
        }
        return total;
    }

    /// Runs one merge and returns the segment it produced.
    ///
    /// `nullopt` means the merge produced no document, which happens when every
    /// input document is deleted: the inputs are then simply dropped.
    [[nodiscard]] Result<std::optional<SegmentCommitInfo>> do_merge(const OneMerge &merge) {
        std::vector<std::shared_ptr<SegmentReader>> readers;
        std::vector<CodecReader *> raw_readers;
        readers.reserve(merge.size());
        raw_readers.reserve(merge.size());
        for (const SegmentCommitInfo &segment : merge.segments()) {
            auto reader = SegmentReader::open(directory_, segment, infos_.index_created_version_major,
                                              io_context_default());
            if (!reader.has_value()) {
                return std::unexpected(std::move(reader.error()));
            }
            raw_readers.push_back(reader.value().get());
            readers.push_back(std::move(reader.value()));
        }
        if (raw_readers.empty()) {
            return err(LuceneError::illegal_state("a merge needs at least one segment"));
        }

        auto bytes = estimated_merge_bytes(merge);
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes.error()));
        }

        SegmentInfo merged_info;
        merged_info.name = "_" + std::to_string(documents_writer_->next_segment_counter());
        documents_writer_->set_next_segment_counter(documents_writer_->next_segment_counter() + 1);
        infos_.counter = documents_writer_->next_segment_counter();
        const auto random = StringHelper::random_id();
        merged_info.id.assign(random.begin(), random.end());
        merged_info.version = LUCENE_CURRENT_VERSION;
        merged_info.min_version = LUCENE_CURRENT_VERSION;
        merged_info.codec_name = std::string(config_.codec().name());
        merged_info.diagnostics.emplace("source", "merge");
        merged_info.diagnostics.emplace("lucene.version", LUCENE_CURRENT_VERSION.to_string());

        // A merge keeps the index sort, but only when every input is sorted by it:
        // otherwise the merged documents would not be in that order and the
        // metadata would lie.  The merger then reorders the documents itself.
        if (config_.index_sort().has_value()) {
            bool sorted_like_config = true;
            for (const SegmentCommitInfo &segment : merge.segments()) {
                if (!segment.info.sort.has_value() || !segment.info.sort.value().equals(config_.index_sort().value())) {
                    sorted_like_config = false;
                    break;
                }
            }
            if (sorted_like_config) {
                merged_info.sort = config_.index_sort().value();
            }
        }

        auto merge_context = IOContext::with_merge(MergeInfo(static_cast<int32_t>(merge.total_max_doc()), bytes.value(),
                                                             false, static_cast<int32_t>(merge.size())));
        if (!merge_context.has_value()) {
            return std::unexpected(std::move(merge_context.error()));
        }

        auto merger = SegmentMerger::create(raw_readers, merged_info, get_default_info_stream(), directory_,
                                            merge_context.value());
        if (!merger.has_value()) {
            return std::unexpected(std::move(merger.error()));
        }
        if (!merger.value()->should_merge()) {
            // There is nothing left to write; the caller drops the inputs.
            return std::optional<SegmentCommitInfo>{};
        }
        if (auto merged = merger.value()->merge(); !merged.has_value()) {
            return std::unexpected(std::move(merged.error()));
        }

        SegmentCommitInfo commit;
        commit.info = merged_info;
        // The file list goes into both `.si` and `segments_N`, so it is collected
        // before the metadata is written.
        auto names = directory_.list_all();
        if (!names.has_value()) {
            return std::unexpected(std::move(names.error()));
        }
        std::set<std::string> files;
        for (const std::string &name : names.value()) {
            if (is_segment_file(name, merged_info.name)) {
                files.insert(name);
            }
        }
        if (auto set = commit.info.set_files(std::move(files)); !set.has_value()) {
            return std::unexpected(std::move(set.error()));
        }
        if (auto written = config_.codec().segment_info_format().write(directory_, commit.info, io_context_default());
            !written.has_value()) {
            return std::unexpected(std::move(written.error()));
        }
        return std::optional<SegmentCommitInfo>(std::move(commit));
    }

    /// Replaces the input segments of `merge` with `merged`, or just drops them
    /// when the merge produced no document.
    void register_merged_segment(const OneMerge &merge, std::optional<SegmentCommitInfo> merged) {
        std::set<std::string> replaced;
        for (const SegmentCommitInfo &segment : merge.segments()) {
            replaced.insert(segment.info.name);
            orphan_files_.insert(segment.info.files.begin(), segment.info.files.end());
        }

        std::vector<SegmentCommitInfo> segments;
        segments.reserve(infos_.segments.size());
        size_t insert_at = infos_.segments.size();
        for (SegmentCommitInfo &segment : infos_.segments) {
            if (replaced.contains(segment.info.name)) {
                insert_at = std::min(insert_at, segments.size());
                continue;
            }
            segments.push_back(std::move(segment));
        }
        if (insert_at > segments.size()) {
            insert_at = segments.size();
        }
        if (merged.has_value()) {
            segments.insert(segments.begin() + static_cast<std::ptrdiff_t>(insert_at), std::move(merged.value()));
        }
        infos_.segments = std::move(segments);
    }

    // ---- MergeSource ----

    [[nodiscard]] Result<std::optional<OneMerge>> get_next_merge() override {
        if (pending_merges_.empty()) {
            return std::optional<OneMerge>{};
        }
        OneMerge next = std::move(pending_merges_.front());
        pending_merges_.erase(pending_merges_.begin());
        return std::optional<OneMerge>(std::move(next));
    }

    [[nodiscard]] Result<bool> has_pending_merges() override { return !pending_merges_.empty(); }

    [[nodiscard]] VoidResult merge(OneMerge one_merge) override {
        auto merged = do_merge(one_merge);
        if (!merged.has_value()) {
            return std::unexpected(std::move(merged.error()));
        }
        register_merged_segment(one_merge, std::move(merged.value()));
        return {};
    }

    // ---- merging ----

    /// Asks the policy for the merges that are necessary now and queues them.
    [[nodiscard]] VoidResult update_pending_merges(MergeTrigger trigger) {
        if (!pending_merges_.empty()) {
            return {};
        }
        const std::shared_ptr<MergePolicy> &policy = config_.merge_policy();
        if (policy == nullptr) {
            return {};
        }
        WriterMergeContext context(*this);
        auto spec = policy->find_merges(trigger, infos_, context);
        if (!spec.has_value()) {
            return std::unexpected(std::move(spec.error()));
        }
        if (!spec.value().has_value()) {
            return {};
        }
        for (const OneMerge &one_merge : spec.value()->merges()) {
            pending_merges_.push_back(one_merge);
        }
        return {};
    }

    /// Queues the merges a forced merge asks for.
    [[nodiscard]] VoidResult update_pending_forced_merges(int32_t max_num_segments) {
        const std::shared_ptr<MergePolicy> &policy = config_.merge_policy();
        if (policy == nullptr) {
            return {};
        }
        WriterMergeContext context(*this);
        auto spec = policy->find_forced_merges(infos_, max_num_segments, context);
        if (!spec.has_value()) {
            return std::unexpected(std::move(spec.error()));
        }
        if (!spec.value().has_value()) {
            return {};
        }
        for (const OneMerge &one_merge : spec.value()->merges()) {
            pending_merges_.push_back(one_merge);
        }
        return {};
    }

    /// Queues the merges that expunge deletes.
    [[nodiscard]] VoidResult update_pending_forced_deletes_merges() {
        const std::shared_ptr<MergePolicy> &policy = config_.merge_policy();
        if (policy == nullptr) {
            return {};
        }
        WriterMergeContext context(*this);
        auto spec = policy->find_forced_deletes_merges(infos_, context);
        if (!spec.has_value()) {
            return std::unexpected(std::move(spec.error()));
        }
        if (!spec.value().has_value()) {
            return {};
        }
        for (const OneMerge &one_merge : spec.value()->merges()) {
            pending_merges_.push_back(one_merge);
        }
        return {};
    }

    /// Hands the queued merges to the scheduler.
    [[nodiscard]] VoidResult run_scheduler(MergeTrigger trigger) {
        if (pending_merges_.empty()) {
            return {};
        }
        const std::shared_ptr<MergeScheduler> &scheduler = config_.merge_scheduler();
        if (scheduler == nullptr) {
            return {};
        }
        return scheduler->merge(*this, trigger);
    }

    /// Asks the policy for merges and runs them.  Port of `IndexWriter::maybeMerge`.
    [[nodiscard]] VoidResult maybe_merge(MergeTrigger trigger) {
        if (auto updated = update_pending_merges(trigger); !updated.has_value()) {
            return updated;
        }
        return run_scheduler(trigger);
    }

    /// Runs forced merges until the policy stops proposing them.
    [[nodiscard]] VoidResult force_merge(int32_t max_num_segments) {
        // Every round removes at least one segment, so the number of segments is
        // a safe bound.
        size_t remaining_rounds = infos_.segments.size() + 1;
        while (remaining_rounds-- > 0) {
            const size_t before = pending_merges_.size();
            if (auto queued = update_pending_forced_merges(max_num_segments); !queued.has_value()) {
                return queued;
            }
            if (pending_merges_.size() == before) {
                return {};
            }
            if (auto merged = run_scheduler(MergeTrigger::Explicit); !merged.has_value()) {
                return merged;
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult force_merge_deletes() {
        size_t remaining_rounds = infos_.segments.size() + 1;
        while (remaining_rounds-- > 0) {
            const size_t before = pending_merges_.size();
            if (auto queued = update_pending_forced_deletes_merges(); !queued.has_value()) {
                return queued;
            }
            if (pending_merges_.size() == before) {
                return {};
            }
            if (auto merged = run_scheduler(MergeTrigger::Explicit); !merged.has_value()) {
                return merged;
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult commit() {
        if (auto flushed = flush(); !flushed.has_value()) {
            return flushed;
        }
        if (auto applied = apply_deletes(); !applied.has_value()) {
            return applied;
        }
        documents_writer_->clear_delete_buffer();

        if (auto merged = maybe_merge(MergeTrigger::Commit); !merged.has_value()) {
            return merged;
        }

        infos_.counter = documents_writer_->next_segment_counter();
        ++infos_.version;
        ++infos_.generation;
        if (auto written = infos_.write_to(directory_, io_context_default()); !written.has_value()) {
            return written;
        }
        committed_infos_ = infos_;
        // The segments a merge replaced are unreachable from the commit that was
        // just written.
        for (const std::string &file : orphan_files_) {
            (void)directory_.delete_file(file);
        }
        orphan_files_.clear();
        return {};
    }

    [[nodiscard]] VoidResult rollback() {
        if (auto rolled = documents_writer_->rollback(); !rolled.has_value()) {
            return rolled;
        }
        // Segments written since the last commit are not referenced by any
        // `segments_N`: remove their files and forget about them.
        SegmentInfos uncommitted;
        std::set<std::string> keep = files_of(committed_infos_);
        for (SegmentCommitInfo &segment : infos_.segments) {
            bool committed = false;
            for (const SegmentCommitInfo &known : committed_infos_.segments) {
                if (known.info.name == segment.info.name) {
                    committed = true;
                    break;
                }
            }
            if (!committed) {
                for (const std::string &file : segment.info.files) {
                    if (keep.find(file) == keep.end()) {
                        (void)directory_.delete_file(file);
                    }
                }
            }
        }
        // The merge results are dropped, but the segments they replaced may
        // still be referenced by the committed state.
        for (const std::string &file : orphan_files_) {
            if (keep.find(file) == keep.end()) {
                (void)directory_.delete_file(file);
            }
        }
        orphan_files_.clear();
        infos_ = committed_infos_;
        documents_writer_->set_next_segment_counter(infos_.counter);
        return {};
    }

    Directory &directory_;

    /// The readers handed out by `get_reader`, so consecutive reads reuse them.

    std::unique_ptr<ReaderPool> reader_pool_;
    IndexWriterConfig config_;
    SegmentInfos infos_;
    SegmentInfos committed_infos_;
    std::unique_ptr<DocumentsWriter> documents_writer_;
    /// Merges the policy asked for and that have not run yet.
    std::vector<OneMerge> pending_merges_;
    /// Files of merged-away segments, deleted once the next commit is written.
    std::set<std::string> orphan_files_;
    bool open_{true};
};

IndexWriter::IndexWriter(Directory &directory, IndexWriterConfig config)
    : impl_(std::make_unique<Impl>(directory, std::move(config))) {}

IndexWriter::~IndexWriter() = default;

Result<std::unique_ptr<IndexWriter>> IndexWriter::open(Directory &directory, const IndexWriterConfig &config) {
    auto generations = SegmentInfos::list_generations(directory);
    if (!generations.has_value()) {
        return std::unexpected(std::move(generations.error()));
    }

    std::unique_ptr<IndexWriter> writer(new IndexWriter(directory, config));
    SegmentInfos infos;
    if (generations.value().empty()) {
        // A fresh index: the first commit writes `segments_1`.
        infos.version = 1;
        infos.counter = 0;
        infos.generation = 0;
        infos.index_created_version_major = config.index_created_version_major();
        infos.lucene_version = LUCENE_CURRENT_VERSION;
        const auto random = StringHelper::random_id();
        infos.id.assign(random.begin(), random.end());
    } else {
        auto read = SegmentInfos::read_latest(directory);
        if (!read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        infos = std::move(read.value());
    }

    // Fail early when the configured index sort cannot order documents: the
    // flush path would otherwise fail on every segment.
    if (config.index_sort().has_value()) {
        for (const SortField &field : config.index_sort().value().fields()) {
            auto sorter = field.index_sorter();
            if (!sorter.has_value()) {
                auto text = field.to_string();
                return err(LuceneError::illegal_argument(
                        "cannot sort the index by " + (text.has_value() ? text.value() : std::string("?")) + ": " +
                        sorter.error().to_string()));
            }
        }
    }

    writer->impl_->infos_ = infos;
    writer->impl_->committed_infos_ = std::move(infos);
    writer->impl_->documents_writer_ =
            std::make_unique<DocumentsWriter>(directory, config, writer->impl_->infos_.counter);
    return writer;
}

VoidResult IndexWriter::add_document(const Document &document) {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    if (auto added = impl_->documents_writer_->add_document(document); !added.has_value()) {
        return added;
    }
    return impl_->drain_flushed_segments();
}

VoidResult IndexWriter::update_document(const Term &term, const Document &document) {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // The buffered delete must not touch the document added right after it, so
    // it records how many documents the index held at this point.
    if (auto deleted = impl_->documents_writer_->delete_documents(term, impl_->total_docs());
        !deleted.has_value()) {
        return deleted;
    }
    return add_document(document);
}

VoidResult IndexWriter::delete_documents(const Term &term) {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->documents_writer_->delete_documents(term, impl_->total_docs());
}

VoidResult IndexWriter::delete_all() {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->documents_writer_->delete_all();
}

VoidResult IndexWriter::flush() {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->flush();
}

VoidResult IndexWriter::commit() {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->commit();
}

VoidResult IndexWriter::force_merge(int32_t max_num_segments) {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // The merge has to see the buffered documents and deletes.
    if (auto committed = impl_->commit(); !committed.has_value()) {
        return committed;
    }
    if (auto merged = impl_->force_merge(max_num_segments); !merged.has_value()) {
        return merged;
    }
    // Rewriting `segments_N` makes the merged segment the only visible one and
    // drops the files of the inputs.
    return impl_->commit();
}

VoidResult IndexWriter::force_merge_deletes() {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    if (auto committed = impl_->commit(); !committed.has_value()) {
        return committed;
    }
    if (auto merged = impl_->force_merge_deletes(); !merged.has_value()) {
        return merged;
    }
    return impl_->commit();
}

VoidResult IndexWriter::rollback() {
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    return impl_->rollback();
}

VoidResult IndexWriter::close() {
    if (!impl_->open_) {
        return {};
    }
    if (auto committed = impl_->commit(); !committed.has_value()) {
        impl_->open_ = false;
        return committed;
    }
    impl_->open_ = false;
    return {};
}

Result<std::shared_ptr<DirectoryReader>> IndexWriter::get_reader() { return get_reader(true, false); }

Result<std::shared_ptr<DirectoryReader>> IndexWriter::get_reader(bool apply_all_deletes, bool write_all_deletes) {
    (void)write_all_deletes;
    if (!impl_->open_) {
        return err(LuceneError::already_closed("this IndexWriter is closed"));
    }
    // The buffered documents have to become a segment and the buffered deletes
    // have to reach the `.liv` files before a reader can see them.
    if (auto flushed = impl_->flush(); !flushed.has_value()) {
        return std::unexpected(std::move(flushed.error()));
    }
    if (apply_all_deletes) {
        if (auto applied = impl_->apply_deletes(); !applied.has_value()) {
            return std::unexpected(std::move(applied.error()));
        }
    }

    // Reuse the readers of the previous near-real-time read; the pool reopens the
    // segments that changed (e.g. the ones a commit or a merge rewrote).
    // A commit that applied deletions replaces a segment, so drop the readers of
    // the segments the writer no longer holds.
    impl_->reader_pool_->retain_only(impl_->infos_);

    std::vector<std::shared_ptr<SegmentReader>> readers;
    readers.reserve(impl_->infos_.segments.size());
    for (SegmentCommitInfo &segment : impl_->infos_.segments) {
        auto reader = impl_->reader_pool_->get(segment, impl_->infos_.index_created_version_major,
                                               io_context_default());
        if (!reader.has_value()) {
            return std::unexpected(std::move(reader.error()));
        }
        readers.push_back(std::move(reader.value()));
    }
    return DirectoryReader::open_with_readers(impl_->directory_, impl_->infos_, std::move(readers));
}

Result<int64_t> IndexWriter::num_docs() const {
    int64_t count = 0;
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        count += segment.doc_count();
    }
    count += impl_->documents_writer_->pending_doc_count();
    return count;
}

Result<int64_t> IndexWriter::max_doc() const {
    int64_t count = 0;
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        count += segment.max_doc();
    }
    count += impl_->documents_writer_->pending_doc_count();
    return count;
}

Result<bool> IndexWriter::has_deletions() const {
    for (const SegmentCommitInfo &segment : impl_->infos_.segments) {
        if (segment.total_del_count() > 0) {
            return true;
        }
    }
    return false;
}

const SegmentInfos &IndexWriter::segment_infos() const { return impl_->infos_; }

bool IndexWriter::is_open() const noexcept { return impl_->open_; }

} // namespace lucene
