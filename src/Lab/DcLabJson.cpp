/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Lab/DcLabJson.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace DcLabJson
{
    namespace
    {
        Value const kNull{};

        class Parser
        {
        public:
            explicit Parser(std::string_view t) : _t(t) {}

            bool Run(Value& out, std::string* err)
            {
                SkipWs();
                if (!ParseValue(out, 0))
                    return Fail(err);
                SkipWs();
                if (_i != _t.size())
                {
                    _msg = "trailing characters";
                    return Fail(err);
                }
                return true;
            }

        private:
            bool Fail(std::string* err)
            {
                if (err)
                {
                    std::size_t line = 1, col = 1;
                    for (std::size_t k = 0; k < _i && k < _t.size(); ++k)
                    {
                        if (_t[k] == '\n')
                        {
                            ++line;
                            col = 1;
                        }
                        else
                            ++col;
                    }
                    *err = std::to_string(line) + ":" + std::to_string(col) + ": " + _msg;
                }
                return false;
            }

            void SkipWs()
            {
                while (_i < _t.size())
                {
                    char const c = _t[_i];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                        ++_i;
                    else if (c == '#' || (c == '/' && _i + 1 < _t.size() && _t[_i + 1] == '/'))
                    {
                        while (_i < _t.size() && _t[_i] != '\n')
                            ++_i;
                    }
                    else
                        break;
                }
            }

            bool ParseValue(Value& v, int depth)
            {
                if (depth > 64)
                {
                    _msg = "nesting too deep";
                    return false;
                }
                if (_i >= _t.size())
                {
                    _msg = "unexpected end";
                    return false;
                }
                char const c = _t[_i];
                if (c == '{')
                    return ParseObject(v, depth);
                if (c == '[')
                    return ParseArray(v, depth);
                if (c == '"')
                {
                    v.type = Value::Type::String;
                    return ParseString(v.str);
                }
                if (c == '-' || (c >= '0' && c <= '9'))
                    return ParseNumber(v);
                if (_t.substr(_i, 4) == "true")
                {
                    v.type = Value::Type::Bool;
                    v.b = true;
                    _i += 4;
                    return true;
                }
                if (_t.substr(_i, 5) == "false")
                {
                    v.type = Value::Type::Bool;
                    v.b = false;
                    _i += 5;
                    return true;
                }
                if (_t.substr(_i, 4) == "null")
                {
                    v.type = Value::Type::Null;
                    _i += 4;
                    return true;
                }
                _msg = std::string("unexpected '") + c + "'";
                return false;
            }

            bool ParseObject(Value& v, int depth)
            {
                v.type = Value::Type::Object;
                ++_i;
                SkipWs();
                if (_i < _t.size() && _t[_i] == '}')
                {
                    ++_i;
                    return true;
                }
                while (true)
                {
                    SkipWs();
                    if (_i >= _t.size() || _t[_i] != '"')
                    {
                        _msg = "expected object key";
                        return false;
                    }
                    std::string key;
                    if (!ParseString(key))
                        return false;
                    SkipWs();
                    if (_i >= _t.size() || _t[_i] != ':')
                    {
                        _msg = "expected ':'";
                        return false;
                    }
                    ++_i;
                    SkipWs();
                    Value child;
                    if (!ParseValue(child, depth + 1))
                        return false;
                    v.obj[key] = std::move(child);
                    SkipWs();
                    if (_i < _t.size() && _t[_i] == ',')
                    {
                        ++_i;
                        SkipWs();
                        // Tolerate a trailing comma — hand-written scenarios.
                        if (_i < _t.size() && _t[_i] == '}')
                        {
                            ++_i;
                            return true;
                        }
                        continue;
                    }
                    if (_i < _t.size() && _t[_i] == '}')
                    {
                        ++_i;
                        return true;
                    }
                    _msg = "expected ',' or '}'";
                    return false;
                }
            }

            bool ParseArray(Value& v, int depth)
            {
                v.type = Value::Type::Array;
                ++_i;
                SkipWs();
                if (_i < _t.size() && _t[_i] == ']')
                {
                    ++_i;
                    return true;
                }
                while (true)
                {
                    SkipWs();
                    Value child;
                    if (!ParseValue(child, depth + 1))
                        return false;
                    v.arr.push_back(std::move(child));
                    SkipWs();
                    if (_i < _t.size() && _t[_i] == ',')
                    {
                        ++_i;
                        SkipWs();
                        if (_i < _t.size() && _t[_i] == ']')
                        {
                            ++_i;
                            return true;
                        }
                        continue;
                    }
                    if (_i < _t.size() && _t[_i] == ']')
                    {
                        ++_i;
                        return true;
                    }
                    _msg = "expected ',' or ']'";
                    return false;
                }
            }

            static void AppendUtf8(std::string& out, unsigned cp)
            {
                if (cp < 0x80)
                    out += static_cast<char>(cp);
                else if (cp < 0x800)
                {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }

            bool ParseString(std::string& out)
            {
                ++_i;  // opening quote
                while (_i < _t.size())
                {
                    char const c = _t[_i++];
                    if (c == '"')
                        return true;
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (_i >= _t.size())
                        break;
                    char const e = _t[_i++];
                    switch (e)
                    {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u':
                        {
                            if (_i + 4 > _t.size())
                            {
                                _msg = "bad \\u escape";
                                return false;
                            }
                            unsigned cp = static_cast<unsigned>(
                                std::strtoul(std::string(_t.substr(_i, 4)).c_str(), nullptr, 16));
                            _i += 4;
                            AppendUtf8(out, cp);
                            break;
                        }
                        default:
                            _msg = "bad escape";
                            return false;
                    }
                }
                _msg = "unterminated string";
                return false;
            }

            bool ParseNumber(Value& v)
            {
                std::size_t const start = _i;
                if (_t[_i] == '-')
                    ++_i;
                while (_i < _t.size())
                {
                    char const c = _t[_i];
                    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' ||
                        c == '-')
                        ++_i;
                    else
                        break;
                }
                v.type = Value::Type::Number;
                v.str = std::string(_t.substr(start, _i - start));
                char* end = nullptr;
                v.num = std::strtod(v.str.c_str(), &end);
                if (!end || *end != '\0')
                {
                    _msg = "bad number";
                    return false;
                }
                return true;
            }

            std::string_view _t;
            std::size_t _i = 0;
            std::string _msg;
        };

        void DumpTo(Value const& v, std::string& out)
        {
            switch (v.type)
            {
                case Value::Type::Null: out += "null"; break;
                case Value::Type::Bool: out += v.b ? "true" : "false"; break;
                case Value::Type::Number:
                    if (!v.str.empty())
                        out += v.str;
                    else
                    {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%.9g", v.num);
                        out += buf;
                    }
                    break;
                case Value::Type::String:
                    out += '"';
                    out += Escape(v.str);
                    out += '"';
                    break;
                case Value::Type::Array:
                {
                    out += '[';
                    bool first = true;
                    for (Value const& c : v.arr)
                    {
                        if (!first)
                            out += ',';
                        first = false;
                        DumpTo(c, out);
                    }
                    out += ']';
                    break;
                }
                case Value::Type::Object:
                {
                    out += '{';
                    bool first = true;
                    for (auto const& [k, c] : v.obj)
                    {
                        if (!first)
                            out += ',';
                        first = false;
                        out += '"';
                        out += Escape(k);
                        out += "\":";
                        DumpTo(c, out);
                    }
                    out += '}';
                    break;
                }
            }
        }
    }

    Value const& Value::operator[](std::string const& key) const
    {
        if (type != Type::Object)
            return kNull;
        auto it = obj.find(key);
        return it == obj.end() ? kNull : it->second;
    }

    bool Value::Has(std::string const& key) const
    {
        return type == Type::Object && obj.find(key) != obj.end();
    }

    double Value::AsNumber(double def) const
    {
        if (type == Type::Number)
            return num;
        if (type == Type::Bool)
            return b ? 1.0 : 0.0;
        if (type == Type::String && !str.empty())
        {
            char* end = nullptr;
            double const d = std::strtod(str.c_str(), &end);
            if (end && *end == '\0')
                return d;
        }
        return def;
    }

    std::uint64_t Value::AsU64(std::uint64_t def) const
    {
        if (type == Type::Number)
        {
            // Integer text parses exactly; a double would round past 2^53.
            char* end = nullptr;
            unsigned long long const u = std::strtoull(str.c_str(), &end, 10);
            if (end && *end == '\0')
                return u;
            return num < 0 ? def : static_cast<std::uint64_t>(num);
        }
        if (type == Type::String && !str.empty())
        {
            char* end = nullptr;
            unsigned long long const u = std::strtoull(str.c_str(), &end, 0);
            if (end && *end == '\0')
                return u;
        }
        return def;
    }

    std::int64_t Value::AsI64(std::int64_t def) const
    {
        if (type == Type::Number)
        {
            char* end = nullptr;
            long long const i = std::strtoll(str.c_str(), &end, 10);
            if (end && *end == '\0')
                return i;
            return static_cast<std::int64_t>(num);
        }
        if (type == Type::String && !str.empty())
        {
            char* end = nullptr;
            long long const i = std::strtoll(str.c_str(), &end, 0);
            if (end && *end == '\0')
                return i;
        }
        return def;
    }

    bool Value::AsBool(bool def) const
    {
        if (type == Type::Bool)
            return b;
        if (type == Type::Number)
            return num != 0.0;
        if (type == Type::String)
        {
            if (str == "true" || str == "1" || str == "yes" || str == "on")
                return true;
            if (str == "false" || str == "0" || str == "no" || str == "off")
                return false;
        }
        return def;
    }

    std::string Value::AsString(std::string const& def) const
    {
        if (type == Type::String || type == Type::Number)
            return str;
        if (type == Type::Bool)
            return b ? "true" : "false";
        return def;
    }

    bool Parse(std::string_view text, Value& out, std::string* err)
    {
        out = Value{};
        return Parser(text).Run(out, err);
    }

    std::string Escape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 4);
        for (char const c : s)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                        out += c;
            }
        }
        return out;
    }

    std::string Dump(Value const& v)
    {
        std::string out;
        DumpTo(v, out);
        return out;
    }

    std::string HexGuid(std::uint64_t g)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(g));
        return buf;
    }

    void Line::Key(char const* key)
    {
        if (!_first)
            _s += ',';
        _first = false;
        _s += '"';
        _s += key;
        _s += "\":";
    }

    Line& Line::Add(char const* key, std::string_view v)
    {
        Key(key);
        _s += '"';
        _s += Escape(v);
        _s += '"';
        return *this;
    }

    Line& Line::Add(char const* key, double v)
    {
        Key(key);
        if (!std::isfinite(v))
        {
            _s += "null";
            return *this;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        _s += buf;
        return *this;
    }

    Line& Line::Add(char const* key, std::uint64_t v)
    {
        Key(key);
        _s += std::to_string(v);
        return *this;
    }

    Line& Line::Add(char const* key, std::int64_t v)
    {
        Key(key);
        _s += std::to_string(v);
        return *this;
    }

    Line& Line::Add(char const* key, bool v)
    {
        Key(key);
        _s += v ? "true" : "false";
        return *this;
    }

    Line& Line::Guid(char const* key, std::uint64_t g)
    {
        return Add(key, std::string_view(HexGuid(g)));
    }
}
