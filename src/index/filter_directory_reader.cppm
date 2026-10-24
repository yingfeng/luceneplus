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

export module lucene_core:filter_directory_reader;

import :types;
import std;

import :error;
import :leaf_reader;
import :standard_directory_reader;

namespace lucene {

/// Turns the leaves of a reader into the leaves of the reader above it.
///
/// Port of `SubReaderWrapper` (`index/filter_directory_reader.rs`).
///
/// A wrapper may wrap each leaf (soft deletes hide the documents of a "hard"
/// leaf behind a filtered one) or drop whole leaves; the number of leaves may
/// therefore shrink.
export class SubReaderWrapper {
public:
    virtual ~SubReaderWrapper() = default;

    /// The leaves of the wrapped reader, in document order.
    ///
    /// The default wraps every leaf with [`wrap`](Self::wrap).
    [[nodiscard]] virtual Result<std::vector<std::shared_ptr<LeafReader>>>
    wrap_readers(std::vector<std::shared_ptr<LeafReader>> readers) const;

    /// One leaf.
    [[nodiscard]] virtual Result<std::shared_ptr<LeafReader>> wrap(std::shared_ptr<LeafReader> reader) const = 0;
};

/// A reader over the leaves of `delegate`, transformed by `wrapper`.
///
/// Port of `FilterDirectoryReader` (`index/filter_directory_reader.rs`).
///
/// **Deviations** (C++ mechanics and scope):
///  * the reference implementation is a trait a subclass implements together with
///    `do_wrap_directory_reader`; here it is one function, because this port has
///    no `CacheHelper` / `ClosedListener` machinery to forward;
///  * the result reports no [`segment_readers`](DirectoryReader::segment_readers),
///    since a wrapped leaf is not a `SegmentReader`; use
///    [`DirectoryReader::leaf_readers`] instead;
///  * `open_if_changed` on the result re-reads the directory and does **not**
///    re-apply the wrapper: the caller wraps the new reader again, which is what
///    the reference implementation's `wrap_directory_reader` hook is for.
///
/// The wrapped leaves have to be kept alive by `wrapper` or by the leaves
/// themselves; the result holds them through its base.
[[nodiscard]] Result<std::shared_ptr<DirectoryReader>>
wrap_directory_reader(std::shared_ptr<DirectoryReader> delegate, const SubReaderWrapper &wrapper);

} // namespace lucene
