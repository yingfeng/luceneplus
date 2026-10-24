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

export module lucene_core:rate_limiter;

import :types;
import std;

import :error;

namespace lucene {

/// Limits the I/O rate of the codecs.
///
/// Port of the `RateLimiter` trait (`core/store/rate_limiter.rs`).
///
/// A limiter is shared between the streams that belong together (all the streams
/// of one merge, for instance): each of them reports the bytes it moved to
/// [`pause`] once it has moved at least [`min_pause_check_bytes`], and the limiter
/// sleeps for as long as the rate requires.
export class RateLimiter {
public:
    virtual ~RateLimiter() = default;

    /// Updates the limit; `0` means "no limit".
    [[nodiscard]] virtual VoidResult set_mb_per_sec(double mb_per_sec) = 0;

    /// The current limit, in MB/s.
    [[nodiscard]] virtual double mb_per_sec() const = 0;

    /// Sleeps if `bytes` more would exceed the rate, and returns how long it
    /// paused in nanoseconds.
    [[nodiscard]] virtual Result<int64_t> pause(int64_t bytes) = 0;

    /// How many bytes a caller should accumulate before asking [`pause`].
    [[nodiscard]] virtual int64_t min_pause_check_bytes() const = 0;
};

/// The limiter the codecs use: a token-bucket-free limiter that paces the
/// instantaneous rate.
///
/// Port of `SimpleRateLimiter` (`core/store/rate_limiter.rs`).
///
/// The implementation is the reference one: every [`pause`] moves a shared
/// "next allowed time" forward by the time `bytes` would take, and sleeps until
/// then.  Pacing the *instantaneous* rate (rather than the average over all
/// history) is what keeps a long merge from banking credit.
export class SimpleRateLimiter final : public RateLimiter {
public:
    /// A limiter of `mb_per_sec` MB/s; `0` means "no limit".
    explicit SimpleRateLimiter(double mb_per_sec);

    [[nodiscard]] VoidResult set_mb_per_sec(double mb_per_sec) override;
    [[nodiscard]] double mb_per_sec() const override;
    [[nodiscard]] Result<int64_t> pause(int64_t bytes) override;
    [[nodiscard]] int64_t min_pause_check_bytes() const override;

private:
    /// Nanoseconds since the limiter was created, plus one (so the clock never
    /// reads zero).
    [[nodiscard]] int64_t clock_ns() const;

    mutable std::mutex mutex_;
    double mb_per_sec_{0.0};
    int64_t min_pause_check_bytes_{0};
    std::chrono::steady_clock::time_point origin_;
    int64_t last_ns_{1};
};

} // namespace lucene
