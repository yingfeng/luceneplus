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

module lucene_core:filter_directory_reader.impl;

import :filter_directory_reader;

import std;

import :error;
import :leaf_reader;
import :standard_directory_reader;
import :types;

namespace lucene {

Result<std::vector<std::shared_ptr<LeafReader>>>
SubReaderWrapper::wrap_readers(std::vector<std::shared_ptr<LeafReader>> readers) const {
    std::vector<std::shared_ptr<LeafReader>> wrapped;
    wrapped.reserve(readers.size());
    for (std::shared_ptr<LeafReader> &reader : readers) {
        auto one = wrap(std::move(reader));
        if (!one.has_value()) {
            return std::unexpected(std::move(one.error()));
        }
        if (one.value() == nullptr) {
            return err(LuceneError::illegal_argument("a wrapped leaf must not be null"));
        }
        wrapped.push_back(std::move(one.value()));
    }
    return wrapped;
}

Result<std::shared_ptr<DirectoryReader>>
wrap_directory_reader(std::shared_ptr<DirectoryReader> delegate, const SubReaderWrapper &wrapper) {
    if (delegate == nullptr) {
        return err(LuceneError::illegal_argument("the delegate reader must not be null"));
    }
    auto wrapped = wrapper.wrap_readers(delegate->leaf_readers());
    if (!wrapped.has_value()) {
        return std::unexpected(std::move(wrapped.error()));
    }
    return DirectoryReader::open_with_leaves(delegate->directory(), delegate->segment_infos(),
                                            std::move(wrapped.value()));
}

} // namespace lucene
