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

export module lucene_core:index_commit;

import :types;
import std;

import :directory;
import :error;

namespace lucene {

/// A point-in-time view of an index: one `segments_N` file and the files it
/// references.
///
/// Port of the `IndexCommit` trait (`core/index/index_commit.rs`).
///
/// A commit point is deleted either by the writer (when the
/// [`IndexDeletionPolicy`](:index_deletion_policy) says so) or when it is
/// released; [`delete_commit`] only marks it, and the writer's file deleter does
/// the actual removal.
export class IndexCommit {
public:
    virtual ~IndexCommit() = default;

    /// The `segments_N` file this commit point stands for.
    [[nodiscard]] virtual const std::string &segments_file_name() const = 0;

    /// Every index file the commit references.
    [[nodiscard]] virtual const std::vector<std::string> &file_names() const = 0;

    /// The directory the commit lives in.
    [[nodiscard]] virtual Directory &directory() const = 0;

    /// Marks the commit for deletion; the writer's file deleter removes it.
    [[nodiscard]] virtual VoidResult delete_commit() const = 0;

    /// Whether the commit was marked for deletion.
    [[nodiscard]] virtual bool is_deleted() const = 0;

    /// How many segments the commit references.
    [[nodiscard]] virtual size_t segment_count() const = 0;

    /// The generation, i.e. the `N` of `segments_N`.
    [[nodiscard]] virtual int64_t generation() const = 0;

    /// The user data recorded with the commit.
    [[nodiscard]] virtual const std::map<std::string, std::string> &user_data() const = 0;

    /// A short description, for diagnostics.
    [[nodiscard]] virtual std::string to_string() const = 0;
};

/// Whether `first` and `second` are the same commit point.
///
/// Port of `is_same_commit` (`core/index/index_commit.rs`).
///
/// **Deviation:** the reference implementation asks the directory whether it is
/// the same directory (`Directory::is_same_identity`); the port compares the
/// [`Directory`] objects themselves, so two objects over the same path compare
/// unequal.
[[nodiscard]] bool is_same_commit(const IndexCommit &first, const IndexCommit &second);

/// Orders two commit points of the same directory by generation, the oldest
/// first.
///
/// Port of `cmp_commit` (`core/index/index_commit.rs`).
[[nodiscard]] int32_t cmp_commit(const IndexCommit &first, const IndexCommit &second);

} // namespace lucene
