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

module lucene_core:persistent_snapshot_deletion_policy.impl;

import :persistent_snapshot_deletion_policy;

import std;

import :codec_util;
import :data_input;
import :data_output;
import :directory;
import :error;
import :index_commit;
import :index_deletion_policy;
import :index_input;
import :index_output;
import :io_context;
import :snapshot_deletion_policy;
import :types;

namespace lucene {

namespace {

/// Codec name of the snapshot files.
constexpr std::string_view SNAPSHOTS_CODEC = "snapshots";
constexpr int32_t VERSION_START = 0;
constexpr int32_t VERSION_CURRENT = VERSION_START;

std::string snapshots_file_name(int64_t generation) {
    return std::string(SNAPSHOTS_PREFIX) + std::to_string(generation);
}

/// Parses the generation of a `snapshots_<n>` file.
Result<int64_t> parse_generation(std::string_view file) {
    const std::string_view suffix = file.substr(SNAPSHOTS_PREFIX.size());
    if (suffix.empty()) {
        return err(LuceneError::illegal_argument("the snapshot file name has no generation: " + std::string(file)));
    }
    int64_t generation = 0;
    const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), generation);
    if (error != std::errc{} || end != suffix.data() + suffix.size()) {
        return err(LuceneError::illegal_argument("cannot parse the generation of the snapshot file " +
                                                 std::string(file)));
    }
    return generation;
}

/// Deletes `file`, ignoring failures: the reference implementation does the same
/// through `IOUtils.deleteFilesIgnoringExceptions`.
VoidResult delete_ignoring_errors(Directory &directory, std::string_view file) {
    const auto ignored = directory.delete_file(file);
    (void)ignored;
    return {};
}

} // namespace

PersistentSnapshotDeletionPolicy::PersistentSnapshotDeletionPolicy(std::unique_ptr<IndexDeletionPolicy> primary,
                                                                   Directory &directory)
    : base_(std::make_shared<SnapshotDeletionPolicy>(std::move(primary))), directory_(&directory) {}

// ---------------- factories ----------------

Result<std::shared_ptr<PersistentSnapshotDeletionPolicy>>
create_persistent_snapshot_deletion_policy(std::unique_ptr<IndexDeletionPolicy> primary, Directory &directory) {
    return create_persistent_snapshot_deletion_policy(std::move(primary), directory,
                                                      SnapshotOpenMode::kCreateOrAppend);
}

Result<std::shared_ptr<PersistentSnapshotDeletionPolicy>>
create_persistent_snapshot_deletion_policy(std::unique_ptr<IndexDeletionPolicy> primary, Directory &directory,
                                           SnapshotOpenMode mode) {
    std::shared_ptr<PersistentSnapshotDeletionPolicy> policy(
            new PersistentSnapshotDeletionPolicy(std::move(primary), directory));

    const std::lock_guard<std::mutex> lock(policy->mutex_);
    if (mode == SnapshotOpenMode::kCreate) {
        if (auto cleared = policy->clear_prior_snapshots(); !cleared.has_value()) {
            return std::unexpected(std::move(cleared.error()));
        }
    }
    if (auto loaded = policy->load_prior_snapshots(); !loaded.has_value()) {
        return std::unexpected(std::move(loaded.error()));
    }
    if (mode == SnapshotOpenMode::kAppend && policy->next_write_gen_ == 0) {
        return err(LuceneError::illegal_state("no snapshots stored in this directory"));
    }
    return policy;
}

// ---------------- IndexDeletionPolicy ----------------

VoidResult PersistentSnapshotDeletionPolicy::on_init(std::span<const std::shared_ptr<IndexCommit>> commits) {
    return base_->on_init(commits);
}

VoidResult PersistentSnapshotDeletionPolicy::on_commit(std::span<const std::shared_ptr<IndexCommit>> commits) {
    return base_->on_commit(commits);
}

