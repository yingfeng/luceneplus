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

module lucene_core:sort_field.impl;

import :sort_field;

import std;

import :error;
import :index_sorter;
import :types;

namespace lucene {

namespace {

/// The `<type: "field">` part of a sort field's text.
std::string_view type_name(SortFieldType type) {
    switch (type) {
        case SortFieldType::kScore:
            return "score";
        case SortFieldType::kDoc:
            return "doc";
        case SortFieldType::kString:
            return "string";
        case SortFieldType::kInt:
            return "int";
        case SortFieldType::kFloat:
            return "float";
        case SortFieldType::kLong:
            return "long";
        case SortFieldType::kDouble:
            return "double";
        case SortFieldType::kSortedNumeric:
            return "sorted_numeric";
        case SortFieldType::kSortedNumericDouble:
            return "sorted_numeric_double";
        case SortFieldType::kSortedSet:
            return "sorted_set";
        case SortFieldType::kCustom:
            return "custom";
    }
    return "unknown";
}

} // namespace

SortField::SortField(std::optional<std::string> field, SortFieldType type, bool reverse,
                     SortFieldSelector selector)
        : field_(std::move(field)), type_(type), reverse_(reverse), selector_(selector) {}

Result<SortField> SortField::create(std::optional<std::string> field, SortFieldType type) {
    return with_reverse(std::move(field), type, false);
}

Result<SortField> SortField::with_reverse(std::optional<std::string> field, SortFieldType type, bool reverse) {
    if (!field.has_value() && type != SortFieldType::kScore && type != SortFieldType::kDoc) {
        return err(LuceneError::illegal_argument(std::format("field must not be null for {}", type_name(type))));
    }
    return SortField(std::move(field), type, reverse, SortFieldSelector::kMin);
}

Result<SortField> SortField::with_selector(std::optional<std::string> field, SortFieldType type,
                                           SortFieldSelector selector) {
    return with_reverse_selector(std::move(field), type, selector, false);
}

Result<SortField> SortField::with_reverse_selector(std::optional<std::string> field, SortFieldType type,
                                                   SortFieldSelector selector, bool reverse) {
    if (type != SortFieldType::kSortedNumeric && type != SortFieldType::kSortedNumericDouble &&
        type != SortFieldType::kSortedSet) {
        return err(LuceneError::illegal_argument(std::format("{} is not a multi-valued sort type", type_name(type))));
    }
    if (!field.has_value()) {
        return err(LuceneError::illegal_argument(std::format("field must not be null for {}", type_name(type))));
    }
    return SortField(std::move(field), type, reverse, selector);
}

Result<SortField> SortField::score() { return with_reverse(std::nullopt, SortFieldType::kScore, false); }

Result<SortField> SortField::doc() { return with_reverse(std::nullopt, SortFieldType::kDoc, false); }

Result<SortField> SortField::with_custom(std::optional<std::string> field, std::string display,
                                        CustomComparatorFactory factory) {
    if (!factory) {
        return err(LuceneError::illegal_argument("a custom sort field needs a comparator factory"));
    }
    SortField sort_field(std::move(field), SortFieldType::kCustom, false, SortFieldSelector::kMin);
    sort_field.custom_display_ = std::move(display);
    sort_field.custom_factory_ = std::move(factory);
    return sort_field;
}

Result<std::unique_ptr<IndexSorter>> SortField::index_sorter() const {
    if (type_ == SortFieldType::kCustom) {
        return err(LuceneError::illegal_argument(
                "a custom sort field cannot drive an index sort: its values are computed per search"));
    }
    if (needs_scores()) {
        return err(LuceneError::illegal_argument(
                "Cannot sort an index with a Sort that refers to the relevance score"));
    }
    if (!field_.has_value()) {
        return err(LuceneError::illegal_argument("Cannot use the document id as an index sort"));
    }
    if (is_multi_valued()) {
        return err(LuceneError::unsupported_operation(
                "multi-valued sort fields cannot be used as an index sort yet"));
    }

    std::unique_ptr<IndexSorter> sorter;
    switch (type_) {
        case SortFieldType::kCustom:
            // A custom sort field computes its values per search.
            return err(LuceneError::illegal_argument("a custom sort field cannot drive an index sort"));
        case SortFieldType::kInt: {
            auto created = IntIndexSorter::create(field_.value(), std::nullopt, reverse_);
            if (!created.has_value()) {
                return std::unexpected(std::move(created.error()));
            }
            sorter.reset(new IntIndexSorter(std::move(created.value())));
            break;
        }
        case SortFieldType::kLong: {
            auto created = LongIndexSorter::create(field_.value(), std::nullopt, reverse_);
            if (!created.has_value()) {
                return std::unexpected(std::move(created.error()));
            }
            sorter.reset(new LongIndexSorter(std::move(created.value())));
            break;
        }
        case SortFieldType::kFloat: {
            auto created = FloatIndexSorter::create(field_.value(), std::nullopt, reverse_);
            if (!created.has_value()) {
                return std::unexpected(std::move(created.error()));
            }
            sorter.reset(new FloatIndexSorter(std::move(created.value())));
            break;
        }
        case SortFieldType::kDouble: {
            auto created = DoubleIndexSorter::create(field_.value(), std::nullopt, reverse_);
            if (!created.has_value()) {
                return std::unexpected(std::move(created.error()));
            }
            sorter.reset(new DoubleIndexSorter(std::move(created.value())));
            break;
        }
        case SortFieldType::kString: {
            auto created = StringIndexSorter::create(field_.value(), false, reverse_);
            if (!created.has_value()) {
                return std::unexpected(std::move(created.error()));
            }
            sorter.reset(new StringIndexSorter(std::move(created.value())));
            break;
        }
        case SortFieldType::kScore:
        case SortFieldType::kDoc:
        case SortFieldType::kSortedNumeric:
        case SortFieldType::kSortedNumericDouble:
        case SortFieldType::kSortedSet:
            return err(LuceneError::unsupported_operation("this sort field cannot be used as an index sort"));
    }
    return sorter;
}

Result<std::string> SortField::to_string() const {
    if (type_ == SortFieldType::kCustom) {
        return custom_display_;
    }
    std::string buffer;
    if (type_ == SortFieldType::kScore) {
        buffer = "<score>";
    } else if (type_ == SortFieldType::kDoc) {
        buffer = "<doc>";
    } else {
        buffer = std::format("<{}: \"{}\">", type_name(type_), field_.value_or(""));
        if (is_multi_valued()) {
            buffer += selector_ == SortFieldSelector::kMax ? "(max)" : "(min)";
        }
    }
    if (reverse_) {
        buffer += '!';
    }
    return buffer;
}

size_t SortField::hash() const noexcept {
    size_t result = std::hash<std::string>{}(field_.value_or(""));
    result = result * 31 + static_cast<size_t>(type_);
    result = result * 31 + (reverse_ ? 1 : 0);
    return result * 31 + static_cast<size_t>(selector_);
}

Sort::Sort(std::vector<SortField> fields) : fields_(std::move(fields)) {}

Result<Sort> Sort::relevance() {
    auto field = SortField::score();
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    return Sort({std::move(field.value())});
}

Result<Sort> Sort::index_order() {
    auto field = SortField::doc();
    if (!field.has_value()) {
        return std::unexpected(std::move(field.error()));
    }
    return Sort({std::move(field.value())});
}

Result<Sort> Sort::with_fields(std::vector<SortField> fields) {
    if (fields.empty()) {
        return err(LuceneError::illegal_argument("There must be at least 1 sort field"));
    }
    return Sort(std::move(fields));
}

bool Sort::needs_scores() const noexcept {
    for (const SortField &field : fields_) {
        if (field.needs_scores()) {
            return true;
        }
    }
    return false;
}

std::string Sort::to_string() const {
    std::string result;
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (i > 0) {
            result += ',';
        }
        auto field = fields_[i].to_string();
        result += field.has_value() ? field.value() : std::string("<invalid>");
    }
    return result;
}

