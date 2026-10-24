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

export module lucene_core:error;

import :types;
import std;

namespace lucene {

/// Error taxonomy of the reference implementation (`core/util/error`).
///
/// The reference implementation models every failure as a distinct struct (`IllegalArgumentError`,
/// `Eof`, ...).  For C++ a single tagged error type is cheaper to propagate and
/// keeps `std::expected` ergonomic while preserving the original taxonomy.
export enum class ErrorKind : int32_t {
    kNone = 0,
    kIo,
    kIllegalArgument,
    kIllegalState,
    kArrayIndexOutOfBounds,
    kAlreadyClosed,
    kBufferAllocation,
    kCollectionTerminated,
    kConcurrentModification,
    kCorruptIndex,
    kEof,
    kFuzzyTerms,
    kIndexFormatTooNew,
    kIndexFormatTooOld,
    kIndexNotFound,
    kLockAlreadyHeld,
    kLockHeldByOther,
    kLockObtainFailed,
    kLockReleaseFailed,
    kMaxBytesLengthExceeded,
    kMergeAborted,
    kMerge,
    kNeedImplemented,
    kNoMoreTerms,
    kNoSuchElement,
    kNotImplemented,
    kNotSuchFile,
    kNumberFormat,
    kNumberOverflow,
    kTimeExceeded,
    kTooComplexToDeterminize,
    kTooManyClauses,
    kTooManyNestedClauses,
    kTragedy,
    kUncheckedIo,
    kUnreachable,
    kUnsupportedOperation,
    kVersion,
    kParse,
    kOther,
};

export std::string_view error_kind_name(ErrorKind kind);

/// A recoverable failure.  Mirrors `LuceneError`.
export class LuceneError {
public:
    LuceneError() = default;
    explicit LuceneError(ErrorKind kind, std::string message = {});

    // ---- Factories mirroring the `LuceneError::xxx(..)` constructors ----
    static LuceneError io(std::string message);
    static LuceneError illegal_argument(std::string message);
    static LuceneError illegal_state(std::string message);
    static LuceneError array_index_out_of_bounds(std::string message);
    static LuceneError already_closed(std::string message);
    static LuceneError buffer_allocation(std::string message);
    static LuceneError collection_terminated(std::string message);
    static LuceneError concurrent_modification(std::string message);
    static LuceneError corrupt_index(std::string message);
    static LuceneError eof(std::string message);
    static LuceneError fuzzy_terms(std::string message);
    static LuceneError index_format_too_new(std::string message);
    static LuceneError index_format_too_old(std::string message);
    static LuceneError index_not_found(std::string message);
    static LuceneError lock_already_held(std::string message);
    static LuceneError lock_held_by_other(std::string message);
    static LuceneError lock_obtain_failed(std::string message);
    static LuceneError lock_release_failed(std::string message);
    static LuceneError max_bytes_length_exceeded(std::string message);
    static LuceneError merge_aborted(std::string message);
    static LuceneError merge(std::string message);
    static LuceneError need_implemented(std::string message);
    static LuceneError no_more_terms(std::string message);
    static LuceneError no_such_element(std::string message);
    static LuceneError not_implemented(std::string message);
    static LuceneError not_such_file(std::string message);
    static LuceneError number_format(std::string message);
    static LuceneError number_overflow(std::string message);
    static LuceneError time_exceeded(std::string message);
    static LuceneError too_complex_to_determinize(std::string message);
    static LuceneError too_many_clauses(std::string message);
    static LuceneError too_many_nested_clauses(std::string message);
    static LuceneError tragedy(std::string message);
    static LuceneError unchecked_io(std::string message);
    static LuceneError unreachable(std::string message);
    static LuceneError unsupported_operation(std::string message);
    static LuceneError version(std::string message);
    static LuceneError parse(std::string message);

    [[nodiscard]] ErrorKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string &message() const noexcept { return message_; }

    /// `"<kind>: <message>"`, the string form used in logs and tests.
    [[nodiscard]] std::string to_string() const;

    /// Attaches a suppressed failure (the C++ analogue of `add_suppressed`).
    void add_suppressed(const LuceneError &error);
    [[nodiscard]] const std::shared_ptr<const LuceneError> &suppressed() const noexcept { return suppressed_; }

    [[nodiscard]] bool is_eof() const noexcept { return kind_ == ErrorKind::kEof; }
    [[nodiscard]] bool is_io() const noexcept { return kind_ == ErrorKind::kIo; }

    /// Whether this is an I/O-related failure, in the reference implementation's
    /// broad sense: a corrupt or unreadable index, a missing file or a lock
    /// problem.  Callers that report the first failure of a batch of closes use
    /// it to distinguish "the storage complained" from "the caller misused the
    /// API", which is returned immediately.
    [[nodiscard]] bool is_io_error() const noexcept;

    /// Whether this is a state failure, including "already closed".
    [[nodiscard]] bool is_illegal_state_error() const noexcept {
        return kind_ == ErrorKind::kIllegalState || kind_ == ErrorKind::kAlreadyClosed;
    }

private:
    ErrorKind kind_{ErrorKind::kNone};
    std::string message_{};
    std::shared_ptr<const LuceneError> suppressed_{};
};

/// `Result<T>`: the reference implementation's `Result<T>` = `std::expected<T, LuceneError>`.
export template <class T>
using Result = std::expected<T, LuceneError>;

/// `Result<()>`.
export using VoidResult = std::expected<void, LuceneError>;

export std::string to_string(const LuceneError &error);

} // namespace lucene

export namespace lucene {

/// `std::unexpected` shorthand used by the port: `return err(LuceneError::eof("..."))`.
inline std::unexpected<LuceneError> err(LuceneError error) { return std::unexpected<LuceneError>(std::move(error)); }

/// Wrap an `std::error_code`-producing call into a `LuceneError`.
inline LuceneError io_error(const std::error_code &ec, std::string_view context) {
    std::string message(context);
    if (!ec.message().empty()) {
        message.append(": ").append(ec.message());
    }
    return LuceneError::io(std::move(message));
}

} // namespace lucene
