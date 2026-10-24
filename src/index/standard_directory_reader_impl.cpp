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

module lucene_core:standard_directory_reader.impl;

import :standard_directory_reader;

import :base_composite_reader;
import :error;
import :index_commit;
import :index_file_deleter;
import :index_file_names;
import :index_reader;
import :leaf_reader_context;
import :segment_infos;
import :segment_reader;
import :stored_fields;
import :term_vectors;
import :types;

import std;

namespace lucene {

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open(Directory &dir, const IOContext &context) {
    auto infos = SegmentInfos::read_latest(dir, context);
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    return open_infos(dir, std::move(infos.value()), context);
}

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open_infos(Directory &dir, SegmentInfos infos,
                                                                     const IOContext &context) {
    std::shared_ptr<DirectoryReader> reader(new DirectoryReader());
    reader->dir_ = &dir;
    reader->infos_ = std::move(infos);

    // A missing commit file is a bug in the caller, not an empty index: an empty
    // index still has a `segments_N`, it just lists no segments.
    reader->segment_readers_.reserve(reader->infos_.size());
    std::vector<std::shared_ptr<IndexReader>> sub_readers;
    sub_readers.reserve(reader->infos_.size());
    for (const SegmentCommitInfo &info : reader->infos_.segments) {
        auto segment = SegmentReader::open(dir, info, reader->infos_.index_created_version_major, context);
        if (!segment.has_value()) {
            return std::unexpected(std::move(segment.error()));
        }
        reader->leaf_readers_.push_back(segment.value());
        sub_readers.push_back(segment.value());
        reader->segment_readers_.push_back(std::move(segment.value()));
    }

    auto base = CompositeReaderBase::create(std::move(sub_readers));
    if (!base.has_value()) {
        return std::unexpected(std::move(base.error()));
    }
    reader->base_ = std::move(base.value());
    return reader;
}

namespace {

/// A read-only [`IndexCommit`] over the `segments_N` file of a directory.
///
/// Port of `ReaderCommit`.  It never deletes anything: deleting a commit is the
/// writer's job, through its `IndexFileDeleter`.
class ReaderCommit final : public IndexCommit {
public:
    ReaderCommit(Directory &directory, SegmentInfos infos, std::string segments_file_name)
        : directory_(&directory),
          infos_(std::move(infos)),
          segments_file_name_(std::move(segments_file_name)),
          file_names_(commit_files(infos_)) {
        file_names_.push_back(segments_file_name_);
    }

    [[nodiscard]] const std::string &segments_file_name() const override { return segments_file_name_; }

    [[nodiscard]] const std::vector<std::string> &file_names() const override { return file_names_; }

    [[nodiscard]] Directory &directory() const override { return *directory_; }

    [[nodiscard]] VoidResult delete_commit() const override {
        return err(LuceneError::unsupported_operation(
                "only an IndexWriter may delete a commit; this commit point is a read-only view"));
    }

    [[nodiscard]] bool is_deleted() const override { return false; }

    [[nodiscard]] size_t segment_count() const override { return infos_.segments.size(); }

    [[nodiscard]] int64_t generation() const override { return infos_.generation; }

    [[nodiscard]] const std::map<std::string, std::string> &user_data() const override {
        return infos_.user_data;
    }

