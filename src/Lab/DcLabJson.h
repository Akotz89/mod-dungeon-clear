/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCLABJSON_H
#define _PLAYERBOT_DCLABJSON_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Minimal JSON for the Pull Lab: scenario files (nested, `//` and `#` line
// comments allowed so a scenario can carry its own notes) and the flat JSONL
// trace lines. DcDecisionJson's flat split-on-comma parser cannot hold either:
// scenarios nest, and trace strings (action names, details) contain commas.
//
// Engine-free (stdlib only): the same code reads a scenario in the worldserver
// and in the gtest runner.
namespace DcLabJson
{
    struct Value
    {
        enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

        Type type = Type::Null;
        bool b = false;
        double num = 0.0;
        std::string str;  // String, and the raw text of a Number
        std::vector<Value> arr;
        // Ordered so a re-serialised object is stable for diffs.
        std::map<std::string, Value> obj;

        bool IsNull() const { return type == Type::Null; }
        bool IsObject() const { return type == Type::Object; }
        bool IsArray() const { return type == Type::Array; }
        bool IsString() const { return type == Type::String; }
        bool IsNumber() const { return type == Type::Number; }
        bool IsBool() const { return type == Type::Bool; }

        // Member lookup; a shared Null for a missing key or a non-object.
        Value const& operator[](std::string const& key) const;
        bool Has(std::string const& key) const;

        double AsNumber(double def = 0.0) const;
        std::uint64_t AsU64(std::uint64_t def = 0) const;  // also accepts "0x..." strings
        std::int64_t AsI64(std::int64_t def = 0) const;
        bool AsBool(bool def = false) const;
        std::string AsString(std::string const& def = "") const;
    };

    // Parse one JSON document. On failure returns false and fills *err with
    // "line:col: message".
    bool Parse(std::string_view text, Value& out, std::string* err = nullptr);

    // Escape `s` as the body of a JSON string (no surrounding quotes).
    std::string Escape(std::string_view s);

    // Serialise (compact, object keys sorted).
    std::string Dump(Value const& v);

    // Flat-object line writer for the trace JSONL. Chainable; Str() closes it.
    class Line
    {
    public:
        Line() { _s.reserve(128); _s += '{'; }
        Line& Add(char const* key, std::string_view v);
        Line& Add(char const* key, char const* v) { return Add(key, std::string_view(v)); }
        Line& Add(char const* key, double v);
        Line& Add(char const* key, float v) { return Add(key, static_cast<double>(v)); }
        Line& Add(char const* key, std::uint64_t v);
        Line& Add(char const* key, std::int64_t v);
        Line& Add(char const* key, std::uint32_t v) { return Add(key, static_cast<std::uint64_t>(v)); }
        Line& Add(char const* key, std::int32_t v) { return Add(key, static_cast<std::int64_t>(v)); }
        Line& Add(char const* key, bool v);
        // A GUID, as a "0x..." hex string: creature GUIDs carry high-type bits
        // past 2^53 and would lose precision in any JS/JSON double reader.
        Line& Guid(char const* key, std::uint64_t g);
        std::string Str() { _s += '}'; return _s; }

    private:
        void Key(char const* key);
        std::string _s;
        bool _first = true;
    };

    std::string HexGuid(std::uint64_t g);
}

#endif  // _PLAYERBOT_DCLABJSON_H
