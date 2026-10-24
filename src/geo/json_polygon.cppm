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

export module lucene_core:geo_json_polygon;

import :types;
import std;

import :error;
import :geo_polygon;

namespace lucene {

/// A deliberately small GeoJSON reader that only understands polygons.
///
/// Port of `SimpleGeoJSONPolygonParser`
/// (`core/geo/simple_geo_json_polygon_parser.rs`).
///
/// It accepts a `Polygon`, a `MultiPolygon`, or a `Feature`/`FeatureCollection`
/// that holds exactly one of those, and rejects everything else: no linked CRS,
/// only one geometry, and coordinates must be `[lon, lat]` pairs.  A polygon is
/// built from the first coordinate array, its holes from the remaining ones.
///
/// The reader is hand written (like the reference implementation's) rather than
/// built on a JSON library, so the port keeps the same error messages and the
/// same notion of a "geometry path".
export class GeoJsonPolygonParser final {
public:
    /// A parser over `input`, which it copies.
    explicit GeoJsonPolygonParser(std::string input);
    ~GeoJsonPolygonParser();

    GeoJsonPolygonParser(GeoJsonPolygonParser &&) noexcept;
    GeoJsonPolygonParser &operator=(GeoJsonPolygonParser &&) noexcept;
    GeoJsonPolygonParser(const GeoJsonPolygonParser &) = delete;
    GeoJsonPolygonParser &operator=(const GeoJsonPolygonParser &) = delete;

    /// The polygons of the geometry; fails with `illegal_state` when the input is
    /// not a single polygon geometry.
    [[nodiscard]] Result<std::vector<Polygon>> parse();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Port of `from_geo_json` (`core/geo/polygon.rs`).
///
/// **Deviation:** it is a free function here, not
/// `Polygon::from_geo_json`, because the polygon shape module cannot import this
/// one without forming a cycle.
[[nodiscard]] Result<std::vector<Polygon>> polygons_from_geo_json(std::string_view geo_json);

} // namespace lucene
