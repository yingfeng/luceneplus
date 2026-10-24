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

module lucene_core:geo_json_polygon.impl;

import :geo_json_polygon;

import std;

import :error;
import :geo_polygon;
import :types;

namespace lucene {

namespace {

/// One JSON value, as far as the polygon reader needs it.
///
/// Port of `JsonValue` (`core/geo/simple_geo_json_polygon_parser.rs`).
struct JsonValue {
    enum class Kind : uint8_t {
        Array,
        Bool,
        Null,
        Number,
        String,
    };

    Kind kind{Kind::Null};
    std::vector<JsonValue> array;
    bool boolean{false};
    double number{0.0};
    std::string text;

    [[nodiscard]] static JsonValue make_array(std::vector<JsonValue> items) {
        JsonValue value;
        value.kind = Kind::Array;
        value.array = std::move(items);
        return value;
    }

    [[nodiscard]] static JsonValue make_bool(bool value_in) {
        JsonValue value;
        value.kind = Kind::Bool;
        value.boolean = value_in;
        return value;
    }

    [[nodiscard]] static JsonValue make_null() { return JsonValue{}; }

    [[nodiscard]] static JsonValue make_number(double value_in) {
        JsonValue value;
        value.kind = Kind::Number;
        value.number = value_in;
        return value;
    }

    [[nodiscard]] static JsonValue make_string(std::string text_in) {
        JsonValue value;
        value.kind = Kind::String;
        value.text = std::move(text_in);
        return value;
    }

    [[nodiscard]] std::string_view type_name() const noexcept {
        switch (kind) {
            case Kind::Array:
                return "array";
            case Kind::Bool:
                return "boolean";
            case Kind::Null:
                return "null";
            case Kind::Number:
                return "number";
            case Kind::String:
                return "string";
        }
        return "null";
    }
};

} // namespace

struct GeoJsonPolygonParser::Impl {
    explicit Impl(std::string text) : input(std::move(text)) {}

    std::string input;
    size_t upto{0};
    std::optional<std::string> poly_type;
    std::optional<std::vector<JsonValue>> coordinates;

    // ---- entry point ----

    [[nodiscard]] Result<std::vector<Polygon>> parse() {
        if (auto parsed = parse_object(""); !parsed.has_value()) {
            return std::unexpected(std::move(parsed.error()));
        }
        if (auto ended = read_end(); !ended.has_value()) {
            return std::unexpected(std::move(ended.error()));
        }

        if (!coordinates.has_value()) {
            return err(new_parse_error("did not see any polygon coordinates"));
        }
        auto parsed_coordinates = std::move(coordinates.value());
        coordinates.reset();

        if (!poly_type.has_value()) {
            return err(new_parse_error("did not see type: Polygon or MultiPolygon"));
        }
        const std::string type = std::move(poly_type.value());
        poly_type.reset();

        if (type == "Polygon") {
            auto polygon = parse_polygon(parsed_coordinates);
            if (!polygon.has_value()) {
                return std::unexpected(std::move(polygon.error()));
            }
            std::vector<Polygon> polygons;
            polygons.push_back(std::move(polygon.value()));
            return polygons;
        }

        std::vector<Polygon> polygons;
        polygons.reserve(parsed_coordinates.size());
        for (const JsonValue &coordinate : parsed_coordinates) {
            if (coordinate.kind != JsonValue::Kind::Array) {
                return err(new_parse_error("elements of coordinates array should be an array, but got: " +
                                           std::string(coordinate.type_name())));
            }
            auto polygon = parse_polygon(coordinate.array);
            if (!polygon.has_value()) {
                return std::unexpected(std::move(polygon.error()));
            }
            polygons.push_back(std::move(polygon.value()));
        }
        return polygons;
    }

    // ---- object ----

