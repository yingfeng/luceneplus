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

export module lucene_core:rate_limited_directory;

import :types;
import std;

import :directory;
import :error;
import :index_input;
import :index_output;
import :io_context;
import :lock;
import :rate_limiter;

namespace lucene {

/// A [`Directory`] whose created outputs are paced by a [`RateLimiter`].
///
/// Port of `RateLimitedDirectory`.
///
/// Only [`Directory::create_output`] is wrapped: the writer uses it for the
/// merge files, which is the traffic worth limiting.  Temporary outputs (written
/// once by flush, then renamed) and inputs are passed through untouched, as in
/// the reference implementation.
///
/// All outputs returned by one wrapper share the same limiter, so they are paced
/// together as one stream.
export class RateLimitedDirectory final : public Directory {
public:
    using Directory::create_output;
    using Directory::open_input;

    /// Wraps `delegate`, which must outlive the wrapper.
    RateLimitedDirectory(Directory &delegate, std::shared_ptr<RateLimiter> rate_limiter);

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
    [[nodiscard]] bool is_fs_directory() const override { return delegate_.is_fs_directory(); }
    [[nodiscard]] std::string to_string() const override;

    // ---- introspection ----

    [[nodiscard]] Directory &delegate() noexcept { return delegate_; }
    [[nodiscard]] const Directory &delegate() const noexcept { return delegate_; }
    [[nodiscard]] const RateLimiter &rate_limiter() const noexcept { return *rate_limiter_; }

private:
    Directory &delegate_;
    std::shared_ptr<RateLimiter> rate_limiter_;
};

} // namespace lucene
