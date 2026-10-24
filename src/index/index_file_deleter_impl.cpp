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

module lucene_core:index_file_deleter.impl;

import :index_file_deleter;

import std;

import :directory;
import :error;
import :file_deleter;
import :index_commit;
import :index_deletion_policy;
import :index_file_names;
import :info_stream;
import :segment_commit_info;
import :segment_infos;
import :types;

namespace lucene {

namespace {

/// The files of one segment, including its `.liv` / `.fnm` / `.dvd` updates.
void append_segment_files(const SegmentCommitInfo &info, std::vector<std::string> &files) {
    for (const std::string &file : info.info.files) {
        files.push_back(file);
    }
    for (const std::string &file : info.field_infos_files) {
        files.push_back(file);
    }
    for (const auto &[generation, updates] : info.doc_values_updates_files) {
        for (const std::string &file : updates) {
            files.push_back(file);
        }
    }
}

/// Splits a segment file name into its segment name and its generation.
///
/// `_0.si` becomes (`_0`, 0) and `_0_3.liv` becomes (`_0`, 3).  Anything that
/// does not follow the convention is returned unchanged with generation 0.
std::pair<std::string, int64_t> split_segment_file(std::string_view file) {
    const std::string base = IndexFileNames::strip_extension(file);
    const size_t underscore = base.rfind('_');
    if (underscore != std::string::npos && underscore > 0) {
        auto generation = IndexFileNames::base36_decode(std::string_view(base).substr(underscore + 1));
        if (generation.has_value()) {
            return {base.substr(0, underscore), generation.value()};
        }
    }
    return {base, 0};
}

/// The generation encoded in `segments_7` (or `pending_segments_7`).
std::optional<int64_t> commit_generation(std::string_view file, std::string_view base) {
    auto generation = IndexFileNames::generation_from_segment_file_name(file, base);
    if (!generation.has_value()) {
        return std::nullopt;
    }
    return generation.value();
}

} // namespace

bool is_codec_file(std::string_view file) { return file.starts_with('_'); }

std::vector<std::string> segment_files(const SegmentInfos &infos) {
    std::vector<std::string> files;
    for (const SegmentCommitInfo &info : infos.segments) {
        append_segment_files(info, files);
    }
    return files;
}

std::vector<std::string> commit_files(const SegmentInfos &infos) {
    std::vector<std::string> files;
    files.push_back(infos.generation_file_name());
    for (const SegmentCommitInfo &info : infos.segments) {
        append_segment_files(info, files);
    }
    return files;
}

VoidResult inflate_gens(SegmentInfos &infos, std::span<const std::string> files, const InfoStreamMT &info_stream) {
    int64_t max_commit_generation = std::numeric_limits<int64_t>::min();
    int64_t max_segment_name = std::numeric_limits<int64_t>::min();
    std::map<std::string, int64_t, std::less<>> max_per_segment_generation;

    for (const std::string &file : files) {
        if (file == WRITE_LOCK_NAME) {
            continue;
        }
        if (file.starts_with(IndexFileNames::SEGMENTS)) {
            // Junk files starting with "segments" are tolerated, like the
            // reference implementation does.
            if (auto generation = commit_generation(file, IndexFileNames::SEGMENTS); generation.has_value()) {
                max_commit_generation = std::max(max_commit_generation, generation.value());
            }
            continue;
        }
        if (file.starts_with(IndexFileNames::PENDING_SEGMENTS)) {
            // `pending_segments_7` carries the same generation as `segments_7`.
            constexpr std::string_view PENDING_PREFIX = "pending_";
            if (auto generation = commit_generation(file.substr(PENDING_PREFIX.size()), IndexFileNames::SEGMENTS);
                generation.has_value()) {
                max_commit_generation = std::max(max_commit_generation, generation.value());
            }
            continue;
        }
        if (!is_codec_file(file) || file.ends_with(".tmp")) {
            continue;
        }
        auto [segment_name, generation] = split_segment_file(file);
        if (segment_name.size() <= 1) {
            continue;
        }
        if (auto index = IndexFileNames::base36_decode(std::string_view(segment_name).substr(1)); index.has_value()) {
            max_segment_name = std::max(max_segment_name, index.value());
        }
        max_per_segment_generation[segment_name] =
                std::max(max_per_segment_generation[segment_name], generation);
    }

    if (max_commit_generation != std::numeric_limits<int64_t>::min()) {
        infos.generation = std::max(infos.generation, max_commit_generation);
    }
    if (max_segment_name != std::numeric_limits<int64_t>::min()) {
        const int64_t desired = max_segment_name + 1;
        if (infos.counter < desired) {
            if (info_stream != nullptr && info_stream->is_enabled("IFD")) {
                (void)info_stream->message("IFD", "init: inflate infos.counter to " + std::to_string(desired) +
                                                          " vs current=" + std::to_string(infos.counter));
            }
            infos.counter = desired;
        }
    }
    for (SegmentCommitInfo &info : infos.segments) {
        const auto found = max_per_segment_generation.find(info.info.name);
        if (found == max_per_segment_generation.end()) {
            continue;
        }
        const int64_t generation = found->second;
        if (info.del_gen < generation) {
            info.del_gen = generation;
        }
        if (info.field_infos_gen < generation) {
            info.field_infos_gen = generation;
        }
        if (info.doc_values_gen < generation) {
            info.doc_values_gen = generation;
        }
    }
    return {};
}

// ---------------- CommitPoint ----------------

CommitPoint::CommitPoint(Directory &directory, const SegmentInfos &infos,
                         std::shared_ptr<std::atomic<bool>> commits_to_delete)
    : directory_(directory),
      files_(commit_files(infos)),
      // Qualified: the member function of the same name hides the free one.
      segment_files_(::lucene::segment_files(infos)),
      segments_file_name_(infos.generation_file_name()),
      generation_(infos.generation),
      segment_count_(infos.segments.size()),
      user_data_(infos.user_data),
      commits_to_delete_(std::move(commits_to_delete)) {}

VoidResult CommitPoint::delete_commit() const {
    if (!deleted_.load(std::memory_order_seq_cst)) {
        deleted_.store(true, std::memory_order_seq_cst);
        if (commits_to_delete_ != nullptr) {
            commits_to_delete_->store(true, std::memory_order_seq_cst);
        }
    }
    return {};
}

// ---------------- IndexFileDeleter ----------------

namespace {

/// Forwards [`FileDeleter`] diagnostics to the info stream.
class InfoStreamMessenger final : public FileDeleterListener {
public:
    explicit InfoStreamMessenger(InfoStreamMT info_stream) : info_stream_(std::move(info_stream)) {}

