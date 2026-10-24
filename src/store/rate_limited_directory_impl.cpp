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

module lucene_core:rate_limited_directory.impl;

import :rate_limited_directory;

import std;

import :directory;
import :error;
import :types;

import :rate_limited_index_output;

namespace lucene {

RateLimitedDirectory::RateLimitedDirectory(Directory &delegate, std::shared_ptr<RateLimiter> rate_limiter)
    : delegate_(delegate), rate_limiter_(std::move(rate_limiter)) {}

Result<std::vector<std::string>> RateLimitedDirectory::list_all() const { return delegate_.list_all(); }

VoidResult RateLimitedDirectory::delete_file(std::string_view name) { return delegate_.delete_file(name); }

Result<size_t> RateLimitedDirectory::file_length(std::string_view name) const {
    return delegate_.file_length(name);
}

Result<std::unique_ptr<IndexOutput>> RateLimitedDirectory::create_output(std::string_view name,
                                                                         const IOContext &context) {
    auto output = delegate_.create_output(name, context);
    if (!output.has_value()) {
        return std::unexpected(std::move(output.error()));
    }
    return std::unique_ptr<IndexOutput>(
            new RateLimitedIndexOutput(rate_limiter_, std::move(output.value())));
}

Result<CreatedOutput> RateLimitedDirectory::create_temp_output(std::string_view prefix, std::string_view suffix,
                                                               const IOContext &context) {
    return delegate_.create_temp_output(prefix, suffix, context);
}

VoidResult RateLimitedDirectory::sync(std::span<const std::string> names) { return delegate_.sync(names); }

VoidResult RateLimitedDirectory::sync_metadata() { return delegate_.sync_metadata(); }

VoidResult RateLimitedDirectory::rename(std::string_view source, std::string_view dest) {
    return delegate_.rename(source, dest);
}

Result<std::unique_ptr<IndexInput>> RateLimitedDirectory::open_input(std::string_view name,
                                                                     const IOContext &context) const {
    return delegate_.open_input(name, context);
}

Result<std::unique_ptr<Lock>> RateLimitedDirectory::obtain_lock(std::string_view name) {
    return delegate_.obtain_lock(name);
}

std::string RateLimitedDirectory::to_string() const {
    return "RateLimitedDirectory(" + delegate_.to_string() + ")";
}

} // namespace lucene
