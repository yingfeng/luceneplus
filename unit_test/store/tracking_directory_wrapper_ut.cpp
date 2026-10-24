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
#include <set>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.tracking_directory_wrapper;

import lucene_core;

using namespace lucene;

namespace {

/// Writes `name` with one byte, so the delegate has a real file.
///
/// The bytes only reach the directory on `close()`, which is why the output is
/// closed explicitly.
VoidResult write_file(Directory &directory, std::string_view name) {
    auto output = directory.create_output(name, io_context_default());
    if (!output.has_value()) {
        return std::unexpected(std::move(output.error()));
    }
    if (auto written = output.value()->write_byte(0x2A); !written.has_value()) {
        return written;
    }
    return output.value()->close();
}

} // namespace

TEST(TrackingDirectoryWrapperTest, RecordsTheCreatedFiles) {
    ByteBuffersDirectory delegate;
    TrackingDirectoryWrapper directory(delegate);

    ASSERT_TRUE(write_file(directory, "_0.si").has_value());
    ASSERT_TRUE(write_file(directory, "_0.fdt").has_value());

    EXPECT_TRUE(directory.has_created_files());
    EXPECT_EQ(directory.created_files(), (std::set<std::string>{"_0.fdt", "_0.si"}));

    // The delegate really has the files.
    auto names = delegate.list_all();
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(names.value().size(), 2U);

    EXPECT_NE(directory.to_string().find("TrackingDirectoryWrapper("), std::string::npos);
}

TEST(TrackingDirectoryWrapperTest, RecordsTemporaryFiles) {
    ByteBuffersDirectory delegate;
    TrackingDirectoryWrapper directory(delegate);

    auto temp = directory.create_temp_output("seg", ".tmp", io_context_default());
    ASSERT_TRUE(temp.has_value()) << (temp ? "" : temp.error().to_string());
    const std::string name = temp.value().name;

    EXPECT_EQ(directory.created_files(), (std::set<std::string>{name}));
}

TEST(TrackingDirectoryWrapperTest, ForgetsDeletedFiles) {
    ByteBuffersDirectory delegate;
    TrackingDirectoryWrapper directory(delegate);

    ASSERT_TRUE(write_file(directory, "_0.si").has_value());
    ASSERT_TRUE(write_file(directory, "_0.fdt").has_value());

    ASSERT_TRUE(directory.delete_file("_0.fdt").has_value());
    EXPECT_EQ(directory.created_files(), (std::set<std::string>{"_0.si"}));

    // Deleting a name that was never recorded leaves the record alone.
    EXPECT_TRUE(directory.delete_file("nope").has_value());
    EXPECT_EQ(directory.created_files(), (std::set<std::string>{"_0.si"}));
}

TEST(TrackingDirectoryWrapperTest, TakesAndClearsTheRecordedFiles) {
    ByteBuffersDirectory delegate;
    TrackingDirectoryWrapper directory(delegate);

    ASSERT_TRUE(write_file(directory, "_0.si").has_value());
    ASSERT_TRUE(write_file(directory, "_0.fdt").has_value());

    const std::set<std::string> taken = directory.take_created_files();
    EXPECT_EQ(taken, (std::set<std::string>{"_0.fdt", "_0.si"}));
    EXPECT_FALSE(directory.has_created_files());

    ASSERT_TRUE(write_file(directory, "_1.si").has_value());
    directory.clear_created_files();
    EXPECT_FALSE(directory.has_created_files());
}
