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

module lucene_core:geo_geometry.impl;

import :geo_geometry;

import std;

import :component_tree;
import :error;
import :geo_component2d;

namespace lucene {

Result<std::shared_ptr<Component2D>> create_geometry_component(const std::vector<const Geometry *> &geometries) {
    if (geometries.empty()) {
        return err(LuceneError::illegal_argument("geometries must not be empty"));
    }

    if (geometries.size() == 1) {
        if (geometries[0] == nullptr) {
            return err(LuceneError::illegal_argument("the geometry must not be null"));
        }
        return geometries[0]->to_component2d();
    }

    std::vector<std::shared_ptr<Component2D>> components;
    components.reserve(geometries.size());
    for (const Geometry *geometry : geometries) {
        if (geometry == nullptr) {
            return err(LuceneError::illegal_argument("the geometries must not be null"));
        }
        auto component = geometry->to_component2d();
        if (!component.has_value()) {
            return std::unexpected(std::move(component.error()));
        }
        components.push_back(std::move(component.value()));
    }

    auto tree = ComponentTree::create(std::move(components));
    if (!tree.has_value()) {
        return std::unexpected(std::move(tree.error()));
    }
    return std::shared_ptr<Component2D>(std::move(tree.value()));
}

} // namespace lucene
