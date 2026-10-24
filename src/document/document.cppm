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

export module lucene_core:document;

import :types;
import :error;
import :number;

import std;

import :indexable_field;

namespace lucene {

/// A set of fields, indexed by name.  Port of `document/document.rs`.
///
/// Field order is preserved (the stored-fields format writes in insertion
/// order), while name lookups use an index for O(1) access.
export class Document {
public:
    Document() = default;
    Document(Document &&) = default;
    Document &operator=(Document &&) = default;

    /// Appends a field.
    void add(std::unique_ptr<IndexableField> field);

    /// Removes the first field called `name`.
    void remove_field(std::string_view name);
    /// Removes every field called `name`.
    void remove_fields(std::string_view name);

    /// The first field called `name`.
    [[nodiscard]] const IndexableField *get_field(std::string_view name) const;
    /// Every field called `name`, in insertion order.
    [[nodiscard]] std::vector<const IndexableField *> get_fields_with_name(std::string_view name) const;
    /// All fields, in insertion order.
    [[nodiscard]] std::vector<const IndexableField *> get_fields() const;

    /// Moves every field out, leaving the document empty.
    ///
    /// Used when documents are concatenated into one (`ParallelStoredFields`).
    [[nodiscard]] std::vector<std::unique_ptr<IndexableField>> take_fields();
    [[nodiscard]] size_t field_count() const noexcept { return fields_.size(); }
    [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }

    /// The string value of the first field called `name`.
    [[nodiscard]] Result<std::optional<std::string>> get(std::string_view name) const;
    /// Every string value of the fields called `name`.
    [[nodiscard]] Result<std::vector<std::string>> get_values(std::string_view name) const;
    /// Every binary value of the fields called `name`.
    [[nodiscard]] Result<std::vector<std::vector<uint8_t>>> get_binary_values(std::string_view name) const;
    /// The first binary value of the fields called `name`.
    [[nodiscard]] Result<std::optional<std::vector<uint8_t>>> get_binary_value(std::string_view name) const;

    void clear();

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<std::unique_ptr<IndexableField>> fields_;
    std::multimap<std::string, size_t, std::less<>> index_;
};

} // namespace lucene
