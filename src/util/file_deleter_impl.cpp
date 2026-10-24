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

module lucene_core:file_deleter.impl;

import :file_deleter;

import std;

import :directory;
import :error;
import :types;

namespace lucene {

namespace {

/// The commit file prefix.  Kept local so that the store-level utility does not
/// depend on the index layer (`:index_file_names`).
constexpr std::string_view SEGMENTS_PREFIX = "segments";

} // namespace

FileDeleter::FileDeleter(Directory &directory, std::shared_ptr<FileDeleterListener> listener)
    : directory_(directory), listener_(std::move(listener)) {}

VoidResult FileDeleter::inc_ref(std::span<const std::string> files) {
    for (const std::string &file : files) {
        if (auto increased = inc_ref(file); !increased.has_value()) {
            return increased;
        }
    }
    return {};
}

VoidResult FileDeleter::inc_ref(std::string_view file) {
    auto found = ref_counts_.find(file);
    if (found == ref_counts_.end()) {
        // First sighting: the file becomes known with a count of one.
        RefCount count{std::string(file), false, 0};
        found = ref_counts_.emplace(std::string(file), std::move(count)).first;
    }
    if (auto notified = notify(FileDeleterMsg::kRef,
                               "IncRef \"" + std::string(file) + "\": pre-incr count is " +
                                       std::to_string(found->second.count));
        !notified.has_value()) {
        return notified;
    }
    ++found->second.count;
    return {};
}

bool FileDeleter::dec_ref_one(std::string_view file) {
    auto found = ref_counts_.find(file);
    if (found == ref_counts_.end()) {
        return false;
    }
    // The reference implementation reports the count before decreasing it; a
    // failure there is only a logging failure, so it cannot change the outcome.
    (void)notify(FileDeleterMsg::kRef,
                 "DecRef \"" + std::string(file) + "\": pre-decr count is " + std::to_string(found->second.count));
    if (found->second.count == 0) {
        return false;
    }
    --found->second.count;
    if (found->second.count == 0) {
        ref_counts_.erase(found);
        return true;
    }
    return false;
}

VoidResult FileDeleter::dec_ref(std::span<const std::string> files) {
    std::vector<std::string> to_delete;
    std::optional<LuceneError> first_error;
    for (const std::string &file : files) {
        if (dec_ref_one(file)) {
            to_delete.push_back(file);
        }
    }
    // Every file is decref'd first, so a deletion failure cannot leave the counts
    // inconsistent.
    if (auto deleted = delete_files(to_delete); !deleted.has_value()) {
        first_error = std::move(deleted.error());
    }
    if (first_error.has_value()) {
        return std::unexpected(std::move(first_error.value()));
    }
    return {};
}

void FileDeleter::init_ref_count(std::string_view file) {
    if (ref_counts_.find(file) == ref_counts_.end()) {
        ref_counts_.emplace(std::string(file), RefCount{std::string(file), false, 0});
    }
}

size_t FileDeleter::ref_count(std::string_view file) const {
    const auto found = ref_counts_.find(file);
    return found == ref_counts_.end() ? 0 : found->second.count;
}

std::vector<std::string> FileDeleter::all_files() const {
    std::vector<std::string> files;
    files.reserve(ref_counts_.size());
    for (const auto &[name, count] : ref_counts_) {
        files.push_back(name);
    }
    return files;
}

bool FileDeleter::exists(std::string_view file) const { return ref_count(file) > 0; }

std::vector<std::string> FileDeleter::unrefed_files() const {
    std::vector<std::string> unrefed;
    for (const auto &[name, count] : ref_counts_) {
        if (count.count == 0) {
            (void)notify(FileDeleterMsg::kFile, "removing unreferenced file \"" + name + "\"");
            unrefed.push_back(name);
        }
    }
    return unrefed;
}

VoidResult FileDeleter::delete_files_if_no_ref(std::span<const std::string> files) {
    std::vector<std::string> to_delete;
    for (const std::string &file : files) {
        // A known file with count 0 is normal after a crash: the writer removed
        // the files of a dead segment and later reused its name.
        if (!exists(file)) {
            (void)notify(FileDeleterMsg::kFile, "will delete new file \"" + file + "\"");
            to_delete.push_back(file);
        }
    }
    return delete_files(to_delete);
}

VoidResult FileDeleter::force_delete(std::string_view file) {
    ref_counts_.erase(std::string(file));
    return delete_file(file);
}

VoidResult FileDeleter::delete_file_if_no_ref(std::string_view file) {
    if (exists(file)) {
        return {};
    }
    (void)notify(FileDeleterMsg::kFile, "will delete new file \"" + std::string(file) + "\"");
    return delete_file(file);
}

VoidResult FileDeleter::delete_files(std::span<const std::string> files) {
    if (!files.empty()) {
        (void)notify(FileDeleterMsg::kFile, "now delete " + std::to_string(files.size()) + " files");
    }

    // Commit files go first: if we crash now, no stale commit points at files we
    // have already removed.
    for (const std::string &file : files) {
        if (file.starts_with(SEGMENTS_PREFIX)) {
            if (auto deleted = delete_file(file); !deleted.has_value()) {
                return deleted;
            }
        }
    }
    for (const std::string &file : files) {
        if (!file.starts_with(SEGMENTS_PREFIX)) {
            if (auto deleted = delete_file(file); !deleted.has_value()) {
                return deleted;
            }
        }
    }
    return {};
}

VoidResult FileDeleter::delete_file(std::string_view file) { return directory_.delete_file(file); }

VoidResult FileDeleter::notify(FileDeleterMsg type, const std::string &message) const {
    if (listener_ == nullptr) {
        return {};
    }
    return listener_->accept(type, message);
}

} // namespace lucene
