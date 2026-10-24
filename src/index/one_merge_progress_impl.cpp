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

module lucene_core:one_merge_progress.impl;

import :one_merge_progress;

import std;

import :debug;
import :types;

namespace lucene {

void OneMergeProgress::abort() {
    aborted_.store(true, std::memory_order_seq_cst);
    // Wakes a paused merge thread so it notices the abort immediately.
    wakeup();
}

void OneMergeProgress::pause_nanos(uint64_t pause_nanos, PauseReason reason,
                                   const std::function<bool()> &condition) {
    {
        std::lock_guard<std::mutex> owner_lock(owner_mutex_);
        debug_check(!owner_.has_value() || owner_.value() == std::this_thread::get_id());
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::nanoseconds(pause_nanos);
    {
        std::unique_lock<std::mutex> lock(pause_mutex_);
        // The condition has to be re-checked after every wakeup: a spurious
        // wakeup must not end the pause.
        while (!aborted_.load(std::memory_order_seq_cst) && condition()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }
            pausing_.wait_for(lock, deadline - now);
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start);
    add_pause_time(reason, static_cast<uint64_t>(elapsed.count()));
}

void OneMergeProgress::wakeup() {
    std::lock_guard<std::mutex> lock(pause_mutex_);
    pausing_.notify_all();
}

uint64_t OneMergeProgress::pause_time(PauseReason reason) const noexcept {
    switch (reason) {
        case PauseReason::kStopped: return stopped_ns_.load(std::memory_order_seq_cst);
        case PauseReason::kPaused: return paused_ns_.load(std::memory_order_seq_cst);
        case PauseReason::kOther: return other_ns_.load(std::memory_order_seq_cst);
    }
    return 0;
}

void OneMergeProgress::set_merge_thread() {
    std::lock_guard<std::mutex> lock(owner_mutex_);
    owner_ = std::this_thread::get_id();
}

void OneMergeProgress::clear_merge_thread() {
    std::lock_guard<std::mutex> lock(owner_mutex_);
    owner_.reset();
}

void OneMergeProgress::add_pause_time(PauseReason reason, uint64_t nanos) {
    switch (reason) {
        case PauseReason::kStopped: stopped_ns_.fetch_add(nanos, std::memory_order_seq_cst); break;
        case PauseReason::kPaused: paused_ns_.fetch_add(nanos, std::memory_order_seq_cst); break;
        case PauseReason::kOther: other_ns_.fetch_add(nanos, std::memory_order_seq_cst); break;
    }
}

} // namespace lucene
