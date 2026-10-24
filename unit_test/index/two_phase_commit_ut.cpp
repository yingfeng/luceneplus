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

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

module lucene_core:ut.two_phase_commit;

import lucene_core;

using namespace lucene;

namespace {

/// A participant that records the calls it receives.
class RecordingParticipant final : public TwoPhaseCommit {
public:
    RecordingParticipant(std::vector<std::string> &log, std::string name, int64_t generation,
                         std::optional<LuceneError> failure = std::nullopt)
            : log_(log), name_(std::move(name)), generation_(generation), failure_(std::move(failure)) {}

    [[nodiscard]] Result<int64_t> prepare_commit() override {
        log_.push_back(name_ + ":prepare");
        if (failure_.has_value()) {
            return std::unexpected(failure_.value());
        }
        return generation_;
    }

    [[nodiscard]] Result<int64_t> commit() override {
        log_.push_back(name_ + ":commit");
        if (failure_.has_value()) {
            return std::unexpected(failure_.value());
        }
        return generation_;
    }

    [[nodiscard]] VoidResult rollback() override {
        log_.push_back(name_ + ":rollback");
        return {};
    }

private:
    std::vector<std::string> &log_;
    std::string name_;
    int64_t generation_;
    std::optional<LuceneError> failure_;
};

} // namespace

TEST(TwoPhaseCommitTest, ParticipantsCanBeCoordinated) {
    std::vector<std::string> log;
    RecordingParticipant first(log, "a", 7);
    RecordingParticipant second(log, "b", 7);

    // The coordinator's contract: prepare everywhere, then commit everywhere.
    const std::vector<int64_t> prepared{first.prepare_commit().value(), second.prepare_commit().value()};
    EXPECT_EQ(prepared, (std::vector<int64_t>{7, 7}));

    const std::vector<int64_t> committed{first.commit().value(), second.commit().value()};
    EXPECT_EQ(committed, (std::vector<int64_t>{7, 7}));
    EXPECT_EQ(log, (std::vector<std::string>{"a:prepare", "b:prepare", "a:commit", "b:commit"}));
}

TEST(TwoPhaseCommitTest, AFailedPrepareRollsTheOthersBack) {
    std::vector<std::string> log;
    RecordingParticipant healthy(log, "healthy", 3);
    RecordingParticipant broken(log, "broken", 3, LuceneError::io("disk full"));

    ASSERT_TRUE(healthy.prepare_commit().has_value());
    auto failed = broken.prepare_commit();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().kind(), ErrorKind::kIo);

    ASSERT_TRUE(healthy.rollback().has_value());
    EXPECT_EQ(log, (std::vector<std::string>{"healthy:prepare", "broken:prepare", "healthy:rollback"}));
}
