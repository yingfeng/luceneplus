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

module lucene_core:document.impl;

import :document;
import :types;
import :error;
import :indexable_field;

import std;

namespace lucene {

void Document::add(std::unique_ptr<IndexableField> field) {
    const size_t position = fields_.size();
    std::string name(field->name());
    index_.emplace(std::move(name), position);
    fields_.push_back(std::move(field));
}

namespace {

void rebuild_index(std::vector<std::unique_ptr<IndexableField>> &fields,
                   std::multimap<std::string, size_t, std::less<>> &index) {
    index.clear();
    for (size_t i = 0; i < fields.size(); ++i) {
        index.emplace(std::string(fields[i]->name()), i);
    }
}

} // namespace

std::vector<std::unique_ptr<IndexableField>> Document::take_fields() {
    index_.clear();
    return std::move(fields_);
}

void Document::remove_field(std::string_view name) {
    auto range = index_.equal_range(name);
    if (range.first == range.second) {
        return;
    }
    const size_t position = range.first->second;
    fields_.erase(fields_.begin() + static_cast<std::ptrdiff_t>(position));
    rebuild_index(fields_, index_);
}

void Document::remove_fields(std::string_view name) {
    auto range = index_.equal_range(name);
    if (range.first == range.second) {
        return;
    }
    std::vector<size_t> positions;
    for (auto it = range.first; it != range.second; ++it) {
        positions.push_back(it->second);
    }
    std::ranges::sort(positions, std::greater<>{});
    for (const size_t position : positions) {
        fields_.erase(fields_.begin() + static_cast<std::ptrdiff_t>(position));
    }
    rebuild_index(fields_, index_);
}

const IndexableField *Document::get_field(std::string_view name) const {
    auto range = index_.equal_range(name);
    if (range.first == range.second) {
        return nullptr;
    }
    return fields_[range.first->second].get();
}

std::vector<const IndexableField *> Document::get_fields_with_name(std::string_view name) const {
    std::vector<const IndexableField *> result;
    auto range = index_.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        result.push_back(fields_[it->second].get());
    }
    // `multimap` keeps equivalent keys in insertion order, which is the order the
    // contract asks for, and the positions it holds are ascending.
    //
    // This used to sort the result by pointer value, which was both needlessly
    // expensive and wrong: comparing two unrelated pointers is unspecified, so two
    // fields with the same name came back in whatever order the allocator happened
    // to produce.
    return result;
}

std::vector<const IndexableField *> Document::get_fields() const {
    std::vector<const IndexableField *> result;
    result.reserve(fields_.size());
    for (const auto &field : fields_) {
        result.push_back(field.get());
    }
    return result;
}

Result<std::optional<std::string>> Document::get(std::string_view name) const {
    const IndexableField *field = get_field(name);
    if (field == nullptr) {
        return std::optional<std::string>{};
    }
    if (auto text = field->string_value(); text.has_value()) {
        return std::optional<std::string>{std::string(text.value())};
    }
    return err(LuceneError::illegal_argument(std::format("field {} is not a string field", name)));
}

Result<std::vector<std::string>> Document::get_values(std::string_view name) const {
    std::vector<std::string> values;
    for (const IndexableField *field : get_fields_with_name(name)) {
        if (auto text = field->string_value(); text.has_value()) {
            values.emplace_back(text.value());
        } else {
            return err(LuceneError::illegal_argument(std::format("field {} is not a string field", name)));
        }
    }
    return values;
}

Result<std::vector<std::vector<uint8_t>>> Document::get_binary_values(std::string_view name) const {
    std::vector<std::vector<uint8_t>> values;
    for (const IndexableField *field : get_fields_with_name(name)) {
        if (auto bytes = field->binary_value(); bytes.has_value()) {
            values.push_back(std::move(bytes.value()));
        } else {
            return err(LuceneError::illegal_argument(std::format("field {} is not a binary field", name)));
        }
    }
    return values;
}

Result<std::optional<std::vector<uint8_t>>> Document::get_binary_value(std::string_view name) const {
    const IndexableField *field = get_field(name);
    if (field == nullptr) {
        return std::optional<std::vector<uint8_t>>{};
    }
    if (auto bytes = field->binary_value(); bytes.has_value()) {
        return std::optional<std::vector<uint8_t>>{std::move(bytes.value())};
    }
    return err(LuceneError::illegal_argument(std::format("field {} is not a binary field", name)));
}

void Document::clear() {
    fields_.clear();
    index_.clear();
}

std::string Document::to_string() const {
    std::string result = "Document<";
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (i > 0) {
            result += ' ';
        }
        result += fields_[i]->to_string();
    }
    result.push_back('>');
    return result;
}

} // namespace lucene