size_t Sort::hash() const noexcept {
    size_t result = fields_.size();
    for (const SortField &field : fields_) {
        result = result * 31 + field.hash();
    }
    return result;
}

Result<std::optional<IndexSortDocMapImpl>> Sort::sort_documents(LeafReader &leaf_reader, int32_t max_doc) const {
    if (max_doc < 0) {
        return err(LuceneError::illegal_argument("max_doc must not be negative"));
    }
    std::vector<std::unique_ptr<DocComparator>> comparators;
    comparators.reserve(fields_.size());
    for (const SortField &field : fields_) {
        auto sorter = field.index_sorter();
        if (!sorter.has_value()) {
            auto text = field.to_string();
            return err(LuceneError::illegal_argument(
                    "cannot sort an index by " + (text.has_value() ? text.value() : std::string("<sort field>")) +
                    ": " + sorter.error().to_string()));
        }
        auto comparator = sorter.value()->doc_comparator(leaf_reader, max_doc);
        if (!comparator.has_value()) {
            return std::unexpected(std::move(comparator.error()));
        }
        comparators.push_back(std::move(comparator.value()));
    }

    if (comparators.size() == 1) {
        return sort_documents_with_comparator(*comparators[0], max_doc);
    }
    auto chained = ChainedDocComparator::create(std::move(comparators));
    if (!chained.has_value()) {
        return std::unexpected(std::move(chained.error()));
    }
    return sort_documents_with_comparator(*chained.value(), max_doc);
}

} // namespace lucene
