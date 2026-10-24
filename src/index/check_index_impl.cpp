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

module lucene_core:check_index.impl;

import :check_index;

import std;

import :automata;
import :automaton;
import :bits;
import :byte_run_automaton;
import :bytes_ref;
import :codec_reader;
import :compiled_automaton;
import :directory;
import :doc_id_set_iterator;
import :error;
import :field_infos;
import :fixed_bit_set;
import :index_commit;
import :index_types;
import :io_context;
import :leaf_reader;
import :operations;
import :postings_enum;
import :run_automaton;
import :segment_commit_info;
import :segment_infos;
import :segment_reader;
import :term_vectors;
import :terms;
import :terms_enum;
import :types;

namespace lucene {

namespace {

/// The automata [`check_terms_intersect`] probes a term dictionary with.
///
/// Port of the automata `CheckIndex::test_postings` builds for every field.
struct IntersectProbes {
    /// `any* [a-e] any*`: matches a good part of a normal dictionary.
    Automaton any_between;
    /// Every non-empty term.
    Automaton non_empty;
};

/// Runs `bytes` through `run` the way `ByteRunAutomaton::run` does.
Result<bool> run_bytes(ByteRunAutomaton &run, std::span<const uint8_t> bytes) {
    int32_t state = 0;
    for (const uint8_t byte : bytes) {
        auto next = run.step(state, static_cast<int32_t>(byte));
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        state = next.value();
        if (state == -1) {
            return false;
        }
    }
    return run.is_accept(state);
}

/// Verifies `Terms::intersect` against a linear scan filtered by `automaton`.
///
/// Port of `CheckIndex::check_terms_intersect`: the intersection may skip terms,
/// but it must yield exactly the terms the linear scan accepts, in the same order.
VoidResult check_terms_intersect(Terms &terms, const Automaton &automaton, const BytesRef *start_term) {
    auto all_terms = terms.iterator();
    if (!all_terms.has_value()) {
        return std::unexpected(std::move(all_terms.error()));
    }
    if (all_terms.value() == nullptr) {
        return {}; // An empty dictionary has nothing to intersect.
    }

    auto compiled = CompiledAutomaton::with_binary(automaton, false, true, true);
    if (!compiled.has_value()) {
        return std::unexpected(std::move(compiled.error()));
    }
    auto run = ByteRunAutomaton::create_with_binary(automaton, true);
    if (!run.has_value()) {
        return std::unexpected(std::move(run.error()));
    }

    auto filtered = terms.intersect(compiled.value(), start_term);
    if (!filtered.has_value()) {
        return std::unexpected(std::move(filtered.error()));
    }
    if (filtered.value() == nullptr) {
        return err(LuceneError::corrupt_index("the term intersection returned no enumeration"));
    }

    bool use_current_term = false;
    bool exhausted = false;
    if (start_term != nullptr) {
        auto status = all_terms.value()->seek_ceil(start_term->as_slice());
        if (!status.has_value()) {
            return std::unexpected(std::move(status.error()));
        }
        switch (status.value()) {
            case SeekStatus::Found: break;
            case SeekStatus::NotFound: use_current_term = true; break;
            case SeekStatus::End: exhausted = true; break;
        }
    }

    if (!exhausted) {
        while (true) {
            const BytesRef *current = nullptr;
            if (use_current_term) {
                use_current_term = false;
                auto term = all_terms.value()->term();
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                current = term.value();
            } else {
                auto term = all_terms.value()->next();
                if (!term.has_value()) {
                    return std::unexpected(std::move(term.error()));
                }
                current = term.value();
            }
            if (current == nullptr) {
                break;
            }

            auto matches = run_bytes(run.value(), current->as_slice());
            if (!matches.has_value()) {
                return std::unexpected(std::move(matches.error()));
            }
            if (!matches.value()) {
                continue;
            }

            auto filtered_term = filtered.value()->next();
            if (!filtered_term.has_value()) {
                return std::unexpected(std::move(filtered_term.error()));
            }
            if (filtered_term.value() == nullptr || filtered_term.value()->compare_to(*current) != 0) {
                return err(LuceneError::corrupt_index(
                        "the term intersection returned a term the linear scan did not expect"));
            }
        }
    }

    auto trailing = filtered.value()->next();
    if (!trailing.has_value()) {
        return std::unexpected(std::move(trailing.error()));
    }
    if (trailing.value() != nullptr) {
        return err(LuceneError::corrupt_index("the term intersection returned more terms than the linear scan"));
    }
    return {};
}

/// Builds the probes of [`check_terms_intersect`].
Result<IntersectProbes> make_intersect_probes() {
    auto any_before = Automata::make_any_binary();
    if (!any_before.has_value()) {
        return std::unexpected(std::move(any_before.error()));
    }
    auto char_range = Automata::make_char_range(static_cast<int32_t>('a'), static_cast<int32_t>('e'));
    if (!char_range.has_value()) {
        return std::unexpected(std::move(char_range.error()));
    }
    auto any_after = Automata::make_any_binary();
    if (!any_after.has_value()) {
        return std::unexpected(std::move(any_after.error()));
    }

    const std::vector<const Automaton *> parts{&any_before.value(), &char_range.value(), &any_after.value()};
    auto concatenated = Operations::concatenate_list(parts);
    if (!concatenated.has_value()) {
        return std::unexpected(std::move(concatenated.error()));
    }
    auto non_empty = Automata::make_non_empty_binary();
    if (!non_empty.has_value()) {
        return std::unexpected(std::move(non_empty.error()));
    }

    // `CompiledAutomaton` requires a deterministic automaton, and concatenation
    // produces an NFA.
    Automaton any_between = std::move(concatenated.value());
    auto deterministic = Operations::determinize(any_between, static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    if (!deterministic.has_value()) {
        return std::unexpected(std::move(deterministic.error()));
    }
    if (deterministic.value().has_value()) {
        any_between = std::move(deterministic.value().value());
    }
    return IntersectProbes{std::move(any_between), std::move(non_empty.value())};
}

/// Runs `check` and records what it reports, so one broken field does not stop the
/// rest of the segment from being checked.
template <class F>
void expect_ok(std::vector<std::string> &failures, std::string_view what, F &&check) {
    auto result = check();
    if (!result.has_value()) {
        failures.push_back(std::string(what) + ": " + result.error().to_string());
    }
}

/// `true` when `values` could be produced, recording the failure otherwise.
template <class F>
void expect_present(std::vector<std::string> &failures, std::string_view what, F &&values) {
    auto produced = values();
    if (!produced.has_value()) {
        failures.push_back(std::string(what) + ": " + produced.error().to_string());
        return;
    }
    if (produced.value() == nullptr) {
        failures.push_back(std::string(what) + ": the field has no values");
    }
}

/// Checks the live docs of `reader` against the delete count of `info`.
void check_live_docs(SegmentCheckResult &segment, const SegmentReader &reader, const SegmentCommitInfo &info) {
    auto live_docs = reader.live_docs();
    if (!live_docs.has_value()) {
        segment.failures.push_back("live docs: " + live_docs.error().to_string());
        return;
    }
    if (live_docs.value() == nullptr) {
        if (info.total_del_count() > 0) {
            segment.failures.push_back("live docs: the commit has " + std::to_string(info.total_del_count()) +
                                       " deletes but the segment has none");
        }
        return;
    }

    const Bits &bits = *live_docs.value();
    if (bits.length() != static_cast<size_t>(segment.max_doc)) {
        segment.failures.push_back("live docs: length " + std::to_string(bits.length()) +
                                   " does not match maxDoc " + std::to_string(segment.max_doc));
        return;
    }

    int64_t deleted = 0;
    for (size_t doc = 0; doc < bits.length(); ++doc) {
        auto live = bits.get(doc);
        if (!live.has_value()) {
            segment.failures.push_back("live docs: cannot read document " + std::to_string(doc) + ": " +
                                       live.error().to_string());
            return;
        }
        if (!live.value()) {
            ++deleted;
        }
    }
    if (deleted != info.total_del_count()) {
        segment.failures.push_back("live docs: " + std::to_string(deleted) + " deleted documents but the commit says " +
                                   std::to_string(info.total_del_count()));
    }
}

/// Walks the term dictionary of `field` and verifies the postings it reports.
///
/// Port of the part of `CheckIndex::test_postings` that this port can run: term
/// order, `docFreq`, `totalTermFreq`, doc id order, frequencies, positions and
/// offsets, plus the aggregates the field advertises.  The reference
/// implementation additionally verifies `doc_count`, term vectors against
/// postings and the term dictionary against the block tree; those need machinery
/// this port does not have.
void check_postings_of_field(SegmentCheckResult &segment, SegmentReader &reader, const std::string &field,
                             const IntersectProbes *probes) {
    auto terms = reader.terms(field);
    if (!terms.has_value()) {
        segment.failures.push_back("terms of " + field + ": " + terms.error().to_string());
        return;
    }
    if (terms.value() == nullptr) {
        segment.failures.push_back("terms of " + field + ": the field has no terms");
        return;
    }
    Terms &field_terms = *terms.value();

    auto terms_enum = field_terms.iterator();
    if (!terms_enum.has_value()) {
        segment.failures.push_back("terms of " + field + ": " + terms_enum.error().to_string());
        return;
    }
    if (terms_enum.value() == nullptr) {
        auto size = field_terms.size();
        if (!size.has_value() || size.value() != 0) {
            segment.failures.push_back("terms of " + field + ": no term iterator but a non-zero term count");
        }
        return;
    }

    const bool has_freqs = field_terms.has_freqs();
    const bool has_positions = field_terms.has_positions();
    const bool has_offsets = field_terms.has_offsets();

    int64_t term_count = 0;
    int64_t sum_doc_freq = 0;
    int64_t sum_total_term_freq = 0;
    std::optional<BytesRef> previous_term;
    // `doc_count` of the field is the number of documents with at least one term,
    // which the walk can compute exactly.
    FixedBitSet docs_with_field(static_cast<size_t>(segment.max_doc));

    while (true) {
        auto term = terms_enum.value()->next();
        if (!term.has_value()) {
            segment.failures.push_back("terms of " + field + ": " + term.error().to_string());
            return;
        }
        if (term.value() == nullptr) {
            break;
        }
        ++term_count;
        if (previous_term.has_value() && previous_term->compare_to(*term.value()) >= 0) {
            segment.failures.push_back("terms of " + field + ": the term dictionary is not sorted");
            return;
        }
        previous_term = *term.value();

        auto doc_freq = terms_enum.value()->doc_freq();
        if (!doc_freq.has_value()) {
            segment.failures.push_back("terms of " + field + ": " + doc_freq.error().to_string());
            return;
        }
        if (doc_freq.value() < 0) {
            segment.failures.push_back("terms of " + field + ": negative docFreq");
            return;
        }
        sum_doc_freq += doc_freq.value();

        int64_t total_term_freq = 0;
        if (has_freqs) {
            auto total = terms_enum.value()->total_term_freq();
            if (!total.has_value()) {
                segment.failures.push_back("terms of " + field + ": " + total.error().to_string());
                return;
            }
            if (total.value() < doc_freq.value()) {
                segment.failures.push_back("terms of " + field + ": totalTermFreq " + std::to_string(total.value()) +
                                           " is smaller than docFreq " + std::to_string(doc_freq.value()));
                return;
            }
            total_term_freq = total.value();
            sum_total_term_freq += total_term_freq;
        }

        auto postings = terms_enum.value()->postings(std::unique_ptr<PostingsEnum>());
        if (!postings.has_value()) {
            segment.failures.push_back("postings of " + field + ": " + postings.error().to_string());
            return;
        }
        if (postings.value() == nullptr) {
            segment.failures.push_back("postings of " + field + ": no postings for a term with docFreq > 0");
            return;
        }

        int32_t previous_doc = -1;
        int32_t docs = 0;
        int64_t occurrences = 0;
        while (true) {
            auto doc = postings.value()->next_doc();
            if (!doc.has_value()) {
                segment.failures.push_back("postings of " + field + ": " + doc.error().to_string());
                return;
            }
            if (doc.value() == NO_MORE_DOCS) {
                break;
            }
            if (doc.value() <= previous_doc) {
                segment.failures.push_back("postings of " + field + ": doc ids are not increasing");
                return;
            }
            if (doc.value() >= segment.max_doc) {
                segment.failures.push_back("postings of " + field + ": doc id " + std::to_string(doc.value()) +
                                           " is out of range (maxDoc " + std::to_string(segment.max_doc) + ")");
                return;
            }
            previous_doc = doc.value();
            docs_with_field.set(static_cast<size_t>(doc.value()));
            ++docs;

            if (!has_freqs) {
                continue;
            }
            auto freq = postings.value()->freq();
            if (!freq.has_value()) {
                segment.failures.push_back("postings of " + field + ": " + freq.error().to_string());
                return;
            }
            if (freq.value() < 1) {
                segment.failures.push_back("postings of " + field + ": freq < 1");
                return;
            }
            occurrences += freq.value();
            if (!has_positions) {
                continue;
            }

            int32_t previous_position = -1;
            int32_t previous_end_offset = -1;
            for (int32_t i = 0; i < freq.value(); ++i) {
                auto position = postings.value()->next_position();
                if (!position.has_value()) {
                    segment.failures.push_back("postings of " + field + ": " + position.error().to_string());
                    return;
                }
                if (position.value() < previous_position) {
                    segment.failures.push_back("postings of " + field + ": positions are decreasing");
                    return;
                }
                previous_position = position.value();
                if (!has_offsets) {
                    continue;
                }
                auto start = postings.value()->start_offset();
                auto end = postings.value()->end_offset();
                if (!start.has_value() || !end.has_value()) {
                    segment.failures.push_back("postings of " + field + ": offsets are not readable");
                    return;
                }
                if (start.value() < 0 || end.value() < start.value()) {
                    segment.failures.push_back("postings of " + field + ": invalid offsets");
                    return;
                }
                if (start.value() < previous_end_offset) {
                    segment.failures.push_back("postings of " + field + ": offsets are decreasing");
                    return;
                }
                previous_end_offset = end.value();
            }
        }

        segment.posting_count += docs;
        if (docs != doc_freq.value()) {
            segment.failures.push_back("postings of " + field + ": enumerated " + std::to_string(docs) +
                                       " documents but docFreq says " + std::to_string(doc_freq.value()));
            return;
        }
        if (has_freqs && occurrences != total_term_freq) {
            segment.failures.push_back("postings of " + field + ": enumerated " + std::to_string(occurrences) +
                                       " occurrences but totalTermFreq says " +
                                       std::to_string(total_term_freq));
            return;
        }
    }

    segment.term_count += term_count;

    auto size = field_terms.size();
    if (!size.has_value() || size.value() != term_count) {
        segment.failures.push_back("terms of " + field + ": walked " + std::to_string(term_count) +
                                   " terms but the field says " + std::to_string(size.value_or(-1)));
    }
    auto doc_count = field_terms.doc_count();
    const auto seen_docs = static_cast<int64_t>(docs_with_field.cardinality());
    if (!doc_count.has_value() || doc_count.value() != seen_docs) {
        segment.failures.push_back("terms of " + field + ": " + std::to_string(seen_docs) + " documents have a term but " +
                                   "docCount says " + std::to_string(doc_count.value_or(-1)));
    }
    auto sum_doc_freq_from_terms = field_terms.sum_doc_freq();
    if (!sum_doc_freq_from_terms.has_value() || sum_doc_freq_from_terms.value() != sum_doc_freq) {
        segment.failures.push_back("terms of " + field + ": sumDocFreq mismatch");
    }
    if (has_freqs) {
        auto sum_total_term_freq_from_terms = field_terms.sum_total_term_freq();
        if (!sum_total_term_freq_from_terms.has_value() ||
            sum_total_term_freq_from_terms.value() != sum_total_term_freq) {
            segment.failures.push_back("terms of " + field + ": sumTotalTermFreq mismatch");
        }
    }

    if (probes == nullptr) {
        return;
    }

    // `Terms::intersect` must agree with a linear scan over the same automaton.
    const BytesRef empty;
    const BytesRef from_l = BytesRef::from_string("l");
    const BytesRef high(std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF});
    auto record = [&segment, &field](const char *probe, const VoidResult &checked) {
        if (!checked.has_value()) {
            segment.failures.push_back("terms of " + field + " (" + probe + "): " + checked.error().to_string());
        }
    };
    record("intersect, no start term", check_terms_intersect(field_terms, probes->any_between, nullptr));
    record("intersect, empty start term", check_terms_intersect(field_terms, probes->any_between, &empty));
    record("intersect, start term \"l\"", check_terms_intersect(field_terms, probes->non_empty, &from_l));
    record("intersect, high start term", check_terms_intersect(field_terms, probes->non_empty, &high));
}

/// Cross-checks the term vectors of `field` against the postings of the segment.
///
/// Port of the term-vector half of `CheckIndex::test_postings`: every term a
/// document's term vector lists must exist in the field's term dictionary, and
/// the postings of that term must list the document with the same frequency.
/// The reference implementation additionally validates positions, offsets and
/// payloads against the postings; this port compares frequencies only.
void check_term_vectors(SegmentCheckResult &segment, SegmentReader &reader, const std::string &field) {
    auto vectors = reader.term_vectors();
    if (!vectors.has_value()) {
        segment.failures.push_back("term vectors of " + field + ": " + vectors.error().to_string());
        return;
    }
    if (vectors.value() == nullptr) {
        segment.failures.push_back("term vectors of " + field +
                                   ": the field stores them but the segment has no term vectors reader");
        return;
    }

    auto dictionary_terms = reader.terms(field);
    if (!dictionary_terms.has_value() || dictionary_terms.value() == nullptr) {
        segment.failures.push_back("term vectors of " + field + ": the field has no term dictionary");
        return;
    }
    auto dictionary = dictionary_terms.value()->iterator();
    if (!dictionary.has_value() || dictionary.value() == nullptr) {
        return; // An empty dictionary cannot back a term vector.
    }

    auto live_docs = reader.live_docs();
    if (!live_docs.has_value()) {
        segment.failures.push_back("term vectors of " + field + ": " + live_docs.error().to_string());
        return;
    }
    const Bits *live = live_docs.value();

    for (int32_t doc = 0; doc < segment.max_doc; ++doc) {
        if (live != nullptr) {
            auto alive = live->get(static_cast<size_t>(doc));
            if (!alive.has_value()) {
                segment.failures.push_back("term vectors of " + field + ": " + alive.error().to_string());
                return;
            }
            if (!alive.value()) {
                continue; // A deleted document may keep its (stale) vectors.
            }
        }

        auto field_terms = vectors.value()->get_field_terms(doc, field);
        if (!field_terms.has_value()) {
            segment.failures.push_back("term vectors of " + field + ": " + field_terms.error().to_string());
            return;
        }
        if (field_terms.value() == nullptr) {
            continue; // This document has no term vector for the field.
        }
        auto tv_terms = field_terms.value()->iterator();
        if (!tv_terms.has_value() || tv_terms.value() == nullptr) {
            continue;
        }

        while (true) {
            auto tv_term = tv_terms.value()->next();
            if (!tv_term.has_value()) {
                segment.failures.push_back("term vectors of " + field + ": " + tv_term.error().to_string());
                return;
            }
            if (tv_term.value() == nullptr) {
                break;
            }

            // A term vector is a one-document index: document 0, with the frequency
            // of that term in this document.
            auto tv_postings = tv_terms.value()->postings(std::unique_ptr<PostingsEnum>());
            if (!tv_postings.has_value() || tv_postings.value() == nullptr) {
                segment.failures.push_back("term vectors of " + field + ": the term vector has no postings");
                return;
            }
            auto tv_doc = tv_postings.value()->next_doc();
            if (!tv_doc.has_value() || tv_doc.value() != 0) {
                segment.failures.push_back("term vectors of " + field + ": a term vector must expose document 0");
                return;
            }
            auto tv_freq = tv_postings.value()->freq();
            if (!tv_freq.has_value()) {
                segment.failures.push_back("term vectors of " + field + ": " + tv_freq.error().to_string());
                return;
            }

            auto found = dictionary.value()->seek_exact(tv_term.value()->as_slice());
            if (!found.has_value()) {
                segment.failures.push_back("term vectors of " + field + ": " + found.error().to_string());
                return;
            }
            if (!found.value()) {
                segment.failures.push_back("term vectors of " + field + ": document " + std::to_string(doc) +
                                           " lists a term that is not in the term dictionary");
                return;
            }

            auto postings = dictionary.value()->postings(std::unique_ptr<PostingsEnum>());
            if (!postings.has_value() || postings.value() == nullptr) {
                segment.failures.push_back("term vectors of " + field + ": the dictionary has no postings");
                return;
            }
            int32_t current = -1;
            while (true) {
                auto next = postings.value()->next_doc();
                if (!next.has_value()) {
                    segment.failures.push_back("term vectors of " + field + ": " + next.error().to_string());
                    return;
                }
                current = next.value();
                if (current == NO_MORE_DOCS || current >= doc) {
                    break;
                }
            }
            if (current != doc) {
                segment.failures.push_back("term vectors of " + field + ": document " + std::to_string(doc) +
                                           " lists a term whose postings do not contain it");
                return;
            }
            auto posting_freq = postings.value()->freq();
            if (!posting_freq.has_value() || posting_freq.value() != tv_freq.value()) {
                segment.failures.push_back("term vectors of " + field + ": document " + std::to_string(doc) +
                                           " has frequency " + std::to_string(tv_freq.value()) +
                                           " in its term vector but " +
                                           std::to_string(posting_freq.value_or(-1)) + " in the postings");
                return;
            }
        }
    }
}

/// Checks that every field can serve what its `FieldInfo` promises.
void check_fields(SegmentCheckResult &segment, SegmentReader &reader) {
    auto infos = reader.field_infos();
    if (!infos.has_value()) {
        segment.failures.push_back("field infos: " + infos.error().to_string());
        return;
    }
    // The intersect probes are the same for every field of the segment.
    std::optional<IntersectProbes> probes;
    if (auto built = make_intersect_probes(); built.has_value()) {
        probes = std::move(built.value());
    } else {
        segment.failures.push_back("terms: cannot build the intersect probes: " + built.error().to_string());
    }
    const IntersectProbes *probe_pointer = probes.has_value() ? &probes.value() : nullptr;

    for (const FieldInfo &info : infos.value()->fields()) {
        const std::string name(info.name);
        if (info.has_norms()) {
            expect_present(segment.failures, "norms of " + name,
                           [&reader, &name] { return reader.get_norm_values(name); });
        }
        switch (info.doc_values_type) {
            case DocValuesType::None: break;
            case DocValuesType::Numeric:
                expect_present(segment.failures, "numeric doc values of " + name,
                               [&reader, &name] { return reader.get_numeric_doc_values(name); });
                break;
            case DocValuesType::Binary:
                expect_present(segment.failures, "binary doc values of " + name,
                               [&reader, &name] { return reader.get_binary_doc_values(name); });
                break;
            case DocValuesType::Sorted:
                expect_present(segment.failures, "sorted doc values of " + name,
                               [&reader, &name] { return reader.get_sorted_doc_values(name); });
                break;
            case DocValuesType::SortedNumeric:
                expect_present(segment.failures, "sorted numeric doc values of " + name,
                               [&reader, &name] { return reader.get_sorted_numeric_doc_values(name); });
                break;
            case DocValuesType::SortedSet:
                expect_present(segment.failures, "sorted set doc values of " + name,
                               [&reader, &name] { return reader.get_sorted_set_doc_values(name); });
                break;
        }
        if (info.has_point_values()) {
            auto points = reader.point_values(name);
            if (!points.has_value()) {
                segment.failures.push_back("points of " + name + ": " + points.error().to_string());
            } else if (points.value() == nullptr) {
                segment.failures.push_back("points of " + name + ": the field indexes points but has none");
            }
        }
        if (info.is_indexed() && info.index_options != IndexOptions::None) {
            expect_ok(segment.failures, "terms of " + name, [&reader, &name] { return reader.terms(name); });
            check_postings_of_field(segment, reader, name, probe_pointer);
            if (info.has_term_vectors()) {
                check_term_vectors(segment, reader, name);
            }
        }
    }
}

/// Checks the stored fields of the first live document.
void check_stored_fields(SegmentCheckResult &segment, SegmentReader &reader) {
    if (segment.num_docs == 0) {
        return; // Nothing to read.
    }
    auto live_docs = reader.live_docs();
    if (!live_docs.has_value()) {
        segment.failures.push_back("stored fields: " + live_docs.error().to_string());
        return;
    }

    int32_t doc = 0;
    if (live_docs.value() != nullptr) {
        const Bits &bits = *live_docs.value();
        bool found = false;
        for (size_t candidate = 0; candidate < bits.length(); ++candidate) {
            auto live = bits.get(candidate);
            if (!live.has_value()) {
                segment.failures.push_back("stored fields: " + live.error().to_string());
                return;
            }
            if (live.value()) {
                doc = static_cast<int32_t>(candidate);
                found = true;
                break;
            }
        }
        if (!found) {
            segment.failures.push_back("stored fields: the segment claims live documents but has none");
            return;
        }
    }

    auto fields = reader.stored_fields();
    if (!fields.has_value()) {
        segment.failures.push_back("stored fields: " + fields.error().to_string());
        return;
    }
    if (fields.value() == nullptr) {
        segment.failures.push_back("stored fields: the segment has no stored fields reader");
        return;
    }
    expect_ok(segment.failures, "stored fields", [&fields, doc] { return fields.value()->document(doc); });
}

} // namespace

// ---------------- results ----------------

std::string SegmentCheckResult::to_string() const {
    std::string result = "segment " + segment_name + " (maxDoc=" + std::to_string(max_doc) +
                         " numDocs=" + std::to_string(num_docs) + " terms=" + std::to_string(term_count) +
                         " postings=" + std::to_string(posting_count) + "): ";
    if (clean()) {
        result += "OK";
        return result;
    }
    result += "FAILED";
    for (const std::string &failure : failures) {
        result += "\n    " + failure;
    }
    return result;
}

bool CheckIndexResult::clean() const noexcept {
    if (missing_segments_file || !failures.empty()) {
        return false;
    }
    return std::ranges::all_of(segments, [](const SegmentCheckResult &segment) { return segment.clean(); });
}

int64_t CheckIndexResult::total_docs() const noexcept {
    int64_t total = 0;
    for (const SegmentCheckResult &segment : segments) {
        total += segment.num_docs;
    }
    return total;
}

std::string CheckIndexResult::to_string() const {
    if (missing_segments_file) {
        return "no segments file";
    }
    std::string result = "segments file " + segments_file_name + " (generation " + std::to_string(generation) +
                         ", " + std::to_string(segments.size()) + " segments, " + std::to_string(total_docs()) +
                         " documents): ";
    result += clean() ? "OK" : "FAILED";
    for (const std::string &failure : failures) {
        result += "\n  " + failure;
    }
    for (const SegmentCheckResult &segment : segments) {
        if (!segment.clean()) {
            result += "\n  " + segment.to_string();
        }
    }
    return result;
}

// ---------------- CheckIndex ----------------

Result<CheckIndexResult> CheckIndex::check() {
    CheckIndexResult result;

    auto generations = SegmentInfos::list_generations(*directory_);
    if (!generations.has_value()) {
        return std::unexpected(std::move(generations.error()));
    }
    if (generations.value().empty()) {
        result.missing_segments_file = true;
        return result;
    }

    auto latest = SegmentInfos::read_latest(*directory_);
    if (!latest.has_value()) {
        // A commit that cannot be read is exactly what the tool is for.
        result.missing_segments_file = true;
        result.failures.push_back("cannot read the newest commit: " + latest.error().to_string());
        return result;
    }
    return check_infos(latest.value());
}

Result<CheckIndexResult> CheckIndex::check(const IndexCommit &commit) {
    auto infos = SegmentInfos::read_file(*directory_, commit.segments_file_name(), commit.generation());
    if (!infos.has_value()) {
        CheckIndexResult result;
        result.missing_segments_file = true;
        result.generation = commit.generation();
        result.segments_file_name = commit.segments_file_name();
        result.failures.push_back("cannot read the commit: " + infos.error().to_string());
        return result;
    }
    return check_infos(infos.value());
}

Result<CheckIndexResult> CheckIndex::check_infos(const SegmentInfos &infos) {
    CheckIndexResult result;
    result.generation = infos.generation;
    result.segments_file_name = infos.generation_file_name();
    result.segments.reserve(infos.segments.size());

    for (const SegmentCommitInfo &commit_info : infos.segments) {
        SegmentCheckResult segment;
        segment.segment_name = commit_info.info.name;
        segment.max_doc = commit_info.info.max_doc;

        auto reader = SegmentReader::open(*directory_, commit_info, infos.index_created_version_major,
                                          io_context_default());
        if (!reader.has_value()) {
            segment.failures.push_back("cannot open the segment: " + reader.error().to_string());
            result.segments.push_back(std::move(segment));
            continue;
        }

        segment.num_docs = reader.value()->num_docs().value_or(0);

        // 1. The codec readers of the segment must be intact.
        expect_ok(segment.failures, "codec integrity", [&reader] { return reader.value()->check_integrity(); });

        // 2. The document counts must agree with the commit.
        const int64_t expected_docs = static_cast<int64_t>(commit_info.info.max_doc) - commit_info.total_del_count();
        if (segment.num_docs != expected_docs) {
            segment.failures.push_back("numDocs " + std::to_string(segment.num_docs) + " does not match maxDoc " +
                                       std::to_string(segment.max_doc) + " - delCount " +
                                       std::to_string(commit_info.total_del_count()) + " = " +
                                       std::to_string(expected_docs));
        }

        // 3. The live docs must describe the deletes of the commit.
        check_live_docs(segment, *reader.value(), commit_info);

        // 4. Every field must be able to serve what its FieldInfo promises.
        check_fields(segment, *reader.value());

        // 5. The stored fields of the first live document must be readable.
        check_stored_fields(segment, *reader.value());

        result.segments.push_back(std::move(segment));
    }
    return result;
}

} // namespace lucene
