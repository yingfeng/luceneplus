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

export module lucene_core:doc_values_rewrite_method;

import :types;
import std;

import :error;
import :multi_term_query;
import :query;
import :query_visitor;
import :score_mode;
import :weight;

namespace lucene {

/// Matches the documents whose doc values contain one of the terms a
/// [`MultiTermQuery`] selects.
///
/// Port of `MultiTermQueryDocValuesWrapper`
/// (`core/search/doc_values_rewrite_method.rs`), the query the doc values rewrite
/// method produces.  This is what makes a multi-term query usable on a field
/// that has doc values but no postings: the terms come from the field's
/// dictionary (a `SortedSetDocValues`), and the documents are found by walking
/// the doc values of every leaf.
///
/// The reference implementation wraps it in a
/// [`ConstantScoreQuery`](lucene_core:constant_score_query) when it rewrites, so
/// every match scores the query boost.
///
/// **Deviations:**
///  * the "singleton" fast path (a single-valued `SortedDocValues` probed with
///    one `ord` per document) is not ported; the port always walks the sorted set
///    ordinals, which gives the same documents;
///  * the doc values skipper shortcut is not ported: the port walks the whole
///    sorted set instead of skipping blocks through
///    [`DocValuesRangeIterator`](:doc_values_range_iterator);
///  * `Weight::matches` is not ported, for the same reason as elsewhere.
export class MultiTermQueryDocValuesWrapper final : public Query {
public:
    /// Wraps `query`; the query has to be owned by a shared pointer, because the
    /// weight keeps using it.
    [[nodiscard]] static Result<std::shared_ptr<MultiTermQueryDocValuesWrapper>>
    create(std::shared_ptr<const MultiTermQuery> query);

    /// The field whose doc values are read.
    [[nodiscard]] std::string_view field() const;

    /// The wrapped multi-term query.
    [[nodiscard]] const std::shared_ptr<const MultiTermQuery> &query() const noexcept { return query_; }

    // ---- Query ----

    [[nodiscard]] Result<std::unique_ptr<Weight>> create_weight(const IndexSearcher &searcher, ScoreMode score_mode,
                                                                float boost) const override;

    [[nodiscard]] VoidResult visit(QueryVisitor &visitor) const override;

    [[nodiscard]] Result<std::string> to_string(std::string_view field) const override;

    [[nodiscard]] bool equals(const Query &other) const override;

    [[nodiscard]] size_t hash() const override;

private:
    explicit MultiTermQueryDocValuesWrapper(std::shared_ptr<const MultiTermQuery> query);

    std::shared_ptr<const MultiTermQuery> query_;
};

} // namespace lucene
