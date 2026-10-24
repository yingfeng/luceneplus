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

export module lucene_core:geo_geometry;

import :types;
import std;

import :error;
import :geo_component2d;

namespace lucene {

/// A geometric shape that can be turned into a [`Component2D`].
///
/// Port of the `Geometry` trait (`core/geo/geometry.rs`).
///
/// The two marker subclasses below say which coordinate system a shape lives in:
/// a [`LatLonGeometry`] is a shape on the globe (degrees, haversine distances),
/// an [`XYGeometry`] one in the cartesian plane.
export class Geometry {
public:
    virtual ~Geometry() = default;

    /// This shape as a [`Component2D`], for the point-index machinery.
    ///
    /// **Deviations:** the reference implementation returns the concrete component
    /// type (and wraps multi-shape shapes in a `Component2DEnum`); the port always
    /// hands out the [`Component2D`] interface, because the shapes here are never
    /// inspected again after the conversion.  The pointer is shared, because a
    /// shape that needs a component tree hands ownership to the tree.
    [[nodiscard]] virtual Result<std::shared_ptr<Component2D>> to_component2d() const = 0;
};

/// A shape on the globe.
///
/// Port of the `LatLonGeometry` trait (`core/geo/lat_lon_geometry.rs`).
export class LatLonGeometry : public Geometry {};

/// A shape in the cartesian plane.
///
/// Port of the `XYGeometry` trait (`core/geo/xy_geometry.rs`).
export class XYGeometry : public Geometry {};

/// Turns `geometries` into one component: a single shape is used as it is, several
/// are wrapped in a [`ComponentTree`](:component_tree).
///
/// Port of `lat_lon_geometry::create` / `xy_geometry::create`
/// (`core/geo/lat_lon_geometry.rs`, `core/geo/xy_geometry.rs`).
[[nodiscard]] Result<std::shared_ptr<Component2D>> create_geometry_component(
        const std::vector<const Geometry *> &geometries);

} // namespace lucene
