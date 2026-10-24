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

export module lucene_core:file_switch_directory;

import :types;
import std;

import :directory;
import :error;

namespace lucene {

/// The extension of `name`, i.e. the text after its last `.`.
///
/// Port of `get_extension` (`core/store/file_switch_directory.rs`).
///
/// A temporary file (`<something>.tmp`) reports the extension it was derived
/// from, so `_0.fdt.tmp` is still an `fdt` file.
[[nodiscard]] std::string get_extension(std::string_view name);

/// A [`Directory`] that splits its files between two other directories by
/// extension.
///
/// Port of `FileSwitchDirectory` (`core/store/file_switch_directory.rs`).
///
/// Files whose extension is in `primary_extensions` go to the primary directory,
/// everything else goes to the secondary one; locks follow the same rule, so both
/// directories should use the same lock factory.  Listing merges both, filtering
/// by extension so a file that both directories happen to see is listed once.
///
/// **Deviation:** the reference implementation owns its two directories by value
/// and closes them when asked; the port shares them, so the caller decides when
/// they close.
export class FileSwitchDirectory final : public Directory {
public:
    /// Routes the extensions in `primary_extensions` to `primary`, the rest to
    /// `secondary`.  `tmp` is reserved and rejected.
    [[nodiscard]] static Result<std::unique_ptr<FileSwitchDirectory>> create(
            std::vector<std::string> primary_extensions, std::shared_ptr<Directory> primary,
            std::shared_ptr<Directory> secondary);

    [[nodiscard]] Directory &primary_dir() const noexcept { return *primary_; }
    [[nodiscard]] Directory &secondary_dir() const noexcept { return *secondary_; }

    // ---- Directory ----

    [[nodiscard]] Result<std::vector<std::string>> list_all() const override;
    [[nodiscard]] VoidResult delete_file(std::string_view name) override;
    [[nodiscard]] Result<size_t> file_length(std::string_view name) const override;
    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                     const IOContext &context) override;
    [[nodiscard]] Result<CreatedOutput> create_temp_output(std::string_view prefix, std::string_view suffix,
                                                           const IOContext &context) override;
    [[nodiscard]] VoidResult sync(std::span<const std::string> names) override;
    [[nodiscard]] VoidResult sync_metadata() override;
    [[nodiscard]] VoidResult rename(std::string_view source, std::string_view dest) override;
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                 const IOContext &context) const override;
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override;
    [[nodiscard]] bool is_fs_directory() const override { return primary_->is_fs_directory(); }
    [[nodiscard]] std::string to_string() const override;

private:
    FileSwitchDirectory(std::set<std::string> primary_extensions, std::shared_ptr<Directory> primary,
                        std::shared_ptr<Directory> secondary);

    /// The directory a file of that name lives in.
    [[nodiscard]] Directory &directory_for(std::string_view name) const;
    [[nodiscard]] bool is_primary(std::string_view name) const;

    std::set<std::string> primary_extensions_;
    std::shared_ptr<Directory> primary_;
    std::shared_ptr<Directory> secondary_;
};

} // namespace lucene
