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

export module lucene_core:sort_field;

import :types;
import std;

import :error;
import :index_sorter;
import :leaf_reader;

namespace lucene {

export class FieldComparator;

/// How the values of a sort field are read and compared.
///
/// Port of `SortFieldType` (`core/search/sort_field.rs`).
///
/// **Deviation:** the reference implementation's `StringVal`/`IntVal`/... types,
/// which sort on doc values that are not also indexed as points, and the
/// `Rewritable` type are not ported.  The `Custom` type is ported as a hook: a
/// custom sort field carries its own comparator factory, which is what the
/// distance sorts of the geo fields need.
export enum class SortFieldType : int32_t {
    /// Sort by relevance; higher scores come first.
    kScore = 0,
    /// Sort by document number; lower documents come first.
    kDoc = 1,
    /// Sort by a `SortedDocValues` term; lower terms come first.
    kString = 2,
    /// Sort by an `int` in `NumericDocValues`.
    kInt = 3,
    /// Sort by a `float` in `NumericDocValues`.
    kFloat = 4,
    /// Sort by a `long` in `NumericDocValues`.
    kLong = 5,
    /// Sort by a `double` in `NumericDocValues`.
    kDouble = 6,
    /// Sort by a `long` in `SortedNumericDocValues`, taking the
    /// [`selector`](SortField::selector) value.
    kSortedNumeric = 7,
    /// Sort by a `double` in `SortedNumericDocValues`, taking the
    /// [`selector`](SortField::selector) value (the values hold the raw bits).
    kSortedNumericDouble = 8,
    /// Sort by a term in `SortedSetDocValues`, taking the
    /// [`selector`](SortField::selector) value.
    kSortedSet = 9,
    /// Sort by a comparator the field brings along, via
    /// [`SortField::with_custom`].
    kCustom = 10,
};

/// Which value of a multi-valued field a sort uses.
export enum class SortFieldSelector : uint8_t {
    /// The smallest value of the document.
    kMin = 0,
    /// The largest value of the document.
    kMax = 1,
};

/// How one field takes part in a sort.
///
/// Port of `SortField` (`core/search/sort_field.rs`).
///
/// **Deviation:** missing values (`sortMissingFirst`/`sortMissingLast`) are not
/// ported; a document without a value sorts as `0` for a numeric field and as the
/// empty string for a term field, which is the reference implementation's default.
export class SortField {
public:
    /// A sort on `field` of `type`; the field may be absent only for the
    /// [`kScore`](SortFieldType::kScore) and [`kDoc`](SortFieldType::kDoc) types.
    [[nodiscard]] static Result<SortField> create(std::optional<std::string> field, SortFieldType type);

    /// [`create`], with the natural order reversed when `reverse`.
    [[nodiscard]] static Result<SortField> with_reverse(std::optional<std::string> field, SortFieldType type,
                                                       bool reverse);

    /// A sort on a multi-valued field; `selector` picks the value of a document
    /// that has several.  `type` has to be one of the multi-valued types.
    [[nodiscard]] static Result<SortField> with_selector(std::optional<std::string> field, SortFieldType type,
                                                         SortFieldSelector selector);

    /// [`with_selector`], with the natural order reversed when `reverse`.
    [[nodiscard]] static Result<SortField> with_reverse_selector(std::optional<std::string> field, SortFieldType type,
                                                               SortFieldSelector selector, bool reverse);

    /// Sorts by descending relevance.
    [[nodiscard]] static Result<SortField> score();

    /// Sorts by ascending document number.
    [[nodiscard]] static Result<SortField> doc();

    /// A sort field that brings its own comparator, for the sorts whose values
    /// are computed rather than read from a field (the distance sorts, for
    /// instance).
    ///
    /// `display` is what [`to_string`] returns; `factory` builds a comparator
    /// over `num_hits` slots.
    ///
    /// **Deviation:** the reference implementation makes the custom sort field a
    /// separate type that wraps a `SortField`; the port keeps one type and stores
    /// the factory in it, because the search layer consumes `SortField`s and not
    /// their subclasses.
    using CustomComparatorFactory = std::function<std::unique_ptr<FieldComparator>(int32_t num_hits)>;

    [[nodiscard]] static Result<SortField> with_custom(std::optional<std::string> field, std::string display,
                                                      CustomComparatorFactory factory);