    [[nodiscard]] VoidResult parse_object(const std::string &path) {
        if (auto scanned = scan_char('{'); !scanned.has_value()) {
            return scanned;
        }
        bool first = true;
        while (true) {
            auto ch = peek();
            if (!ch.has_value()) {
                return std::unexpected(std::move(ch.error()));
            }
            if (ch.value() == '}') {
                break;
            }
            if (!first) {
                if (ch.value() == ',') {
                    upto += 1;
                    auto next = peek();
                    if (!next.has_value()) {
                        return std::unexpected(std::move(next.error()));
                    }
                    if (next.value() == '}') {
                        break;
                    }
                } else {
                    return err(new_parse_error(std::string("expected , but got ") + ch.value()));
                }
            }

            first = false;

            const size_t key_start = upto;
            auto key = parse_string_literal();
            if (!key.has_value()) {
                return std::unexpected(std::move(key.error()));
            }

            if (path == "crs.properties" && key.value() == "href") {
                upto = key_start;
                return err(new_parse_error("cannot handle linked crs"));
            }

            if (auto scanned = scan_char(':'); !scanned.has_value()) {
                return scanned;
            }

            const size_t value_start = upto;
            auto ch2 = peek();
            if (!ch2.has_value()) {
                return std::unexpected(std::move(ch2.error()));
            }

            std::string child_path;
            if (path.empty()) {
                child_path = key.value();
            } else {
                child_path = path + "." + key.value();
            }

            JsonValue value;
            const char current = ch2.value();
            if (current == '[') {
                auto parsed = parse_array(child_path);
                if (!parsed.has_value()) {
                    return std::unexpected(std::move(parsed.error()));
                }
                value = std::move(parsed.value());
            } else if (current == '{') {
                if (auto parsed = parse_object(child_path); !parsed.has_value()) {
                    return std::unexpected(std::move(parsed.error()));
                }
                value = JsonValue::make_null();
            } else if (current == '"') {
                auto parsed = parse_string_literal();
                if (!parsed.has_value()) {
                    return std::unexpected(std::move(parsed.error()));
                }
                value = JsonValue::make_string(std::move(parsed.value()));
            } else if (current == 't') {
                if (auto scanned = scan_string("true"); !scanned.has_value()) {
                    return scanned;
                }
                value = JsonValue::make_bool(true);
            } else if (current == 'f') {
                if (auto scanned = scan_string("false"); !scanned.has_value()) {
                    return scanned;
                }
                value = JsonValue::make_bool(false);
            } else if (current == 'n') {
                if (auto scanned = scan_string("null"); !scanned.has_value()) {
                    return scanned;
                }
                value = JsonValue::make_null();
            } else if (current == '-' || current == '.' || (current >= '0' && current <= '9')) {
                auto parsed = parse_number();
                if (!parsed.has_value()) {
                    return std::unexpected(std::move(parsed.error()));
                }
                value = JsonValue::make_number(parsed.value());
            } else if (current == '}') {
                break;
            } else {
                return err(new_parse_error(std::string("expected array, object, string or literal value, but got: ") +
                                           current));
            }

            if (path == "crs.properties" && key.value() == "name") {
                if (value.kind != JsonValue::Kind::String) {
                    upto = value_start;
                    return err(new_parse_error("crs.properties.name should be a string, but saw: " +
                                               std::string(value.type_name())));
                }
                const std::string &crs = value.text;
                const bool ogc = crs.rfind("urn:ogc:def:crs:OGC", 0) == 0;
                const bool crs84 = crs.size() >= 6 && crs.compare(crs.size() - 6, 6, ":CRS84") == 0;
                if (!ogc || !crs84) {
                    upto = value_start;
                    return err(new_parse_error("crs must be CRS84 from OGC, but saw: " + crs));
                }
            }

            if (key.value() == "type" && path.rfind("crs", 0) != 0) {
                if (value.kind != JsonValue::Kind::String) {
                    upto = value_start;
                    return err(new_parse_error("type should be a string, but got: " +
                                               std::string(value.type_name())));
                }
                const std::string &type_name = value.text;
                if (type_name == "Polygon" && is_valid_geometry_path(path)) {
                    poly_type = "Polygon";
                } else if (type_name == "MultiPolygon" && is_valid_geometry_path(path)) {
                    poly_type = "MultiPolygon";
                } else if ((type_name == "FeatureCollection" || type_name == "Feature") &&
                           (path == "features.[]" || path.empty())) {
                    // Accepted, but there is nothing to record.
                } else {
                    upto = value_start;
                    return err(new_parse_error(
                            "can only handle type FeatureCollection (if it has a single polygon geometry), Feature, "
                            "Polygon or MultiPolygon, but got " +
                            type_name));
                }
            } else if (key.value() == "coordinates" && is_valid_geometry_path(path)) {
                if (value.kind != JsonValue::Kind::Array) {
                    upto = value_start;
                    return err(new_parse_error("coordinates should be an array, but got: " +
                                               std::string(value.type_name())));
                }
                if (coordinates.has_value()) {
                    upto = value_start;
                    return err(new_parse_error("only one Polygon or MultiPolygon is supported"));
                }
                coordinates = std::move(value.array);
            }
        }

        return scan_char('}');
    }

