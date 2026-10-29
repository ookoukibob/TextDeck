// ---------------------------------------------------------------------------
// map_json.h -- minimal JSON reader for the mapping engine's declarative
// profile/config files. Written from scratch (RFC 8259 subset): objects,
// arrays, strings (with escapes), numbers, bool, null. No third-party code.
// ---------------------------------------------------------------------------
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace map {

struct JsonValue;
using JsonPtr = std::shared_ptr<JsonValue>;
using JsonObj = std::map<std::string, JsonPtr>;
using JsonArr = std::vector<JsonPtr>;

struct JsonValue {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    JsonArr     arr;
    JsonObj     obj;

    bool isNull() const   { return kind == Null; }
    bool isBool() const   { return kind == Bool; }
    bool isNumber() const { return kind == Number; }
    bool isString() const { return kind == String; }
    bool isArray() const  { return kind == Array; }
    bool isObject() const { return kind == Object; }

    const JsonPtr& at(const std::string& key) const;   // throws on missing key
    const JsonPtr& get(const std::string& key) const;  // returns null-ptr on missing
    std::string s() const;                             // string value, or "" if not a string
    double n() const;                                  // number value, or 0
    int    i() const { return (int)n(); }
    bool   flag() const { return b; }

    static JsonPtr Null_();
    static JsonPtr String_(const std::string& s);
    static JsonPtr Number_(double v);
    static JsonPtr Bool_(bool v);
    static JsonPtr Object_();
    static JsonPtr Array_();
};

// Parse a full JSON document. Returns nullptr and sets *err on failure.
JsonPtr ParseJson(const std::string& text, std::string* err = nullptr);

} // namespace map
