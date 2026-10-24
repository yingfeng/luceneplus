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

module lucene_core:file_switch_directory.impl;

import :file_switch_directory;

import std;

import :directory;
import :error;
import :types;

namespace lucene {

namespace {

/// Whether `value` is one or more ASCII letters.
[[nodiscard]] bool is_letters(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char character : value) {
        const bool is_letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        if (!is_letter) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string get_extension(std::string_view name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }

    std::string_view extension = name.substr(dot + 1);
    if (extension != "tmp") {
        return std::string(extension);
    }

    // `_0.fdt.tmp` is an `fdt` file: look for the first `.` followed by letters
    // in the part before the final dot, which is what the reference
    // implementation's regular expression finds.
    const std::string_view prefix = name.substr(0, dot + 1);
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i] != '.') {
            continue;
        }
        size_t end = i + 1;
        while (end < prefix.size() && is_letters(prefix.substr(end, 1))) {
            ++end;
        }
        if (end > i + 1) {
            return std::string(prefix.substr(i + 1, end - i - 1));
        }
    }
    return std::string(extension);
}

FileSwitchDirectory::FileSwitchDirectory(std::set<std::string> primary_extensions, std::shared_ptr<Directory> primary,
                                         std::shared_ptr<Directory> secondary)
        : primary_extensions_(std::move(primary_extensions)),
          primary_(std::move(primary)),
          secondary_(std::move(secondary)) {}

Result<std::unique_ptr<FileSwitchDirectory>> FileSwitchDirectory::create(std::vector<std::string> primary_extensions,
                                                                         std::shared_ptr<Directory> primary,
                                                                         std::shared_ptr<Directory> secondary) {
    if (primary == nullptr || secondary == nullptr) {
        return err(LuceneError::illegal_argument("both directories must not be null"));
    }
    std::set<std::string> extensions;
    for (auto &extension : primary_extensions) {
        if (extension == "tmp") {
            return err(LuceneError::illegal_argument("tmp is a reserved extension"));
        }
        extensions.insert(std::move(extension));
    }
    return std::unique_ptr<FileSwitchDirectory>(
            new FileSwitchDirectory(std::move(extensions), std::move(primary), std::move(secondary)));
}

bool FileSwitchDirectory::is_primary(std::string_view name) const {
    return primary_extensions_.contains(get_extension(name));
}

Directory &FileSwitchDirectory::directory_for(std::string_view name) const {
    return is_primary(name) ? *primary_ : *secondary_;
}

Result<std::vector<std::string>> FileSwitchDirectory::list_all() const {
    std::vector<std::string> files;

    // Either directory may be a file system directory that was never written to,
    // in which case listing it fails; that only matters when both do.
    std::optional<LuceneError> primary_error;
    auto primary_files = primary_->list_all();
    if (primary_files.has_value()) {
        for (auto &file : primary_files.value()) {
            if (primary_extensions_.contains(get_extension(file))) {
                files.push_back(std::move(file));
            }
        }
    } else {
        primary_error = std::move(primary_files.error());
    }

    auto secondary_files = secondary_->list_all();
    if (!secondary_files.has_value()) {
        if (primary_error.has_value()) {
            return std::unexpected(std::move(primary_error.value()));
        }
        if (files.empty()) {
            return std::unexpected(std::move(secondary_files.error()));
        }
        return files;
    }
    for (auto &file : secondary_files.value()) {
        if (!primary_extensions_.contains(get_extension(file))) {
            files.push_back(std::move(file));
        }
    }
    return files;
}

VoidResult FileSwitchDirectory::delete_file(std::string_view name) {
    return directory_for(name).delete_file(name);
}

Result<size_t> FileSwitchDirectory::file_length(std::string_view name) const {
    return directory_for(name).file_length(name);
}

Result<std::unique_ptr<IndexOutput>> FileSwitchDirectory::create_output(std::string_view name,
                                                                       const IOContext &context) {
    return directory_for(name).create_output(name, context);
}

Result<CreatedOutput> FileSwitchDirectory::create_temp_output(std::string_view prefix, std::string_view suffix,
                                                             const IOContext &context) {
    // Temporary files have no extension to route on, so they go to the secondary
    // directory, which is where the reference implementation puts them too.
    return secondary_->create_temp_output(prefix, suffix, context);
}

VoidResult FileSwitchDirectory::sync(std::span<const std::string> names) {
    // Group the names by directory, so each directory only syncs its own files.
    std::vector<std::string> primary_names;
    std::vector<std::string> secondary_names;
    for (const std::string &name : names) {
        if (is_primary(name)) {
            primary_names.push_back(name);
        } else {
            secondary_names.push_back(name);
        }
    }
    if (auto synced = primary_->sync(primary_names); !synced.has_value()) {
        return synced;
    }
    return secondary_->sync(secondary_names);
}

VoidResult FileSwitchDirectory::sync_metadata() {
    if (auto synced = primary_->sync_metadata(); !synced.has_value()) {
        return synced;
    }
    return secondary_->sync_metadata();
}

VoidResult FileSwitchDirectory::rename(std::string_view source, std::string_view dest) {
    if (is_primary(source) != is_primary(dest)) {
        return err(LuceneError::illegal_argument("cannot rename \"" + std::string(source) + "\" to \"" +
                                                 std::string(dest) + "\": the two files live in different directories"));
    }
    return directory_for(source).rename(source, dest);
}

Result<std::unique_ptr<IndexInput>> FileSwitchDirectory::open_input(std::string_view name,
                                                                   const IOContext &context) const {
    return directory_for(name).open_input(name, context);
}

Result<std::unique_ptr<Lock>> FileSwitchDirectory::obtain_lock(std::string_view name) {
    return directory_for(name).obtain_lock(name);
}

std::string FileSwitchDirectory::to_string() const {
    return "FileSwitchDirectory(primary=" + primary_->to_string() + ", secondary=" + secondary_->to_string() + ")";
}

} // namespace lucene