    /// Whether the field brings its own comparator.
    [[nodiscard]] bool is_custom() const noexcept { return type_ == SortFieldType::kCustom; }
    /// The text a custom field prints; empty for the other types.
    [[nodiscard]] const std::string &custom_display() const noexcept { return custom_display_; }
    /// The comparator factory of a custom field; null for the other types.
    [[nodiscard]] const CustomComparatorFactory &custom_comparator_factory() const noexcept {
        return custom_factory_;
    }

    /// The field the sort reads, absent for the score and doc types.
    [[nodiscard]] const std::optional<std::string> &field() const noexcept { return field_; }
    /// How the values are read and compared.
    [[nodiscard]] SortFieldType type() const noexcept { return type_; }
    /// Whether the natural order is reversed.
    [[nodiscard]] bool reverse() const noexcept { return reverse_; }
    /// Which value of a multi-valued field the sort uses.
    [[nodiscard]] SortFieldSelector selector() const noexcept { return selector_; }
    /// Whether the type reads a multi-valued field.
    [[nodiscard]] bool is_multi_valued() const noexcept {
        return type_ == SortFieldType::kSortedNumeric || type_ == SortFieldType::kSortedNumericDouble ||
               type_ == SortFieldType::kSortedSet;
    }
    /// Whether sorting needs the scores, i.e. the type is
    /// [`kScore`](SortFieldType::kScore).
    [[nodiscard]] bool needs_scores() const noexcept { return type_ == SortFieldType::kScore; }

    /// The sorter that orders the documents of a segment by this field, used when
    /// an index is created with an index sort.
    ///
    /// **Deviation:** the multi-valued types need the search layer's sort-field
    /// providers, which the port does not have, so only the scalar types are
    /// supported; `kDoc` cannot be used as an index sort at all.
    [[nodiscard]] Result<std::unique_ptr<IndexSorter>> index_sorter() const;

    /// `<score>`, `<string: "field">` or `<int: "field">`, with `!` appended when
    /// reversed, the form the reference implementation prints.
    [[nodiscard]] Result<std::string> to_string() const;

    [[nodiscard]] bool equals(const SortField &other) const noexcept {
        return field_ == other.field_ && type_ == other.type_ && reverse_ == other.reverse_ &&
               selector_ == other.selector_;
    }

    [[nodiscard]] size_t hash() const noexcept;

    [[nodiscard]] bool operator==(const SortField &other) const noexcept { return equals(other); }

private:
    SortField(std::optional<std::string> field, SortFieldType type, bool reverse, SortFieldSelector selector);

    std::optional<std::string> field_;
    SortFieldType type_{SortFieldType::kScore};
    bool reverse_{false};
    SortFieldSelector selector_{SortFieldSelector::kMin};
    /// What a [`kCustom`](SortFieldType::kCustom) field prints.
    std::string custom_display_;
    /// The comparator of a [`kCustom`](SortFieldType::kCustom) field.
    CustomComparatorFactory custom_factory_;
};

/// The sort criteria of a search: the fields are compared in order, and the
/// document number breaks the remaining ties.
///
/// Port of `Sort` (`core/search/sort.rs`).
export class Sort {
public:
    /// Sorts by relevance.
    [[nodiscard]] static Result<Sort> relevance();

    /// Sorts by index order.
    [[nodiscard]] static Result<Sort> index_order();

    /// Sorts by `fields` in priority order; the list must not be empty.
    [[nodiscard]] static Result<Sort> with_fields(std::vector<SortField> fields);

    /// The sort fields, in priority order.
    [[nodiscard]] const std::vector<SortField> &fields() const noexcept { return fields_; }

    /// Whether any field needs the scores.
    [[nodiscard]] bool needs_scores() const noexcept;

    /// The fields joined with `,`, the form the reference implementation prints.
    [[nodiscard]] std::string to_string() const;

    /// The permutation that puts the `max_doc` documents of `leaf_reader` into
    /// this order, or `nullopt` when they are already in it.
    ///
    /// This is what an index sort needs: the fields are applied in priority
    /// order, and a document without a value sorts as the sorter of its field
    /// says.  Fails when a field cannot drive an index sort (see
    /// [`SortField::index_sorter`]).
    [[nodiscard]] Result<std::optional<IndexSortDocMapImpl>> sort_documents(LeafReader &leaf_reader,
                                                                           int32_t max_doc) const;

    [[nodiscard]] bool equals(const Sort &other) const noexcept { return fields_ == other.fields_; }

    [[nodiscard]] size_t hash() const noexcept;

private:
    explicit Sort(std::vector<SortField> fields);

    std::vector<SortField> fields_;
};

} // namespace lucene