    [[nodiscard]] static bool is_valid_geometry_path(std::string_view path) {
        return path.empty() || path == "geometry" || path == "features.[].geometry";
    }

    // ---- geometry ----

    [[nodiscard]] Result<Polygon> parse_polygon(const std::vector<JsonValue> &polygon_coordinates) {
        if (polygon_coordinates.empty()) {
            return err(new_parse_error("polygon array must not be empty"));
        }

        const JsonValue &first = polygon_coordinates.front();
        if (first.kind != JsonValue::Kind::Array) {
            return err(new_parse_error(
                    "first element of polygon array must be an array [[lat, lon], [lat, lon] ...] but got: " +
                    std::string(first.type_name())));
        }
        auto shell = parse_points(first.array);
        if (!shell.has_value()) {
            return std::unexpected(std::move(shell.error()));
        }

        std::vector<Polygon> holes;
        holes.reserve(polygon_coordinates.size() - 1);
        for (size_t i = 1; i < polygon_coordinates.size(); ++i) {
            const JsonValue &coordinate = polygon_coordinates[i];
            if (coordinate.kind != JsonValue::Kind::Array) {
                return err(new_parse_error(
                        "elements of coordinates array must be an array [[lat, lon], [lat, lon] ...] but got: " +
                        std::string(coordinate.type_name())));
            }
            auto hole_points = parse_points(coordinate.array);
            if (!hole_points.has_value()) {
                return std::unexpected(std::move(hole_points.error()));
            }
            auto hole = Polygon::create(std::move(hole_points.value().first), std::move(hole_points.value().second));
            if (!hole.has_value()) {
                return std::unexpected(std::move(hole.error()));
            }
            holes.push_back(std::move(hole.value()));
        }

        return Polygon::create(std::move(shell.value().first), std::move(shell.value().second), std::move(holes));
    }

    /// The `(lats, lons)` of a coordinate array; each element is `[lon, lat]`.
    [[nodiscard]] Result<std::pair<std::vector<double>, std::vector<double>>> parse_points(
            const std::vector<JsonValue> &values) {
        std::vector<double> lats;
        std::vector<double> lons;
        lats.reserve(values.size());
        lons.reserve(values.size());

        for (const JsonValue &point : values) {
            if (point.kind != JsonValue::Kind::Array) {
                return err(new_parse_error("elements of coordinates array must [lat, lon] array, but got: " +
                                           std::string(point.type_name())));
            }
            if (point.array.size() != 2) {
                return err(new_parse_error(
                        "elements of coordinates array must [lat, lon] array, but got wrong element count: " +
                        std::to_string(point.array.size())));
            }
            if (point.array[0].kind != JsonValue::Kind::Number) {
                return err(new_parse_error(
                        "elements of coordinates array must [lat, lon] array, but first element is not a Double: " +
                        std::string(point.array[0].type_name())));
            }
            if (point.array[1].kind != JsonValue::Kind::Number) {
                return err(new_parse_error(
                        "elements of coordinates array must [lat, lon] array, but second element is not a Double: " +
                        std::string(point.array[1].type_name())));
            }
            lons.push_back(point.array[0].number);
            lats.push_back(point.array[1].number);
        }

        return std::pair<std::vector<double>, std::vector<double>>(std::move(lats), std::move(lons));
    }

