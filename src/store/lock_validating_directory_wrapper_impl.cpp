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

module lucene_core:lock_validating_directory_wrapper.impl;

import :lock_validating_directory_wrapper;

import std;

import :directory;
import :error;
import :types;

namespace lucene {

LockValidatingDirectoryWrapper::LockValidatingDirectoryWrapper(Directory &delegate, Lock &write_lock)
    : delegate_(delegate), write_lock_(write_lock) {}

VoidResult LockValidatingDirectoryWrapper::delete_file(std::string_view name) {
    if (auto valid = write_lock_.ensure_valid(); !valid.has_value()) {
        return valid;
    }
    return delegate_.delete_file(name);
}

Result<std::unique_ptr<IndexOutput>> LockValidatingDirectoryWrapper::create_output(std::string_view name,
                                                                                  const IOContext &context) {
    if (auto valid = write_lock_.ensure_valid(); !valid.has_value()) {
        return std::unexpected(std::move(valid.error()));
    }
    return delegate_.create_output(name, context);
}

VoidResult LockValidatingDirectoryWrapper::sync(std::span<const std::string> names) {
    if (auto valid = write_lock_.ensure_valid(); !valid.has_value()) {
        return valid;
    }
    return delegate_.sync(names);
}

VoidResult LockValidatingDirectoryWrapper::sync_metadata() {
    if (auto valid = write_lock_.ensure_valid(); !valid.has_value()) {
        return valid;
    }
    return delegate_.sync_metadata();
}

VoidResult LockValidatingDirectoryWrapper::rename(std::string_view source, std::string_view dest) {
    if (auto valid = write_lock_.ensure_valid(); !valid.has_value()) {
        return valid;
    }
    return delegate_.rename(source, dest);
}

VoidResult LockValidatingDirectoryWrapper::copy_from(const Directory &from, std::string_view src,
                                                    std::string_view dest, const IOContext &context) {
    if (auto valid = write_lock_.ensure_valid(); !valid.has_value()) {
        return valid;
    }
    return delegate_.copy_from(from, src, dest, context);
}

Result<std::vector<std::string>> LockValidatingDirectoryWrapper::list_all() const { return delegate_.list_all(); }

Result<size_t> LockValidatingDirectoryWrapper::file_length(std::string_view name) const {
    return delegate_.file_length(name);
}

Result<CreatedOutput> LockValidatingDirectoryWrapper::create_temp_output(std::string_view prefix,
                                                                        std::string_view suffix,
                                                                        const IOContext &context) {
    return delegate_.create_temp_output(prefix, suffix, context);
}

Result<std::unique_ptr<IndexInput>> LockValidatingDirectoryWrapper::open_input(std::string_view name,
                                                                              const IOContext &context) const {
    return delegate_.open_input(name, context);
}

Result<std::unique_ptr<Lock>> LockValidatingDirectoryWrapper::obtain_lock(std::string_view name) {
    return delegate_.obtain_lock(name);
}

std::string LockValidatingDirectoryWrapper::to_string() const {
    return "LockValidatingDirectoryWrapper(" + delegate_.to_string() + ")";
}

} // namespace lucene
