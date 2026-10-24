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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.lock_validating_directory_wrapper;

import lucene_core;

using namespace lucene;

namespace {

/// A lock the test can invalidate, standing in for one taken by another process.
class MutableLock final : public Lock {
public:
    [[nodiscard]] VoidResult ensure_valid() const override {
        if (!valid_) {
            return std::unexpected(LuceneError::illegal_state("lock is no longer valid"));
        }
        return {};
    }

    [[nodiscard]] VoidResult close() override {
        ++closes;
        return {};
    }

    [[nodiscard]] std::string to_string() const override { return "MutableLock"; }

    void invalidate() noexcept { valid_ = false; }

    bool valid_{true};
    int closes{0};
};

/// Writes `name` with `count` bytes and closes it, so the file becomes visible.
VoidResult write_file(Directory &directory, std::string_view name, size_t count = 4, uint8_t value = 0x2A) {
    auto output = directory.create_output(name, io_context_default());
    if (!output.has_value()) {
        return std::unexpected(std::move(output.error()));
    }
    const std::vector<uint8_t> bytes(count, value);
    if (auto written = output.value()->write_bytes(bytes); !written.has_value()) {
        return written;
    }
    return output.value()->close();
}

bool contains(const std::vector<std::string> &names, std::string_view name) {
    for (const auto &candidate : names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(LockValidatingDirectoryWrapperTest, AllowsWritesWhileTheLockIsValid) {
    ByteBuffersDirectory delegate;
    MutableLock lock;
    LockValidatingDirectoryWrapper directory(delegate, lock);

    ASSERT_TRUE(write_file(directory, "_0.si").has_value());

    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(names.value(), (std::vector<std::string>{"_0.si"}));

    auto length = directory.file_length("_0.si");
    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), 4U);

    ASSERT_TRUE(directory.sync(std::vector<std::string>{"_0.si"}).has_value());
    ASSERT_TRUE(directory.sync_metadata().has_value());

    ASSERT_TRUE(directory.rename("_0.si", "_0.si.bak").has_value());
    auto renamed = directory.list_all();
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(renamed.value(), (std::vector<std::string>{"_0.si.bak"}));

    ASSERT_TRUE(directory.delete_file("_0.si.bak").has_value());
    auto empty = directory.list_all();
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty.value().empty());

    EXPECT_EQ(directory.to_string(), "LockValidatingDirectoryWrapper(ByteBuffersDirectory)");
    EXPECT_FALSE(directory.is_fs_directory());
    EXPECT_EQ(lock.closes, 0) << "the wrapper does not own the lock";
}

TEST(LockValidatingDirectoryWrapperTest, CopiesFilesWhileTheLockIsValid) {
    ByteBuffersDirectory delegate;
    ByteBuffersDirectory source;
    MutableLock lock;
    LockValidatingDirectoryWrapper directory(delegate, lock);

    ASSERT_TRUE(write_file(source, "_0.fdt", 6, 0x11).has_value());
    ASSERT_TRUE(directory.copy_from(source, "_0.fdt", "_1.fdt", io_context_default()).has_value());

    auto length = delegate.file_length("_1.fdt");
    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), 6U);
}

TEST(LockValidatingDirectoryWrapperTest, RejectsDestructiveOperationsWhenTheLockIsLost) {
    ByteBuffersDirectory delegate;
    MutableLock lock;
    LockValidatingDirectoryWrapper directory(delegate, lock);

    ASSERT_TRUE(write_file(directory, "_0.si", 4, 0x33).has_value());
    lock.invalidate();

    auto output = directory.create_output("_1.si", io_context_default());
    ASSERT_FALSE(output.has_value());
    EXPECT_EQ(output.error().kind(), ErrorKind::kIllegalState);
    EXPECT_FALSE(contains(delegate.list_all().value(), "_1.si"));

    auto deleted = directory.delete_file("_0.si");
    ASSERT_FALSE(deleted.has_value());
    EXPECT_EQ(deleted.error().kind(), ErrorKind::kIllegalState);
    EXPECT_TRUE(contains(delegate.list_all().value(), "_0.si")) << "the file must survive a lost lock";

    auto renamed = directory.rename("_0.si", "_0.si.bak");
    ASSERT_FALSE(renamed.has_value());
    EXPECT_EQ(renamed.error().kind(), ErrorKind::kIllegalState);
    EXPECT_TRUE(contains(delegate.list_all().value(), "_0.si"));

    auto synced = directory.sync(std::vector<std::string>{"_0.si"});
    ASSERT_FALSE(synced.has_value());
    EXPECT_EQ(synced.error().kind(), ErrorKind::kIllegalState);

    auto metadata = directory.sync_metadata();
    ASSERT_FALSE(metadata.has_value());
    EXPECT_EQ(metadata.error().kind(), ErrorKind::kIllegalState);
}

TEST(LockValidatingDirectoryWrapperTest, RejectsCopiesWhenTheLockIsLost) {
    ByteBuffersDirectory delegate;
    ByteBuffersDirectory source;
    MutableLock lock;
    LockValidatingDirectoryWrapper directory(delegate, lock);

    ASSERT_TRUE(write_file(source, "_0.fdt", 6, 0x11).has_value());
    lock.invalidate();

    auto copied = directory.copy_from(source, "_0.fdt", "_1.fdt", io_context_default());
    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(copied.error().kind(), ErrorKind::kIllegalState);

    auto names = delegate.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_TRUE(names.value().empty());
}

TEST(LockValidatingDirectoryWrapperTest, ReadsAndTemporaryOutputsAreNotChecked) {
    ByteBuffersDirectory delegate;
    MutableLock lock;
    LockValidatingDirectoryWrapper directory(delegate, lock);

    ASSERT_TRUE(write_file(directory, "_0.si", 3, 0x44).has_value());
    lock.invalidate();

    // Reads see a consistent snapshot, so a lost lock does not matter.
    auto names = directory.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(names.value(), (std::vector<std::string>{"_0.si"}));

    auto length = directory.file_length("_0.si");
    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), 3U);

    auto input = directory.open_input("_0.si", io_context_default());
    ASSERT_TRUE(input.has_value()) << (input ? "" : input.error().to_string());
    std::vector<uint8_t> bytes(3);
    ASSERT_TRUE(input.value()->read_bytes(bytes).has_value());
    EXPECT_EQ(bytes, (std::vector<uint8_t>{0x44, 0x44, 0x44}));
    ASSERT_TRUE(input.value()->close().has_value());

    // Temporary outputs are written once and never overwrite anything, so the
    // reference implementation does not check the lock for them either.
    auto temp = directory.create_temp_output("seg", ".tmp", io_context_default());
    ASSERT_TRUE(temp.has_value()) << (temp ? "" : temp.error().to_string());
    ASSERT_TRUE(temp.value().output->close().has_value());

    auto locks = directory.obtain_lock("write.lock");
    ASSERT_TRUE(locks.has_value()) << (locks ? "" : locks.error().to_string());
    ASSERT_TRUE(locks.value()->close().has_value());
}