    // ---- arrays and scalars ----

    [[nodiscard]] Result<JsonValue> parse_array(const std::string &path) {
        std::vector<JsonValue> result;
        std::optional<std::string> child_path;
        if (auto scanned = scan_char('['); !scanned.has_value()) {
            return std::unexpected(std::move(scanned.error()));
        }
        while (upto < input.size()) {
            auto ch = peek();
            if (!ch.has_value()) {
                return std::unexpected(std::move(ch.error()));
            }
            if (ch.value() == ']') {
                if (auto scanned = scan_char(']'); !scanned.has_value()) {
                    return std::unexpected(std::move(scanned.error()));
                }
                return JsonValue::make_array(std::move(result));
            }

            if (!result.empty()) {
                if (ch.value() != ',') {
                    return err(new_parse_error(std::string("expected ',' separating list items, but got '") +
                                               ch.value() + "'"));
                }
                upto += 1;
                if (upto == input.size()) {
                    return err(new_parse_error("hit EOF while parsing array"));
                }
            }

            auto next = peek();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            const char current = next.value();
            JsonValue value;
            if (current == '[') {
                if (!child_path.has_value()) {
                    child_path = path + ".[]";
                }
                auto parsed = parse_array(child_path.value());
                if (!parsed.has_value()) {
                    return std::unexpected(std::move(parsed.error()));
                }
                value = std::move(parsed.value());
            } else if (current == '{') {
                if (!child_path.has_value()) {
                    child_path = path + ".[]";
                }
                if (auto parsed = parse_object(child_path.value()); !parsed.has_value()) {
                    return std::unexpected(std::move(parsed.error()));
                }
                value = JsonValue::make_null();
            } else if (current == '-' || current == '.' || (current >= '0' && current <= '9')) {
                auto parsed = parse_number();
                if (!parsed.has_value()) {
                    return std::unexpected(std::move(parsed.error()));
                }
                value = JsonValue::make_number(parsed.value());
            } else if (current == '"') {
                auto parsed = parse_string_literal();
                if (!parsed.has_value()) {
                    return std::unexpected(std::move(parsed.error()));
                }
                value = JsonValue::make_string(std::move(parsed.value()));
            } else {
                return err(new_parse_error(std::string("expected another array or number while parsing array, not '") +
                                           current + "'"));
            }

            result.push_back(std::move(value));
        }

        return err(new_parse_error("hit EOF while reading array"));
    }

    [[nodiscard]] Result<double> parse_number() {
        const size_t start = upto;
        while (upto < input.size()) {
            const char ch = input[upto];
            if (ch == '-' || ch == '.' || (ch >= '0' && ch <= '9') || ch == 'e' || ch == 'E') {
                upto += 1;
            } else {
                break;
            }
        }

        const std::string text = input.substr(start, upto - start);
        char *end_of_number = nullptr;
        const double value = std::strtod(text.c_str(), &end_of_number);
        if (end_of_number == nullptr || static_cast<size_t>(end_of_number - text.c_str()) != text.size()) {
            upto = start;
            return err(new_parse_error("could not parse number as double"));
        }
        return value;
    }

    [[nodiscard]] Result<std::string> parse_string_literal() {
        if (auto scanned = scan_char('"'); !scanned.has_value()) {
            return std::unexpected(std::move(scanned.error()));
        }
        std::string result;
        while (upto < input.size()) {
            const char ch = input[upto];
            if (ch == '"') {
                upto += 1;
                return result;
            }

            if (ch == '\\') {
                upto += 1;
                if (upto == input.size()) {
                    return err(new_parse_error("hit EOF inside string literal"));
                }

                const char escaped = input[upto];
                if (escaped == 'u') {
                    upto += 1;
                    if (upto + 4 > input.size()) {
                        return err(new_parse_error("hit EOF inside string literal"));
                    }
                    uint32_t codepoint = 0;
                    for (size_t i = 0; i < 4; ++i) {
                        const char digit = input[upto + i];
                        uint32_t value = 0;
                        if (digit >= '0' && digit <= '9') {
                            value = static_cast<uint32_t>(digit - '0');
                        } else if (digit >= 'a' && digit <= 'f') {
                            value = static_cast<uint32_t>(digit - 'a') + 10U;
                        } else if (digit >= 'A' && digit <= 'F') {
                            value = static_cast<uint32_t>(digit - 'A') + 10U;
                        } else {
                            return err(new_parse_error("invalid unicode escape in string literal"));
                        }
                        codepoint = codepoint * 16U + value;
                    }
                    // `char::from_u32` rejects the surrogate range and anything
                    // beyond the code space.
                    if (codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
                        return err(new_parse_error("invalid unicode escape in string literal"));
                    }
                    append_utf8(result, codepoint);
                    upto += 4;
                } else if (escaped == '\\') {
                    result.push_back('\\');
                    upto += 1;
                } else {
                    return err(new_parse_error(std::string("unsupported string escape character \\") + escaped));
                }
            } else {
                result.push_back(ch);
                upto += 1;
            }
        }

        return err(new_parse_error("hit EOF inside string literal"));
    }