std::string PersistentSnapshotDeletionPolicy::to_string() const {
    return "PersistentSnapshotDeletionPolicy(" + base_->primary().to_string() + ")";
}

// ---------------- snapshots ----------------

Result<std::shared_ptr<IndexCommit>> PersistentSnapshotDeletionPolicy::snapshot() {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto snapshot = base_->snapshot();
    if (!snapshot.has_value()) {
        return std::unexpected(std::move(snapshot.error()));
    }
    if (auto persisted = persist(); !persisted.has_value()) {
        // The snapshot is only real once it is on disk.
        const auto rolled_back = base_->release(*snapshot.value());
        (void)rolled_back;
        return std::unexpected(std::move(persisted.error()));
    }
    return snapshot;
}

VoidResult PersistentSnapshotDeletionPolicy::release(const IndexCommit &commit) {
    return release_gen(commit.generation());
}

VoidResult PersistentSnapshotDeletionPolicy::release_gen(int64_t generation) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (auto released = base_->release_gen(generation); !released.has_value()) {
        return released;
    }
    if (auto persisted = persist(); !persisted.has_value()) {
        // Keep the in-memory state consistent with the file that is still there.
        const auto restored = base_->restore_ref_counts(base_->ref_counts());
        (void)restored;
        return persisted;
    }
    return {};
}

// ---------------- introspection ----------------

std::optional<std::string> PersistentSnapshotDeletionPolicy::last_save_file() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (next_write_gen_ == 0) {
        return std::nullopt;
    }
    return snapshots_file_name(next_write_gen_ - 1);
}

std::vector<std::shared_ptr<IndexCommit>> PersistentSnapshotDeletionPolicy::get_snapshots() const {
    return base_->get_snapshots();
}

int32_t PersistentSnapshotDeletionPolicy::get_snapshot_count() const { return base_->get_snapshot_count(); }

std::shared_ptr<IndexCommit> PersistentSnapshotDeletionPolicy::get_index_commit(int64_t generation) const {
    return base_->get_index_commit(generation);
}

// ---------------- persistence ----------------

VoidResult PersistentSnapshotDeletionPolicy::persist() {
    const int64_t next_write_gen = next_write_gen_;
    const std::string file_name = snapshots_file_name(next_write_gen);
    const std::map<int64_t, int32_t> counts = base_->ref_counts();

    std::optional<LuceneError> first_error;
    auto output = directory_->create_output(file_name, io_context_default());
    if (!output.has_value()) {
        return std::unexpected(std::move(output.error()));
    }

    if (auto header = CodecUtil::write_header(*output.value(), SNAPSHOTS_CODEC, VERSION_CURRENT);
        !header.has_value()) {
        first_error = std::move(header.error());
    }
    if (!first_error.has_value()) {
        if (auto written = output.value()->write_vint(static_cast<int32_t>(counts.size())); !written.has_value()) {
            first_error = std::move(written.error());
        }
    }
    if (!first_error.has_value()) {
        for (const auto &[generation, ref_count] : counts) {
            if (auto written = output.value()->write_vlong(generation); !written.has_value()) {
                first_error = std::move(written.error());
                break;
            }
            if (auto written = output.value()->write_vint(ref_count); !written.has_value()) {
                first_error = std::move(written.error());
                break;
            }
        }
    }
    if (auto closed = output.value()->close(); !closed.has_value() && !first_error.has_value()) {
        first_error = std::move(closed.error());
    }

    if (first_error.has_value()) {
        // A half-written snapshot file must not be mistaken for a real one.
        (void)delete_ignoring_errors(*directory_, file_name);
        return std::unexpected(std::move(first_error.value()));
    }

    const std::vector<std::string> written{file_name};
    if (auto synced = directory_->sync(written); !synced.has_value()) {
        (void)delete_ignoring_errors(*directory_, file_name);
        return synced;
    }

    if (next_write_gen > 0) {
        // The previous snapshot is superseded now; failing to remove it only
        // wastes a file, so the error is ignored.
        (void)delete_ignoring_errors(*directory_, snapshots_file_name(next_write_gen - 1));
    }

    ++next_write_gen_;
    return {};
}