    [[nodiscard]] VoidResult accept(FileDeleterMsg type, std::string_view message) override {
        // Reference-count chatter is only interesting when debugging the writer.
        if (type == FileDeleterMsg::kRef) {
            return {};
        }
        if (info_stream_ != nullptr && info_stream_->is_enabled("IFD")) {
            return info_stream_->message("IFD", message);
        }
        return {};
    }

private:
    InfoStreamMT info_stream_;
};

} // namespace

IndexFileDeleter::IndexFileDeleter(Directory &directory, InfoStreamMT info_stream)
    : commits_to_delete_(std::make_shared<std::atomic<bool>>(false)),
      info_stream_(std::move(info_stream)),
      file_deleter_(directory, std::make_shared<InfoStreamMessenger>(info_stream_)) {}

Result<std::shared_ptr<IndexFileDeleter>>
create_index_file_deleter(Directory &directory, IndexDeletionPolicy &policy, SegmentInfos &infos) {
    return create_index_file_deleter(directory, policy, infos, get_default_info_stream(), true);
}

Result<std::shared_ptr<IndexFileDeleter>>
create_index_file_deleter(Directory &directory, IndexDeletionPolicy &policy, SegmentInfos &infos,
                          InfoStreamMT info_stream, bool initial_index_exists) {
    if (info_stream == nullptr) {
        info_stream = get_default_info_stream();
    }
    std::shared_ptr<IndexFileDeleter> deleter(new IndexFileDeleter(directory, std::move(info_stream)));

    auto listed = directory.list_all();
    if (!listed.has_value()) {
        return std::unexpected(std::move(listed.error()));
    }

    if (auto logged = deleter->log("init: current segments file is \"" + infos.generation_file_name() +
                                   "\"; deletionPolicy=" + policy.to_string());
        !logged.has_value()) {
        return std::unexpected(std::move(logged.error()));
    }

    std::shared_ptr<CommitPoint> current_commit;
    if (initial_index_exists) {
        for (const std::string &file : listed.value()) {
            if (file == WRITE_LOCK_NAME) {
                continue;
            }
            const bool is_commit_file = file.starts_with(IndexFileNames::SEGMENTS);
            const bool is_pending = file.starts_with(IndexFileNames::PENDING_SEGMENTS);
            if (!is_codec_file(file) && !is_commit_file && !is_pending) {
                continue;
            }
            // Register the file with a count of zero; the commits below incref
            // whatever they really use.
            deleter->file_deleter_.init_ref_count(file);
            if (!is_commit_file) {
                continue;
            }
            auto generation = commit_generation(file, IndexFileNames::SEGMENTS);
            if (!generation.has_value()) {
                continue; // Junk file: not a commit.
            }
            if (auto logged = deleter->log("init: load commit \"" + file + "\""); !logged.has_value()) {
                return std::unexpected(std::move(logged.error()));
            }
            auto read = SegmentInfos::read_file(directory, file, generation.value());
            if (!read.has_value()) {
                return std::unexpected(std::move(read.error()));
            }
            auto commit = std::make_shared<CommitPoint>(directory, read.value(), deleter->commits_to_delete_);
            if (read.value().generation == infos.generation) {
                current_commit = commit;
            }
            deleter->commits_.push_back(commit);
            if (auto increased = deleter->inc_ref_from_segment(read.value(), true); !increased.has_value()) {
                return std::unexpected(std::move(increased.error()));
            }
        }
    }

    if (current_commit == nullptr && initial_index_exists) {
        // The listing was stale (an NFS client with a cached directory, say), yet
        // the commit must exist: the caller holds the write lock.
        if (auto logged = deleter->log("forced open of current segments file " + infos.generation_file_name());
            !logged.has_value()) {
            return std::unexpected(std::move(logged.error()));
        }
        auto read = SegmentInfos::read_file(directory, infos.generation_file_name(), infos.generation);
        if (!read.has_value()) {
            return std::unexpected(std::move(read.error()));
        }
        auto commit = std::make_shared<CommitPoint>(directory, read.value(), deleter->commits_to_delete_);
        current_commit = commit;
        deleter->commits_.push_back(commit);
        if (auto increased = deleter->inc_ref_from_segment(read.value(), true); !increased.has_value()) {
            return std::unexpected(std::move(increased.error()));
        }
    }

    // Oldest first, which is the order the deletion policies expect.
    std::ranges::sort(deleter->commits_, [](const std::shared_ptr<CommitPoint> &left,
                                            const std::shared_ptr<CommitPoint> &right) {
        return left->generation() < right->generation();
    });

    if (auto inflated = inflate_gens(infos, listed.value(), deleter->info_stream_); !inflated.has_value()) {
        return std::unexpected(std::move(inflated.error()));
    }

    // Anything no commit references is a leftover from a crashed writer.
    const std::vector<std::string> unrefed = deleter->file_deleter_.unrefed_files();
    for (const std::string &file : unrefed) {
        if (file.starts_with(IndexFileNames::SEGMENTS)) {
            return err(LuceneError::illegal_state("file \"" + file +
                                                  "\" has refCount=0, which should never happen on init"));
        }
    }
    if (auto deleted = deleter->file_deleter_.delete_files_if_no_ref(unrefed); !deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
    }

    const std::vector<std::shared_ptr<IndexCommit>> views = deleter->commits();
    if (auto initialized = policy.on_init(views); !initialized.has_value()) {
        return std::unexpected(std::move(initialized.error()));
    }

    // The incoming state may not be the newest commit, so protect it explicitly.
    if (auto checkpointed = deleter->checkpoint(infos, false, policy); !checkpointed.has_value()) {
        return std::unexpected(std::move(checkpointed.error()));
    }

    deleter->starting_commit_deleted_ = current_commit != nullptr && current_commit->is_deleted();
    if (auto deleted = deleter->delete_commits(); !deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
    }
    return deleter;
}

VoidResult IndexFileDeleter::inc_ref_from_segment(const SegmentInfos &infos, bool is_commit) {
    const std::vector<std::string> files = is_commit ? commit_files(infos) : segment_files(infos);
    return file_deleter_.inc_ref(files);
}

VoidResult IndexFileDeleter::checkpoint(SegmentInfos &infos, bool is_commit, IndexDeletionPolicy &policy) {
    if (auto increased = inc_ref_from_segment(infos, is_commit); !increased.has_value()) {
        return increased;
    }

    if (is_commit) {
        commits_.push_back(std::make_shared<CommitPoint>(file_deleter_.directory(), infos, commits_to_delete_));
        const std::vector<std::shared_ptr<IndexCommit>> views = commits();
        if (auto committed = policy.on_commit(views); !committed.has_value()) {
            return committed;
        }
        return delete_commits();
    }

    // Release the previous checkpoint, then remember this one.
    std::vector<std::string> previous = std::move(last_files_);
    last_files_.clear();
    if (auto released = dec_ref(previous); !released.has_value()) {
        return released;
    }
    last_files_ = segment_files(infos);
    return {};
}

VoidResult IndexFileDeleter::delete_commits() {
    if (!commits_to_delete_->load(std::memory_order_seq_cst)) {
        return {};
    }

    std::vector<std::shared_ptr<CommitPoint>> removed;
    for (const std::shared_ptr<CommitPoint> &commit : commits_) {
        if (commit->is_deleted()) {
            removed.push_back(commit);
        }
    }

    std::optional<LuceneError> first_error;
    for (const std::shared_ptr<CommitPoint> &commit : removed) {
        if (auto logged = log("deleteCommits: now decRef commit \"" + commit->segments_file_name() + "\"");
            !logged.has_value()) {
            return logged;
        }
        if (auto released = dec_ref(commit->file_names()); !released.has_value() && !first_error.has_value()) {
            first_error = std::move(released.error());
        }
    }
    commits_to_delete_->store(false, std::memory_order_seq_cst);

    std::erase_if(commits_, [](const std::shared_ptr<CommitPoint> &commit) { return commit->is_deleted(); });

    if (first_error.has_value()) {
        return std::unexpected(std::move(first_error.value()));
    }
    return {};
}

VoidResult IndexFileDeleter::refresh() {
    auto listed = file_deleter_.directory().list_all();
    if (!listed.has_value()) {
        return std::unexpected(std::move(listed.error()));
    }
    std::vector<std::string> to_delete;
    for (const std::string &file : listed.value()) {
        if (file == WRITE_LOCK_NAME || file_deleter_.exists(file)) {
            continue;
        }
        if (is_codec_file(file) || file.starts_with(IndexFileNames::SEGMENTS) ||
            file.starts_with(IndexFileNames::PENDING_SEGMENTS)) {
            if (auto logged = log("refresh: removing newly created unreferenced file \"" + file + "\"");
                !logged.has_value()) {
                return logged;
            }
            to_delete.push_back(file);
        }
    }
    return file_deleter_.delete_files_if_no_ref(to_delete);
}

VoidResult IndexFileDeleter::revisit_policy(IndexDeletionPolicy &policy) {
    if (commits_.empty()) {
        return {};
    }
    if (auto logged = log("now revisitPolicy"); !logged.has_value()) {
        return logged;
    }
    const std::vector<std::shared_ptr<IndexCommit>> views = commits();
    if (auto visited = policy.on_commit(views); !visited.has_value()) {
        return visited;
    }
    return delete_commits();
}

VoidResult IndexFileDeleter::inc_ref(std::span<const std::string> files) { return file_deleter_.inc_ref(files); }

VoidResult IndexFileDeleter::dec_ref(std::span<const std::string> files) { return file_deleter_.dec_ref(files); }

VoidResult IndexFileDeleter::dec_ref_from_segment(const SegmentInfos &infos) {
    return file_deleter_.dec_ref(segment_files(infos));
}

VoidResult IndexFileDeleter::delete_new_files(std::span<const std::string> files) {
    return file_deleter_.delete_files_if_no_ref(files);
}

std::vector<std::shared_ptr<IndexCommit>> IndexFileDeleter::commits() const {
    std::vector<std::shared_ptr<IndexCommit>> view;
    view.reserve(commits_.size());
    for (const std::shared_ptr<CommitPoint> &commit : commits_) {
        view.push_back(commit);
    }
    return view;
}

VoidResult IndexFileDeleter::close() {
    if (last_files_.empty()) {
        return {};
    }
    std::vector<std::string> files = std::move(last_files_);
    last_files_.clear();
    return file_deleter_.dec_ref(files);
}

VoidResult IndexFileDeleter::log(const std::string &message) const {
    if (info_stream_ != nullptr && info_stream_->is_enabled("IFD")) {
        return info_stream_->message("IFD", message);
    }
    return {};
}

} // namespace lucene
