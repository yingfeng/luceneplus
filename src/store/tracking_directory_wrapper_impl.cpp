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

module lucene_core:tracking_directory_wrapper.impl;

import :tracking_directory_wrapper;

import std;

import :directory;
import :error;
import :types;

namespace lucene {

TrackingDirectoryWrapper::TrackingDirectoryWrapper(Directory &delegate) : delegate_(delegate) {}

std::set<std::string> TrackingDirectoryWrapper::created_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return created_;
}

bool TrackingDirectoryWrapper::has_created_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !created_.empty();
}

std::set<std::string> TrackingDirectoryWrapper::take_created_files() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> taken;
    taken.swap(created_);
    return taken;
}

void TrackingDirectoryWrapper::clear_created_files() {
    std::lock_guard<std::mutex> lock(mutex_);
    created_.clear();
}

Result<std::vector<std::string>> TrackingDirectoryWrapper::list_all() const { return delegate_.list_all(); }

VoidResult TrackingDirectoryWrapper::delete_file(std::string_view name) {
    if (auto deleted = delegate_.delete_file(name); !deleted.has_value()) {
        return deleted;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    created_.erase(std::string(name));
    return {};
}

Result<size_t> TrackingDirectoryWrapper::file_length(std::string_view name) const {
    return delegate_.file_length(name);
}

Result<std::unique_ptr<IndexOutput>> TrackingDirectoryWrapper::create_output(std::string_view name,
                                                                             const IOContext &context) {
    auto output = delegate_.create_output(name, context);
    if (!output.has_value()) {
        return std::unexpected(std::move(output.error()));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        created_.insert(std::string(name));
    }
    return output;
}

Result<CreatedOutput> TrackingDirectoryWrapper::create_temp_output(std::string_view prefix, std::string_view suffix,
                                                                   const IOContext &context) {
    auto output = delegate_.create_temp_output(prefix, suffix, context);
    if (!output.has_value()) {
        return std::unexpected(std::move(output.error()));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        created_.insert(output.value().name);
    }
    return output;
}

VoidResult TrackingDirectoryWrapper::sync(std::span<const std::string> names) { return delegate_.sync(names); }

VoidResult TrackingDirectoryWrapper::sync_metadata() { return delegate_.sync_metadata(); }

VoidResult TrackingDirectoryWrapper::rename(std::string_view source, std::string_view dest) {
    return delegate_.rename(source, dest);
}

Result<std::unique_ptr<IndexInput>> TrackingDirectoryWrapper::open_input(std::string_view name,
                                                                         const IOContext &context) const {
    return delegate_.open_input(name, context);
}

Result<std::unique_ptr<Lock>> TrackingDirectoryWrapper::obtain_lock(std::string_view name) {
    return delegate_.obtain_lock(name);
}

std::string TrackingDirectoryWrapper::to_string() const {
    return "TrackingDirectoryWrapper(" + delegate_.to_string() + ")";
}

} // namespace lucene