    [[nodiscard]] std::string to_string() const override {
        return "ReaderCommit(segments=" + segments_file_name_ + ", generation=" + std::to_string(infos_.generation) +
               ")";
    }

private:
    Directory *directory_;
    SegmentInfos infos_;
    std::string segments_file_name_;
    std::vector<std::string> file_names_;
};

} // namespace

Result<std::vector<std::shared_ptr<IndexCommit>>> DirectoryReader::list_commits(Directory &dir) {
    auto generations = SegmentInfos::list_generations(dir);
    if (!generations.has_value()) {
        return std::unexpected(std::move(generations.error()));
    }

    std::vector<std::shared_ptr<IndexCommit>> commits;
    commits.reserve(generations.value().size());
    for (const int64_t generation : generations.value()) {
        auto infos = SegmentInfos::read(dir, generation);
        if (!infos.has_value()) {
            return std::unexpected(std::move(infos.error()));
        }
        const std::string file_name =
                std::string(IndexFileNames::SEGMENTS) + "_" + std::to_string(generation);
        commits.push_back(std::make_shared<ReaderCommit>(dir, std::move(infos.value()), file_name));
    }
    return commits;
}

Result<bool> DirectoryReader::index_exists(Directory &dir) {
    auto files = dir.list_all();
    if (!files.has_value()) {
        return std::unexpected(std::move(files.error()));
    }
    for (const std::string &file : files.value()) {
        if (file.starts_with(std::string(IndexFileNames::SEGMENTS) + "_")) {
            return true;
        }
    }
    return false;
}

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open_with_readers(
        Directory &dir, SegmentInfos infos, std::vector<std::shared_ptr<SegmentReader>> segment_readers) {
    if (segment_readers.size() != infos.size()) {
        return err(LuceneError::illegal_argument("expected " + std::to_string(infos.size()) + " segment readers, got " +
                                                 std::to_string(segment_readers.size())));
    }
    for (const std::shared_ptr<SegmentReader> &reader : segment_readers) {
        if (reader == nullptr) {
            return err(LuceneError::illegal_argument("a segment reader must not be null"));
        }
    }

    std::shared_ptr<DirectoryReader> reader(new DirectoryReader());
    reader->dir_ = &dir;
    reader->infos_ = std::move(infos);

    std::vector<std::shared_ptr<IndexReader>> sub_readers;
    sub_readers.reserve(segment_readers.size());
    reader->segment_readers_.reserve(segment_readers.size());
    for (std::shared_ptr<SegmentReader> &segment : segment_readers) {
        reader->leaf_readers_.push_back(segment);
        sub_readers.push_back(segment);
        reader->segment_readers_.push_back(std::move(segment));
    }

    auto base = CompositeReaderBase::create(std::move(sub_readers));
    if (!base.has_value()) {
        return std::unexpected(std::move(base.error()));
    }
    reader->base_ = std::move(base.value());
    return reader;
}

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open_with_sorter(Directory &dir, const LeafSorter &sorter,
                                                                            const IOContext &context) {
    if (!sorter) {
        return err(LuceneError::illegal_argument("the leaf sorter must be set"));
    }
    auto latest = SegmentInfos::read_latest(dir);
    if (!latest.has_value()) {
        return std::unexpected(std::move(latest.error()));
    }

    std::vector<std::shared_ptr<SegmentReader>> segments;
    segments.reserve(latest.value().segments.size());
    for (const SegmentCommitInfo &info : latest.value().segments) {
        auto segment = SegmentReader::open(dir, info, latest.value().index_created_version_major, context);
        if (!segment.has_value()) {
            return std::unexpected(std::move(segment.error()));
        }
        segments.push_back(std::move(segment.value()));
    }

    // A std::sort comparator cannot fail, so the first error is remembered and the
    // comparison reports "equal"; the open fails once the sort returns.
    std::optional<LuceneError> failure;
    std::stable_sort(segments.begin(), segments.end(),
                     [&sorter, &failure](const std::shared_ptr<SegmentReader> &left,
                                         const std::shared_ptr<SegmentReader> &right) {
                         if (failure.has_value()) {
                             return false;
                         }
                         auto compared = sorter(*left, *right);
                         if (!compared.has_value()) {
                             failure = std::move(compared.error());
                             return false;
                         }
                         return compared.value() < 0;
                     });
    if (failure.has_value()) {
        return std::unexpected(std::move(failure.value()));
    }

    // The order of the leaves is the sorter's, so the reader's document order
    // differs from the commit's segment order.
    return open_with_leaves(dir, std::move(latest.value()), std::vector<std::shared_ptr<LeafReader>>(
                                                                  segments.begin(), segments.end()));
}

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open_with_leaves(
        Directory &dir, SegmentInfos infos, std::vector<std::shared_ptr<LeafReader>> leaf_readers) {
    for (const std::shared_ptr<LeafReader> &leaf : leaf_readers) {
        if (leaf == nullptr) {
            return err(LuceneError::illegal_argument("a leaf reader must not be null"));
        }
    }

    std::shared_ptr<DirectoryReader> reader(new DirectoryReader());
    reader->dir_ = &dir;
    reader->infos_ = std::move(infos);

    // A wrapped leaf is not a `SegmentReader`, so `segment_readers()` stays empty
    // here; the leaves themselves are what the base is built from.
    std::vector<std::shared_ptr<IndexReader>> sub_readers;
    sub_readers.reserve(leaf_readers.size());
    reader->leaf_readers_.reserve(leaf_readers.size());
    for (std::shared_ptr<LeafReader> &leaf : leaf_readers) {
        sub_readers.push_back(leaf);
        reader->leaf_readers_.push_back(std::move(leaf));
    }

    auto base = CompositeReaderBase::create(std::move(sub_readers));
    if (!base.has_value()) {
        return std::unexpected(std::move(base.error()));
    }
    reader->base_ = std::move(base.value());
    return reader;
}

Result<std::shared_ptr<DirectoryReader>>
DirectoryReader::open_with_version(const IndexCommit &commit, int32_t min_supported_major_version) {
    auto infos = SegmentInfos::read_file(commit.directory(), commit.segments_file_name(), commit.generation());
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    if (infos.value().index_created_version_major < min_supported_major_version) {
        return err(LuceneError::illegal_argument(
                "this index was created with version " + std::to_string(infos.value().index_created_version_major) +
                ", which is older than the minimum supported major version " +
                std::to_string(min_supported_major_version)));
    }
    return open_infos(commit.directory(), std::move(infos.value()));
}

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open_commit(const IndexCommit &commit) {
    auto infos = SegmentInfos::read_file(commit.directory(), commit.segments_file_name(), commit.generation());
    if (!infos.has_value()) {
        return std::unexpected(std::move(infos.error()));
    }
    return open_infos(commit.directory(), std::move(infos.value()));
}

Result<std::shared_ptr<DirectoryReader>>
DirectoryReader::open_if_changed_with_commit(const IndexCommit &commit) const {
    if (dir_ == nullptr) {
        return err(LuceneError::illegal_state("DirectoryReader was not opened from a directory"));
    }
    if (&commit.directory() == dir_ && commit.generation() == infos_.generation) {
        return std::shared_ptr<DirectoryReader>{};
    }
    return open_commit(commit);
}

Result<std::shared_ptr<DirectoryReader>> DirectoryReader::open_if_changed() const {
    if (dir_ == nullptr) {
        return err(LuceneError::illegal_state("DirectoryReader was not opened from a directory"));
    }
    auto latest = SegmentInfos::read_latest(*dir_);
    if (!latest.has_value()) {
        return std::unexpected(std::move(latest.error()));
    }
    if (latest.value().version == infos_.version) {
        return std::shared_ptr<DirectoryReader>{};
    }
    return open_infos(*dir_, std::move(latest.value()));
}

Result<std::span<const LeafReaderContext>> DirectoryReader::leaves() const {
    // The leaves are the generic leaf readers, not the segment readers: a wrapped
    // reader (see `wrap_directory_reader`) has no segment readers at all.
    const size_t leaves_num = leaf_readers_.size();
    if (leaves_.size() != leaves_num) {
        leaves_.clear();
        leaves_.reserve(leaves_num);
        const TopParentMeta top_parent{static_cast<int32_t>(leaves_num), base_.max_doc(), identity_};
        for (size_t i = 0; i < leaves_num; ++i) {
            leaves_.emplace_back(leaf_readers_[i].get(), static_cast<int32_t>(i), base_.starts()[i], top_parent);
        }
    }
    return std::span<const LeafReaderContext>(leaves_);
}

Result<std::unique_ptr<StoredFields>> DirectoryReader::stored_fields() { return base_.stored_fields(); }

Result<std::unique_ptr<TermVectors>> DirectoryReader::term_vectors() { return base_.term_vectors(); }

Result<bool> DirectoryReader::is_current() const {
    if (dir_ == nullptr) {
        return err(LuceneError::illegal_state("DirectoryReader was not opened from a directory"));
    }
    auto latest = SegmentInfos::read_latest(*dir_);
    if (!latest.has_value()) {
        return std::unexpected(std::move(latest.error()));
    }
    return latest.value().version == infos_.version;
}

} // namespace lucene
