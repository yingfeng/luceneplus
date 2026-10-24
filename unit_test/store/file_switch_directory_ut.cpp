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

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.file_switch_directory;

import lucene_core;

using namespace lucene;

namespace {

/// Writes `name` with one byte and closes it, so the directory has the file.
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

TEST(FileSwitchDirectoryTest, ComputesTheExtension) {
    EXPECT_EQ(get_extension("_0.fdt"), "fdt");
    EXPECT_EQ(get_extension("segments_1"), "");
    EXPECT_EQ(get_extension("_0.si"), "si");

    // A temporary file reports the extension it came from.
    EXPECT_EQ(get_extension("_0.fdt.tmp"), "fdt");
    EXPECT_EQ(get_extension("_0.cfs.tmp"), "cfs");
    // Without a previous extension there is nothing to report but `tmp`.
    EXPECT_EQ(get_extension("tmp"), "");
    EXPECT_EQ(get_extension("noext.tmp"), "tmp");
}

TEST(FileSwitchDirectoryTest, RoutesTheFilesByExtension) {
    auto primary = std::make_shared<ByteBuffersDirectory>();
    auto secondary = std::make_shared<ByteBuffersDirectory>();
    auto created =
            FileSwitchDirectory::create({"cfs", "si"}, std::static_pointer_cast<Directory>(primary),
                                        std::static_pointer_cast<Directory>(secondary));
    ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
    FileSwitchDirectory &directory = *created.value();

    ASSERT_TRUE(write_file(directory, "_0.cfs").has_value());
    ASSERT_TRUE(write_file(directory, "_0.fdt").has_value());
    ASSERT_TRUE(write_file(directory, "_0.si").has_value());

    // The primary directory only has the files it owns, and vice versa.
    auto primary_files = primary->list_all();
    ASSERT_TRUE(primary_files.has_value());
    std::ranges::sort(primary_files.value());
    EXPECT_EQ(primary_files.value(), (std::vector<std::string>{"_0.cfs", "_0.si"}));

    auto secondary_files = secondary->list_all();
    ASSERT_TRUE(secondary_files.has_value());
    EXPECT_EQ(secondary_files.value(), (std::vector<std::string>{"_0.fdt"}));

    // Listing the switch merges both.
    auto all_files = directory.list_all();
    ASSERT_TRUE(all_files.has_value()) << (all_files ? "" : all_files.error().to_string());
    std::ranges::sort(all_files.value());
    EXPECT_EQ(all_files.value(), (std::vector<std::string>{"_0.cfs", "_0.fdt", "_0.si"}));

    // Reading and deleting go through the same routing.
    ASSERT_TRUE(directory.file_length("_0.fdt").has_value());
    ASSERT_TRUE(directory.delete_file("_0.fdt").has_value());
    EXPECT_TRUE(secondary->list_all().value().empty());

    EXPECT_NE(directory.to_string().find("FileSwitchDirectory(primary="), std::string::npos);
}

TEST(FileSwitchDirectoryTest, RejectsTheReservedExtension) {
    auto primary = std::make_shared<ByteBuffersDirectory>();
    auto secondary = std::make_shared<ByteBuffersDirectory>();
    auto created = FileSwitchDirectory::create({"tmp"}, std::static_pointer_cast<Directory>(primary),
                                               std::static_pointer_cast<Directory>(secondary));
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().kind(), ErrorKind::kIllegalArgument);

    auto null_primary = FileSwitchDirectory::create({"cfs"}, nullptr,
                                                    std::static_pointer_cast<Directory>(secondary));
    ASSERT_FALSE(null_primary.has_value());
    EXPECT_EQ(null_primary.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FileSwitchDirectoryTest, SendsTemporaryFilesToTheSecondaryDirectory) {
    auto primary = std::make_shared<ByteBuffersDirectory>();
    auto secondary = std::make_shared<ByteBuffersDirectory>();
    auto created =
            FileSwitchDirectory::create({"cfs"}, std::static_pointer_cast<Directory>(primary),
                                        std::static_pointer_cast<Directory>(secondary));
    ASSERT_TRUE(created.has_value());

    auto temp = created.value()->create_temp_output("seg", ".tmp", io_context_default());
    ASSERT_TRUE(temp.has_value()) << (temp ? "" : temp.error().to_string());
    ASSERT_TRUE(temp.value().output->close().has_value());

    EXPECT_TRUE(primary->list_all().value().empty());
    EXPECT_EQ(secondary->list_all().value().size(), 1U);
}

TEST(FileSwitchDirectoryTest, RefusesToRenameAcrossTheTwoDirectories) {
    auto primary = std::make_shared<ByteBuffersDirectory>();
    auto secondary = std::make_shared<ByteBuffersDirectory>();
    auto created =
            FileSwitchDirectory::create({"cfs"}, std::static_pointer_cast<Directory>(primary),
                                        std::static_pointer_cast<Directory>(secondary));
    ASSERT_TRUE(created.has_value());
    FileSwitchDirectory &directory = *created.value();

    ASSERT_TRUE(write_file(directory, "_0.cfs").has_value());
    ASSERT_TRUE(directory.rename("_0.cfs", "_1.cfs").has_value());
    EXPECT_EQ(primary->list_all().value(), (std::vector<std::string>{"_1.cfs"}));

    auto across = directory.rename("_1.cfs", "_1.fdt");
    ASSERT_FALSE(across.has_value());
    EXPECT_EQ(across.error().kind(), ErrorKind::kIllegalArgument);
}
