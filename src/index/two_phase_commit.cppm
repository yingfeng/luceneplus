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

export module lucene_core:two_phase_commit;

import :types;
import std;

import :error;

namespace lucene {

/// A participant in a two-phase commit.
///
/// Port of the `TwoPhaseCommit` trait (`index/two_phase_commit.rs`).
///
/// The caller coordinates [`prepare_commit`] on every participant, then
/// [`commit`] on every participant, and calls [`rollback`] on all of them as soon
/// as one of the two phases fails.  That way a group of writers sharing one
/// directory either commits as a whole or leaves no trace.
export class TwoPhaseCommit {
public:
    virtual ~TwoPhaseCommit() = default;

    /// First phase: do as much work as possible, but do **not** make the changes
    /// visible yet.  Returns the generation the commit will land in.
    [[nodiscard]] virtual Result<int64_t> prepare_commit() = 0;

    /// Second phase: publish what [`prepare_commit`] prepared, and return the new
    /// generation.  This should do very little work.
    [[nodiscard]] virtual Result<int64_t> commit() = 0;

    /// Discards everything since the last successful commit, which is what the
    /// other participants do when one of them fails.
    [[nodiscard]] virtual VoidResult rollback() = 0;
};

} // namespace lucene
