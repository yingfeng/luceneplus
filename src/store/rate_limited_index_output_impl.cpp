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

module lucene_core:rate_limited_index_output.impl;

import :rate_limited_index_output;

import std;

import :bit_util;
import :error;
import :types;

namespace lucene {

RateLimitedIndexOutput::RateLimitedIndexOutput(std::shared_ptr<RateLimiter> rate_limiter,
                                               std::unique_ptr<IndexOutput> out)
    : rate_limiter_(std::move(rate_limiter)),
      out_(std::move(out)),
      current_min_pause_check_bytes_(rate_limiter_->min_pause_check_bytes()) {}

VoidResult RateLimitedIndexOutput::check_rate() {
    if (bytes_since_last_pause_ > current_min_pause_check_bytes_) {
        auto paused = rate_limiter_->pause(bytes_since_last_pause_);
        if (!paused.has_value()) {
            return std::unexpected(std::move(paused.error()));
        }
        bytes_since_last_pause_ = 0;
        current_min_pause_check_bytes_ = rate_limiter_->min_pause_check_bytes();
    }
    return {};
}

VoidResult RateLimitedIndexOutput::write_byte(uint8_t b) {
    bytes_since_last_pause_ += 1;
    if (auto r = check_rate(); !r) {
        return r;
    }
    return out_->write_byte(b);
}

VoidResult RateLimitedIndexOutput::write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) {
    bytes_since_last_pause_ += static_cast<int64_t>(length);
    if (auto r = check_rate(); !r) {
        return r;
    }
    return out_->write_bytes(b, offset, length);
}

VoidResult RateLimitedIndexOutput::write_int(int32_t i) {
    bytes_since_last_pause_ += static_cast<int64_t>(BitUtil::INT_BYTES);
    if (auto r = check_rate(); !r) {
        return r;
    }
    return out_->write_int(i);
}

VoidResult RateLimitedIndexOutput::write_short(int16_t i) {
    bytes_since_last_pause_ += static_cast<int64_t>(BitUtil::SHORT_BYTES);
    if (auto r = check_rate(); !r) {
        return r;
    }
    return out_->write_short(i);
}

VoidResult RateLimitedIndexOutput::write_long(int64_t i) {
    bytes_since_last_pause_ += static_cast<int64_t>(BitUtil::LONG_BYTES);
    if (auto r = check_rate(); !r) {
        return r;
    }
    return out_->write_long(i);
}

Result<size_t> RateLimitedIndexOutput::get_file_pointer() const { return out_->get_file_pointer(); }

uint64_t RateLimitedIndexOutput::get_checksum() { return out_->get_checksum(); }

std::string_view RateLimitedIndexOutput::get_name() const { return out_->get_name(); }

VoidResult RateLimitedIndexOutput::close() { return out_->close(); }

Result<size_t> RateLimitedIndexOutput::align_file_pointer(size_t alignment_bytes) {
    return out_->align_file_pointer(alignment_bytes);
}

std::string RateLimitedIndexOutput::to_string() const {
    return "RateLimitedIndexOutput(" + out_->to_string() + ")";
}

} // namespace lucene
