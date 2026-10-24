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

module lucene_core:field_comparator.impl;

import :field_comparator;

import std;

import :bytes_ref;
import :error;
import :leaf_reader;
import :leaf_reader_context;
import :numeric_doc_values;
import :scorable;
import :sort_field;
import :sorted_doc_values;
import :types;

namespace lucene {

namespace {

/// `true` when `value` sorts before `other`, without the reverse flag applied.
int32_t natural_compare(const SortValue &left, const SortValue &right) {
    return std::visit(
            [](const auto &first, const auto &second) -> int32_t {
                using First = std::decay_t<decltype(first)>;
                using Second = std::decay_t<decltype(second)>;
                if constexpr (std::is_same_v<First, SortValueMissing> && std::is_same_v<Second, SortValueMissing>) {
                    return 0;
                } else if constexpr (std::is_same_v<First, SortValueMissing>) {
                    // A missing value sorts after every real value.
                    return 1;
                } else if constexpr (std::is_same_v<Second, SortValueMissing>) {
                    return -1;
                } else if constexpr (std::is_same_v<First, std::string> && std::is_same_v<Second, std::string>) {
                    return first < second ? -1 : (first > second ? 1 : 0);
                } else if constexpr (std::is_arithmetic_v<First> && std::is_arithmetic_v<Second>) {
                    const double a = static_cast<double>(first);
                    const double b = static_cast<double>(second);
                    return a < b ? -1 : (a > b ? 1 : 0);
                } else {
                    return 0;
                }
            },
            left, right);
}

} // namespace

// ------------------------------------------------------- ScoreFieldComparator

ScoreFieldComparator::ScoreFieldComparator(int32_t num_hits, bool reverse)
        : values_(static_cast<size_t>(std::max(num_hits, 0))), reverse_(reverse) {}

int32_t ScoreFieldComparator::compare(int32_t slot1, int32_t slot2) const {
    const float first = values_[static_cast<size_t>(slot1)];
    const float second = values_[static_cast<size_t>(slot2)];
    const int32_t result = first < second ? -1 : (first > second ? 1 : 0);
    // Higher scores sort first unless the order is reversed.
    return reverse_ ? result : -result;
}

int32_t ScoreFieldComparator::compare_bottom(int32_t slot) const {
    const float value = values_[static_cast<size_t>(slot)];
    const int32_t result = value < bottom_ ? -1 : (value > bottom_ ? 1 : 0);
    return reverse_ ? result : -result;
}

int32_t ScoreFieldComparator::compare_top(int32_t slot) const {
    if (!top_.has_value()) {
        return 0;
    }
    const float value = values_[static_cast<size_t>(slot)];
    const int32_t result = value < top_.value() ? -1 : (value > top_.value() ? 1 : 0);
    return reverse_ ? result : -result;
}

void ScoreFieldComparator::set_bottom(int32_t slot) { bottom_ = values_[static_cast<size_t>(slot)]; }

void ScoreFieldComparator::set_top_value(const SortValue &value) {
    if (const auto *score = std::get_if<float>(&value); score != nullptr) {
        top_ = *score;
    }
}

Result<SortValue> ScoreFieldComparator::value(int32_t slot) const { return SortValue(values_[static_cast<size_t>(slot)]); }

Result<int32_t> ScoreFieldComparator::compare_values(const SortValue &first, const SortValue &second) const {
    const int32_t result = natural_compare(first, second);
    return reverse_ ? result : -result;
}

Result<std::unique_ptr<LeafFieldComparator>> ScoreFieldComparator::get_leaf_comparator(
        const LeafReaderContext & /*context*/) {
    class Leaf final : public LeafFieldComparator {
    public:
        Leaf(ScoreFieldComparator &parent) : parent_(parent) {}

        void set_scorer(Scorable &scorer) override { scorer_ = &scorer; }

        void set_bottom(int32_t /*slot*/) override {}

        void set_top_value(const SortValue & /*value*/) override {}

        [[nodiscard]] Result<int32_t> compare_bottom(int32_t /*doc*/) override {
            if (scorer_ == nullptr) {
                return err(LuceneError::illegal_state("a score comparator needs a scorer"));
            }
            auto score = scorer_->score();
            if (!score.has_value()) {
                return std::unexpected(std::move(score.error()));
            }
            const int32_t result = score.value() < parent_.bottom_ ? -1 : (score.value() > parent_.bottom_ ? 1 : 0);
            return parent_.reverse_ ? result : -result;
        }

        [[nodiscard]] Result<int32_t> compare_top(int32_t /*doc*/) override { return 0; }

        [[nodiscard]] VoidResult copy(int32_t slot, int32_t /*doc*/) override {
            if (scorer_ == nullptr) {
                return err(LuceneError::illegal_state("a score comparator needs a scorer"));
            }
            auto score = scorer_->score();
            if (!score.has_value()) {
                return std::unexpected(std::move(score.error()));
            }
            parent_.values_[static_cast<size_t>(slot)] = score.value();
            return {};
        }

    private:
        ScoreFieldComparator &parent_;
        Scorable *scorer_{nullptr};
    };

    return std::unique_ptr<LeafFieldComparator>(new Leaf(*this));
}

// --------------------------------------------------------- DocFieldComparator

DocFieldComparator::DocFieldComparator(int32_t num_hits, bool reverse)
        : values_(static_cast<size_t>(std::max(num_hits, 0))), reverse_(reverse) {}

int32_t DocFieldComparator::compare(int32_t slot1, int32_t slot2) const {
    const int32_t first = values_[static_cast<size_t>(slot1)];
    const int32_t second = values_[static_cast<size_t>(slot2)];
    const int32_t result = first < second ? -1 : (first > second ? 1 : 0);
    return reverse_ ? -result : result;
}

int32_t DocFieldComparator::compare_bottom(int32_t slot) const {
    const int32_t value = values_[static_cast<size_t>(slot)];
    const int32_t result = value < bottom_ ? -1 : (value > bottom_ ? 1 : 0);
    return reverse_ ? -result : result;
}

int32_t DocFieldComparator::compare_top(int32_t slot) const {
    if (!top_.has_value()) {
        return 0;
    }
    const int32_t value = values_[static_cast<size_t>(slot)];
    const int32_t result = value < top_.value() ? -1 : (value > top_.value() ? 1 : 0);
    return reverse_ ? -result : result;
}

void DocFieldComparator::set_bottom(int32_t slot) { bottom_ = values_[static_cast<size_t>(slot)]; }

void DocFieldComparator::set_top_value(const SortValue &value) {
    if (const auto *doc = std::get_if<int32_t>(&value); doc != nullptr) {
        top_ = *doc;
    }
}

Result<SortValue> DocFieldComparator::value(int32_t slot) const { return SortValue(values_[static_cast<size_t>(slot)]); }

Result<int32_t> DocFieldComparator::compare_values(const SortValue &first, const SortValue &second) const {
    const int32_t result = natural_compare(first, second);
    return reverse_ ? -result : result;
}

Result<std::unique_ptr<LeafFieldComparator>> DocFieldComparator::get_leaf_comparator(
        const LeafReaderContext &context) {
    class Leaf final : public LeafFieldComparator {
    public:
        Leaf(DocFieldComparator &parent, int32_t doc_base) : parent_(parent), doc_base_(doc_base) {}

        void set_bottom(int32_t /*slot*/) override {}
        void set_top_value(const SortValue & /*value*/) override {}

        [[nodiscard]] Result<int32_t> compare_bottom(int32_t doc) override {
            const int32_t value = doc + doc_base_;
            const int32_t result = value < parent_.bottom_ ? -1 : (value > parent_.bottom_ ? 1 : 0);
            return parent_.reverse_ ? -result : result;
        }

        [[nodiscard]] Result<int32_t> compare_top(int32_t /*doc*/) override { return 0; }

        [[nodiscard]] VoidResult copy(int32_t slot, int32_t doc) override {
            parent_.values_[static_cast<size_t>(slot)] = doc + doc_base_;
            return {};
        }

    private:
        DocFieldComparator &parent_;
        int32_t doc_base_{0};
    };

    return std::unique_ptr<LeafFieldComparator>(new Leaf(*this, context.doc_base()));
}

// ------------------------------------------------- DocValuesFieldComparator

DocValuesFieldComparator::DocValuesFieldComparator(std::string field, SortFieldType type, bool reverse,
                                                   int32_t num_hits, SortFieldSelector selector)
        : field_(std::move(field)), type_(type), reverse_(reverse), selector_(selector) {
    const size_t slots = static_cast<size_t>(std::max(num_hits, 0));
    if (type_ == SortFieldType::kString || type_ == SortFieldType::kSortedSet) {
        values_.assign(slots, SortValue(std::string{}));
        bottom_ = std::string{};
    } else if (type_ == SortFieldType::kFloat || type_ == SortFieldType::kDouble ||
               type_ == SortFieldType::kSortedNumericDouble) {
        values_.assign(slots, SortValue(0.0));
        bottom_ = 0.0;
    } else {
        values_.assign(slots, SortValue(int64_t{0}));
        bottom_ = int64_t{0};
    }
}

int32_t DocValuesFieldComparator::compare_held(const SortValue &first, const SortValue &second) const {
    const int32_t result = natural_compare(first, second);
    return reverse_ ? -result : result;
}

int32_t DocValuesFieldComparator::compare(int32_t slot1, int32_t slot2) const {
    return compare_held(values_[static_cast<size_t>(slot1)], values_[static_cast<size_t>(slot2)]);
}

int32_t DocValuesFieldComparator::compare_bottom(int32_t slot) const {
    return compare_held(values_[static_cast<size_t>(slot)], bottom_);
}

int32_t DocValuesFieldComparator::compare_top(int32_t slot) const {
    if (!top_.has_value()) {
        return 0;
    }
    return compare_held(values_[static_cast<size_t>(slot)], top_.value());
}

void DocValuesFieldComparator::set_bottom(int32_t slot) { bottom_ = values_[static_cast<size_t>(slot)]; }

void DocValuesFieldComparator::set_top_value(const SortValue &value) { top_ = value; }

Result<SortValue> DocValuesFieldComparator::value(int32_t slot) const {
    return values_[static_cast<size_t>(slot)];
}

Result<int32_t> DocValuesFieldComparator::compare_values(const SortValue &first, const SortValue &second) const {
    return compare_held(first, second);
}

Result<std::unique_ptr<LeafFieldComparator>> DocValuesFieldComparator::get_leaf_comparator(
        const LeafReaderContext &context) {
    LeafReader *reader = context.reader();
    if (reader == nullptr) {
        return err(LuceneError::illegal_state("a doc values comparator needs a reader"));
    }
    const bool is_string = type_ == SortFieldType::kString;
    const bool is_double = type_ == SortFieldType::kFloat || type_ == SortFieldType::kDouble ||
                           type_ == SortFieldType::kSortedNumericDouble;

    std::unique_ptr<SortedDocValues> sorted;
    std::unique_ptr<NumericDocValues> numeric;
    std::unique_ptr<SortedNumericDocValues> sorted_numeric;
    std::unique_ptr<SortedSetDocValues> sorted_set;
    if (type_ == SortFieldType::kSortedNumeric || type_ == SortFieldType::kSortedNumericDouble) {
        auto values = reader->get_sorted_numeric_doc_values(field_);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        sorted_numeric = std::move(values.value());
    } else if (type_ == SortFieldType::kSortedSet) {
        auto values = reader->get_sorted_set_doc_values(field_);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        sorted_set = std::move(values.value());
    } else if (is_string) {
        auto values = reader->get_sorted_doc_values(field_);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        sorted = std::move(values.value());
    } else {
        auto values = reader->get_numeric_doc_values(field_);
        if (!values.has_value()) {
            return std::unexpected(std::move(values.error()));
        }
        numeric = std::move(values.value());
    }

    class Leaf final : public LeafFieldComparator {
    public:
        Leaf(DocValuesFieldComparator &parent, std::unique_ptr<SortedDocValues> sorted,
             std::unique_ptr<NumericDocValues> numeric, std::unique_ptr<SortedNumericDocValues> sorted_numeric,
             std::unique_ptr<SortedSetDocValues> sorted_set, bool is_string, bool is_double, SortFieldType type,
             SortFieldSelector selector)
                : parent_(parent), sorted_(std::move(sorted)), numeric_(std::move(numeric)),
                  sorted_numeric_(std::move(sorted_numeric)), sorted_set_(std::move(sorted_set)),
                  is_string_(is_string), is_double_(is_double), type_(type), selector_(selector) {}

        void set_bottom(int32_t /*slot*/) override {}
        void set_top_value(const SortValue &value) override { top_ = value; has_top_ = true; }

        [[nodiscard]] Result<int32_t> compare_bottom(int32_t doc) override {
            auto value = read(doc);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            return parent_.compare_held(value.value(), parent_.bottom_);
        }

        [[nodiscard]] Result<int32_t> compare_top(int32_t doc) override {
            if (!has_top_) {
                return 0;
            }
            auto value = read(doc);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            return parent_.compare_held(value.value(), top_);
        }

        [[nodiscard]] VoidResult copy(int32_t slot, int32_t doc) override {
            auto value = read(doc);
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            parent_.values_[static_cast<size_t>(slot)] = std::move(value.value());
            return {};
        }

    private:
        [[nodiscard]] Result<SortValue> read(int32_t doc) {
            if (is_string_) {
                if (sorted_ == nullptr) {
                    return SortValue(std::string{});
                }
                auto advanced = sorted_->advance_exact(doc);
                if (!advanced.has_value()) {
                    return std::unexpected(std::move(advanced.error()));
                }
                if (!advanced.value()) {
                    return SortValue(std::string{});
                }
                auto ord = sorted_->ord_value();
                if (!ord.has_value()) {
                    return std::unexpected(std::move(ord.error()));
                }
                auto term = sorted_->lookup_ord(ord.value());
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                auto text = term.value().utf8_to_string();
                if (!text.has_value()) {
                    return SortValue(std::string{});
                }
                return SortValue(std::move(text.value()));
            }
            if (sorted_numeric_ != nullptr) {
                auto advanced = sorted_numeric_->advance_exact(doc);
                if (!advanced.has_value()) {
                    return std::unexpected(std::move(advanced.error()));
                }
                if (!advanced.value()) {
                    return is_double_ ? SortValue(0.0) : SortValue(int64_t{0});
                }
                auto count = sorted_numeric_->doc_value_count();
                if (!count.has_value()) {
                    return std::unexpected(std::move(count.error()));
                }
                if (count.value() == 0) {
                    return is_double_ ? SortValue(0.0) : SortValue(int64_t{0});
                }
                bool first = true;
                int64_t chosen = 0;
                double chosen_double = 0.0;
                for (int32_t i = 0; i < count.value(); ++i) {
                    auto raw = sorted_numeric_->next_value();
                    if (!raw.has_value()) {
                        return std::unexpected(std::move(raw.error()));
                    }
                    const double candidate = is_double_ ? std::bit_cast<double>(raw.value())
                                                        : static_cast<double>(raw.value());
                    if (first) {
                        chosen = raw.value();
                        chosen_double = candidate;
                        first = false;
                        continue;
                    }
                    const bool better =
                            selector_ == SortFieldSelector::kMax ? candidate > chosen_double : candidate < chosen_double;
                    if (better) {
                        chosen = raw.value();
                        chosen_double = candidate;
                    }
                }
                return is_double_ ? SortValue(chosen_double) : SortValue(chosen);
            }
            if (sorted_set_ != nullptr) {
                auto advanced = sorted_set_->advance_exact(doc);
                if (!advanced.has_value()) {
                    return std::unexpected(std::move(advanced.error()));
                }
                if (!advanced.value()) {
                    return SortValue(std::string{});
                }
                bool first = true;
                int64_t chosen = 0;
                while (true) {
                    auto ord = sorted_set_->next_ord();
                    if (!ord.has_value()) {
                        return std::unexpected(std::move(ord.error()));
                    }
                    if (ord.value() == NO_MORE_ORDS) {
                        break;
                    }
                    if (first) {
                        chosen = ord.value();
                        first = false;
                        continue;
                    }
                    const bool better =
                            selector_ == SortFieldSelector::kMax ? ord.value() > chosen : ord.value() < chosen;
                    if (better) {
                        chosen = ord.value();
                    }
                }
                if (first) {
                    return SortValue(std::string{});
                }
                auto term = sorted_set_->lookup_ord(chosen);
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                auto text = term.value().utf8_to_string();
                if (!text.has_value()) {
                    return SortValue(std::string{});
                }
                return SortValue(std::move(text.value()));
            }
            if (numeric_ == nullptr) {
                return is_double_ ? SortValue(0.0) : SortValue(int64_t{0});
            }
            auto advanced = numeric_->advance_exact(doc);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            if (!advanced.value()) {
                return is_double_ ? SortValue(0.0) : SortValue(int64_t{0});
            }
            auto raw = numeric_->long_value();
            if (!raw.has_value()) {
                return std::unexpected(std::move(raw.error()));
            }
            if (type_ == SortFieldType::kFloat) {
                const float value = std::bit_cast<float>(static_cast<int32_t>(raw.value()));
                return SortValue(static_cast<double>(value));
            }
            if (type_ == SortFieldType::kDouble) {
                return SortValue(std::bit_cast<double>(raw.value()));
            }
            return SortValue(raw.value());
        }

        DocValuesFieldComparator &parent_;
        std::unique_ptr<SortedDocValues> sorted_;
        std::unique_ptr<NumericDocValues> numeric_;
        std::unique_ptr<SortedNumericDocValues> sorted_numeric_;
        std::unique_ptr<SortedSetDocValues> sorted_set_;
        bool is_string_{false};
        bool is_double_{false};
        SortFieldType type_{SortFieldType::kString};
        SortFieldSelector selector_{SortFieldSelector::kMin};
        SortValue top_{};
        bool has_top_{false};
    };

    return std::unique_ptr<LeafFieldComparator>(
            new Leaf(*this, std::move(sorted), std::move(numeric), std::move(sorted_numeric), std::move(sorted_set),
                     is_string, is_double, type_, selector_));
}

// ------------------------------------------------------------- the factory

Result<std::unique_ptr<FieldComparator>> comparator_for(const SortField &sort_field, int32_t num_hits) {
    switch (sort_field.type()) {
        case SortFieldType::kScore:
            return std::unique_ptr<FieldComparator>(new ScoreFieldComparator(num_hits, sort_field.reverse()));
        case SortFieldType::kDoc:
            return std::unique_ptr<FieldComparator>(new DocFieldComparator(num_hits, sort_field.reverse()));
        case SortFieldType::kString:
        case SortFieldType::kInt:
        case SortFieldType::kFloat:
        case SortFieldType::kLong:
        case SortFieldType::kDouble:
        case SortFieldType::kSortedNumeric:
        case SortFieldType::kSortedNumericDouble:
        case SortFieldType::kSortedSet:
            if (!sort_field.field().has_value()) {
                return err(LuceneError::illegal_argument("a doc values sort field needs a field"));
            }
            return std::unique_ptr<FieldComparator>(new DocValuesFieldComparator(
                    sort_field.field().value(), sort_field.type(), sort_field.reverse(), num_hits,
                    sort_field.selector()));
        case SortFieldType::kCustom: {
            const auto &factory = sort_field.custom_comparator_factory();
            if (!factory) {
                return err(LuceneError::illegal_argument("the custom sort field has no comparator factory"));
            }
            return factory(num_hits);
        }
    }
    return err(LuceneError::illegal_argument("unknown sort field type"));
}

} // namespace lucene
