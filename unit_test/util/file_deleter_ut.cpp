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

module lucene_core:ut.file_deleter;

import lucene_core;

using namespace lucene;

namespace {

/// Wraps a directory and records the order in which files are deleted.
class RecordingDirectory final : public Directory {
public:
    using Directory::create_output;
    using Directory::open_input;

    explicit RecordingDirectory(Directory &delegate) : delegate_(delegate) {}

    [[nodiscard]] Result<std::vector<std::string>> list_all() const override { return delegate_.list_all(); }
    [[nodiscard]] VoidResult delete_file(std::string_view name) override {
        deleted.emplace_back(name);
        return delegate_.delete_file(name);
    }
    [[nodiscard]] Result<size_t> file_length(std::string_view name) const override {
        return delegate_.file_length(name);
    }
    [[nodiscard]] Result<std::unique_ptr<IndexOutput>> create_output(std::string_view name,
                                                                    const IOContext &context) override {
        return delegate_.create_output(name, context);
    }
    [[nodiscard]] Result<CreatedOutput> create_temp_output(std::string_view prefix, std::string_view suffix,
                                                          const IOContext &context) override {
        return delegate_.create_temp_output(prefix, suffix, context);
    }
    [[nodiscard]] VoidResult sync(std::span<const std::string> names) override { return delegate_.sync(names); }
    [[nodiscard]] VoidResult rename(std::string_view source, std::string_view dest) override {
        return delegate_.rename(source, dest);
    }
    [[nodiscard]] Result<std::unique_ptr<IndexInput>> open_input(std::string_view name,
                                                                const IOContext &context) const override {
        return delegate_.open_input(name, context);
    }
    [[nodiscard]] Result<std::unique_ptr<Lock>> obtain_lock(std::string_view name) override {
        return delegate_.obtain_lock(name);
    }
    [[nodiscard]] std::string to_string() const override { return "RecordingDirectory"; }

    std::vector<std::string> deleted;

private:
    Directory &delegate_;
};

/// Records the messages a [`FileDeleter`] emits.
class RecordingListener final : public FileDeleterListener {
public:
    [[nodiscard]] VoidResult accept(FileDeleterMsg type, std::string_view message) override {
        messages.emplace_back(type, std::string(message));
        return {};
    }

    std::vector<std::pair<FileDeleterMsg, std::string>> messages;
};

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

bool contains(const std::vector<std::string> &names, std::string_view name) {
    for (const std::string &candidate : names) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(FileDeleterTest, DeletesAFileWhenTheLastReferenceGoes) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_file(directory, "_0.si").has_value());
    FileDeleter deleter(directory);

    ASSERT_TRUE(deleter.inc_ref("_0.si").has_value());
    EXPECT_EQ(deleter.ref_count("_0.si"), 1U);
    EXPECT_TRUE(deleter.exists("_0.si"));

    ASSERT_TRUE(deleter.inc_ref("_0.si").has_value());
    EXPECT_EQ(deleter.ref_count("_0.si"), 2U);

    ASSERT_TRUE(deleter.dec_ref(std::vector<std::string>{"_0.si"}).has_value());
    EXPECT_TRUE(contains(directory.list_all().value(), "_0.si")) << "still referenced once";

    ASSERT_TRUE(deleter.dec_ref(std::vector<std::string>{"_0.si"}).has_value());
    EXPECT_FALSE(contains(directory.list_all().value(), "_0.si")) << "the last reference is gone";
    EXPECT_EQ(deleter.ref_count("_0.si"), 0U);
}

TEST(FileDeleterTest, DeletesCommitFilesBeforeTheFilesTheyReference) {
    ByteBuffersDirectory delegate;
    ASSERT_TRUE(write_file(delegate, "_0.si").has_value());
    ASSERT_TRUE(write_file(delegate, "_0.fdt").has_value());
    ASSERT_TRUE(write_file(delegate, "segments_3").has_value());
    RecordingDirectory directory(delegate);
    FileDeleter deleter(directory);

    ASSERT_TRUE(deleter.delete_files(std::vector<std::string>{"_0.fdt", "_0.si", "segments_3"}).has_value());

    ASSERT_EQ(directory.deleted.size(), 3U);
    EXPECT_EQ(directory.deleted[0], "segments_3") << "commit files go first";
    EXPECT_FALSE(contains(delegate.list_all().value(), "segments_3"));
}

TEST(FileDeleterTest, KnowsWhichFilesAreStillReferenced) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_file(directory, "a").has_value());
    ASSERT_TRUE(write_file(directory, "b").has_value());
    FileDeleter deleter(directory);

    deleter.init_ref_count("a");
    deleter.init_ref_count("b");
    ASSERT_TRUE(deleter.inc_ref("a").has_value());

    EXPECT_FALSE(deleter.exists("b"));
    EXPECT_EQ(deleter.unrefed_files(), (std::vector<std::string>{"b"}));

    const std::vector<std::string> all = deleter.all_files();
    EXPECT_EQ(all.size(), 2U);

    // Deleting "only if unreferenced" leaves "a" alone.
    ASSERT_TRUE(deleter.delete_files_if_no_ref(std::vector<std::string>{"a", "b"}).has_value());
    EXPECT_TRUE(contains(directory.list_all().value(), "a"));
    EXPECT_FALSE(contains(directory.list_all().value(), "b"));
}

TEST(FileDeleterTest, ForceDeleteIgnoresTheCount) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_file(directory, "_0.si").has_value());
    FileDeleter deleter(directory);
    ASSERT_TRUE(deleter.inc_ref("_0.si").has_value());

    ASSERT_TRUE(deleter.force_delete("_0.si").has_value());
    EXPECT_FALSE(contains(directory.list_all().value(), "_0.si"));
    EXPECT_EQ(deleter.ref_count("_0.si"), 0U);
}

TEST(FileDeleterTest, ReportsWhatItDeletes) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_file(directory, "_0.si").has_value());
    auto listener = std::make_shared<RecordingListener>();
    FileDeleter deleter(directory, listener);

    ASSERT_TRUE(deleter.inc_ref("_0.si").has_value());
    ASSERT_TRUE(deleter.dec_ref(std::vector<std::string>{"_0.si"}).has_value());

    ASSERT_FALSE(listener->messages.empty());
    bool saw_ref = false;
    bool saw_file = false;
    for (const auto &[type, message] : listener->messages) {
        saw_ref = saw_ref || type == FileDeleterMsg::kRef;
        saw_file = saw_file || type == FileDeleterMsg::kFile;
    }
    EXPECT_TRUE(saw_ref);
    EXPECT_TRUE(saw_file);
}

TEST(FileDeleterTest, DeleteFileIfNoRefSkipsAReferencedFile) {
    ByteBuffersDirectory directory;
    ASSERT_TRUE(write_file(directory, "_0.si").has_value());
    FileDeleter deleter(directory);
    ASSERT_TRUE(deleter.inc_ref("_0.si").has_value());

    ASSERT_TRUE(deleter.delete_file_if_no_ref("_0.si").has_value());
    EXPECT_TRUE(contains(directory.list_all().value(), "_0.si"));
}