VoidResult PersistentSnapshotDeletionPolicy::clear_prior_snapshots() {
    auto listed = directory_->list_all();
    if (!listed.has_value()) {
        return std::unexpected(std::move(listed.error()));
    }
    for (const std::string &file : listed.value()) {
        if (!file.starts_with(SNAPSHOTS_PREFIX)) {
            continue;
        }
        if (auto deleted = directory_->delete_file(file); !deleted.has_value()) {
            return deleted;
        }
    }
    return {};
}

VoidResult PersistentSnapshotDeletionPolicy::load_prior_snapshots() {
    auto listed = directory_->list_all();
    if (!listed.has_value()) {
        return std::unexpected(std::move(listed.error()));
    }

    std::vector<std::pair<int64_t, std::string>> candidates;
    for (const std::string &file : listed.value()) {
        if (!file.starts_with(SNAPSHOTS_PREFIX)) {
            continue;
        }
        auto generation = parse_generation(file);
        if (!generation.has_value()) {
            return std::unexpected(std::move(generation.error()));
        }
        candidates.emplace_back(generation.value(), file);
    }
    if (candidates.empty()) {
        return {};
    }
    std::ranges::sort(candidates, [](const auto &left, const auto &right) { return left.first < right.first; });

    // The newest readable file wins; older or corrupt ones are dropped below.
    std::optional<LuceneError> first_io_error;
    std::optional<int64_t> loaded_generation;
    std::map<int64_t, int32_t> loaded_counts;
    for (const auto &[generation, file] : candidates) {
        auto input = directory_->open_input(file, io_context_default());
        if (!input.has_value()) {
            if (!first_io_error.has_value()) {
                first_io_error = std::move(input.error());
            }
            continue;
        }

        std::map<int64_t, int32_t> counts;
        std::optional<LuceneError> read_error;
        auto version = CodecUtil::check_header(*input.value(), SNAPSHOTS_CODEC, VERSION_START, VERSION_CURRENT);
        if (!version.has_value()) {
            read_error = std::move(version.error());
        }
        if (!read_error.has_value()) {
            auto count = input.value()->read_vint();
            if (!count.has_value()) {
                read_error = std::move(count.error());
            } else {
                for (int32_t i = 0; i < count.value(); ++i) {
                    auto commit_generation = input.value()->read_vlong();
                    if (!commit_generation.has_value()) {
                        read_error = std::move(commit_generation.error());
                        break;
                    }
                    auto ref_count = input.value()->read_vint();
                    if (!ref_count.has_value()) {
                        read_error = std::move(ref_count.error());
                        break;
                    }
                    counts[commit_generation.value()] = ref_count.value();
                }
            }
        }
        if (auto closed = input.value()->close(); !closed.has_value() && !read_error.has_value()) {
            read_error = std::move(closed.error());
        }

        if (read_error.has_value()) {
            if (!first_io_error.has_value() && read_error.value().is_io_error()) {
                first_io_error = std::move(read_error.value());
            }
            continue;
        }
        loaded_generation = generation;
        loaded_counts = std::move(counts);
    }

    if (!loaded_generation.has_value()) {
        // Nothing was loaded, and not for lack of trying.
        if (first_io_error.has_value()) {
            return std::unexpected(std::move(first_io_error.value()));
        }
        return {};
    }

    if (auto restored = base_->restore_ref_counts(loaded_counts); !restored.has_value()) {
        return restored;
    }
    for (const auto &[generation, file] : candidates) {
        if (generation != loaded_generation.value()) {
            (void)delete_ignoring_errors(*directory_, file);
        }
    }
    next_write_gen_ = loaded_generation.value() + 1;
    return {};
}

} // namespace lucene
