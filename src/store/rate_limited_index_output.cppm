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

export module lucene_core:rate_limited_index_output;

import :types;
import std;

import :data_input;
import :data_output;
import :index_output;
import :rate_limiter;

namespace lucene {

/// An [`IndexOutput`] that paces its writes through a [`RateLimiter`].
///
/// Port of `RateLimitedIndexOutput`.
///
/// The wrapper counts the bytes it writes and only reports to the limiter once
/// more than [`RateLimiter::min_pause_check_bytes`] have accumulated; asking on
/// every write would cost a lock per byte while telling the limiter nothing new.
/// The pause is paid *before* the bytes are handed to the delegate, and a single
/// `write_bytes` is never split (see LUCENE-10448): a large buffer can therefore
/// exceed the instantaneous rate even though the average stays within it.
export class RateLimitedIndexOutput final : public IndexOutput {
public:
    /// Takes ownership of `out`; `rate_limiter` is shared with the other streams
    /// that must be paced together (one merge, for instance).
    RateLimitedIndexOutput(std::shared_ptr<RateLimiter> rate_limiter, std::unique_ptr<IndexOutput> out);

    // ---- DataOutput ----

    [[nodiscard]] VoidResult write_byte(uint8_t b) override;
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override;
    [[nodiscard]] VoidResult write_int(int32_t i) override;
    [[nodiscard]] VoidResult write_short(int16_t i) override;
    [[nodiscard]] VoidResult write_long(int64_t i) override;

    // ---- IndexOutput ----

    [[nodiscard]] Result<size_t> get_file_pointer() const override;
    [[nodiscard]] uint64_t get_checksum() override;
    [[nodiscard]] std::string_view get_name() const override;
    [[nodiscard]] VoidResult close() override;

    /// Delegated as-is, so the padding written by the delegate is not counted
    /// here; the next writes only report their own bytes.
    [[nodiscard]] Result<size_t> align_file_pointer(size_t alignment_bytes) override;

    [[nodiscard]] std::string to_string() const override;

    // ---- introspection ----

    [[nodiscard]] IndexOutput &delegate() noexcept { return *out_; }
    [[nodiscard]] const IndexOutput &delegate() const noexcept { return *out_; }
    [[nodiscard]] const RateLimiter &rate_limiter() const noexcept { return *rate_limiter_; }

private:
    /// Reports the bytes accumulated since the last pause, if enough of them.
    [[nodiscard]] VoidResult check_rate();

    std::shared_ptr<RateLimiter> rate_limiter_;
    std::unique_ptr<IndexOutput> out_;
    /// Bytes written since the last [`RateLimiter::pause`].
    int64_t bytes_since_last_pause_{0};
    /// Cached so the common path does not have to ask the limiter.
    int64_t current_min_pause_check_bytes_{0};
};

} // namespace lucene
