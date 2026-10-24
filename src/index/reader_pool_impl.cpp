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

module lucene_core:reader_pool.impl;

import :reader_pool;

import std;

import :directory;
import :error;
import :io_context;
import :segment_commit_info;
import :segment_infos;
import :segment_reader;
import :types;

namespace lucene {

Result<std::shared_ptr<SegmentReader>> ReaderPool::get(SegmentCommitInfo &info, int32_t created_version_major,
                                                       const IOContext &context) {
    if (closed_) {
        return err(LuceneError::already_closed("this ReaderPool is closed"));
    }

    auto cached = readers_.find(info.info.name);
    if (cached != readers_.end()) {
        const SegmentReader &reader = *cached->second;
        const SegmentCommitInfo &held = reader.segment_info();
        if (held.del_gen == info.del_gen && held.max_doc() == info.max_doc() &&
            reader.created_version_major() == created_version_major) {
            return cached->second;
        }
        // The segment moved on (new deletions, or a merge rewrote it): the cached
        // reader no longer describes it.
        readers_.erase(cached);
    }

    auto opened = SegmentReader::open(*directory_, info, created_version_major, context);
    if (!opened.has_value()) {
        return std::unexpected(std::move(opened.error()));
    }
    readers_[info.info.name] = opened.value();
    return opened.value();
}

std::shared_ptr<SegmentReader> ReaderPool::find(std::string_view segment_name) const {
    const auto found = readers_.find(segment_name);
    return found == readers_.end() ? nullptr : found->second;
}

void ReaderPool::drop(const SegmentCommitInfo &info) {
    const auto found = readers_.find(info.info.name);
    if (found != readers_.end()) {
        readers_.erase(found);
    }
}

void ReaderPool::drop_all() { readers_.clear(); }

void ReaderPool::retain_only(const SegmentInfos &infos) {
    std::set<std::string> live;
    for (const SegmentCommitInfo &segment : infos.segments) {
        live.insert(segment.info.name);
    }
    for (auto entry = readers_.begin(); entry != readers_.end();) {
        if (live.contains(entry->first)) {
            ++entry;
        } else {
            entry = readers_.erase(entry);
        }
    }
}

VoidResult ReaderPool::close() {
    if (closed_) {
        return {};
    }
    // Readers are shared with the `DirectoryReader`s handed out, so this only
    // releases the pool's own references; a live reader stays usable.
    for (auto &entry : readers_) {
        if (auto closed_reader = entry.second->close(); !closed_reader.has_value()) {
            return closed_reader;
        }
    }
    readers_.clear();
    closed_ = true;
    return {};
}

} // namespace lucene
