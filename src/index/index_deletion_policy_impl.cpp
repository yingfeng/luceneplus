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

module lucene_core:index_deletion_policy.impl;

import :index_deletion_policy;

import std;

import :error;
import :index_commit;
import :types;

namespace lucene {

VoidResult KeepOnlyLastCommitDeletionPolicy::on_commit(std::span<const std::shared_ptr<IndexCommit>> commits) {
    if (commits.empty()) {
        return {};
    }
    // The last commit is the current state of the index, so everything before it
    // goes.
    for (size_t i = 0; i + 1 < commits.size(); ++i) {
        if (commits[i] == nullptr) {
            return err(LuceneError::illegal_argument("the commit points must not be null"));
        }
        if (auto deleted = commits[i]->delete_commit(); !deleted.has_value()) {
            return deleted;
        }
    }
    return {};
}

} // namespace lucene