    static void append_utf8(std::string &out, uint32_t codepoint) {
        if (codepoint < 0x80U) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    // ---- scanning ----

    [[nodiscard]] Result<char> peek() {
        while (upto < input.size()) {
            const char ch = input[upto];
            if (is_json_whitespace(ch)) {
                upto += 1;
                continue;
            }
            return ch;
        }
        return err(new_parse_error("unexpected EOF"));
    }

    [[nodiscard]] VoidResult scan_char(char expected) {
        while (upto < input.size()) {
            const char ch = input[upto];
            if (is_json_whitespace(ch)) {
                upto += 1;
                continue;
            }
            if (ch != expected) {
                return err(new_parse_error(std::string("expected '") + expected + "' but got '" + ch + "'"));
            }
            upto += 1;
            return {};
        }
        return err(new_parse_error(std::string("expected '") + expected + "' but got EOF"));
    }

    [[nodiscard]] VoidResult scan_string(std::string_view expected) {
        if (upto + expected.size() > input.size()) {
            return err(new_parse_error("expected \"" + std::string(expected) + "\" but hit EOF"));
        }
        const std::string_view substring(input.data() + upto, expected.size());
        if (substring != expected) {
            return err(new_parse_error("expected \"" + std::string(expected) + "\" but got \"" +
                                       std::string(substring) + "\""));
        }
        upto += expected.size();
        return {};
    }

    [[nodiscard]] VoidResult read_end() {
        while (upto < input.size()) {
            const char ch = input[upto];
            if (!is_json_whitespace(ch)) {
                return err(new_parse_error(std::string("unexpected character '") + ch +
                                           "' after end of GeoJSON object"));
            }
            upto += 1;
        }
        return {};
    }

    [[nodiscard]] static bool is_json_whitespace(char ch) noexcept {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    }

    [[nodiscard]] LuceneError new_parse_error(std::string_view details) const {
        const size_t end = std::min(input.size(), upto + 1);
        std::string fragment;
        if (upto < 50) {
            fragment = input.substr(0, end);
        } else {
            fragment = "..." + input.substr(upto - 50, end - (upto - 50));
        }
        return LuceneError::illegal_state(std::string(details) + " at character offset " + std::to_string(upto) +
                                          "; fragment leading to this:\n" + fragment);
    }
};

GeoJsonPolygonParser::GeoJsonPolygonParser(std::string input) : impl_(std::make_unique<Impl>(std::move(input))) {}

GeoJsonPolygonParser::~GeoJsonPolygonParser() = default;

GeoJsonPolygonParser::GeoJsonPolygonParser(GeoJsonPolygonParser &&) noexcept = default;

GeoJsonPolygonParser &GeoJsonPolygonParser::operator=(GeoJsonPolygonParser &&) noexcept = default;

Result<std::vector<Polygon>> GeoJsonPolygonParser::parse() { return impl_->parse(); }

Result<std::vector<Polygon>> polygons_from_geo_json(std::string_view geo_json) {
    std::string text(geo_json);
    GeoJsonPolygonParser parser(std::move(text));
    return parser.parse();
}

} // namespace lucene
