// map_json.cpp -- minimal JSON parser implementation (see map_json.h).
#include "map_json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace map {

const JsonPtr& JsonValue::at(const std::string& key) const {
    auto it = obj.find(key);
    if (it == obj.end()) throw std::runtime_error("json: missing key '" + key + "'");
    return it->second;
}
const JsonPtr& JsonValue::get(const std::string& key) const {
    static const JsonPtr kNull = JsonValue::Null_();
    auto it = obj.find(key);
    return it == obj.end() ? kNull : it->second;
}
std::string JsonValue::s() const { return kind == String ? str : std::string(); }
double JsonValue::n() const { return kind == Number ? num : 0.0; }

JsonPtr JsonValue::Null_()   { auto v = std::make_shared<JsonValue>(); v->kind = Null;   return v; }
JsonPtr JsonValue::String_(const std::string& s) { auto v = std::make_shared<JsonValue>(); v->kind = String; v->str = s; return v; }
JsonPtr JsonValue::Number_(double x) { auto v = std::make_shared<JsonValue>(); v->kind = Number; v->num = x; return v; }
JsonPtr JsonValue::Bool_(bool b) { auto v = std::make_shared<JsonValue>(); v->kind = Bool; v->b = b; return v; }
JsonPtr JsonValue::Object_() { auto v = std::make_shared<JsonValue>(); v->kind = Object; return v; }
JsonPtr JsonValue::Array_()  { auto v = std::make_shared<JsonValue>(); v->kind = Array;  return v; }

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string& t) : s(t) {}

    void skipWs() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }

    JsonPtr parse() {
        skipWs();
        JsonPtr v = parseValue();
        skipWs();
        if (err.empty() && i != s.size()) { err = "trailing characters"; return nullptr; }
        return v;
    }

    JsonPtr parseValue() {
        skipWs();
        if (i >= s.size()) { err = "unexpected end"; return nullptr; }
        char c = s[i];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return JsonValue::String_(parseString());
        if (c == 't') { if (expect("true"))  return JsonValue::Bool_(true);  return fail("bad literal"); }
        if (c == 'f') { if (expect("false")) return JsonValue::Bool_(false); return fail("bad literal"); }
        if (c == 'n') { if (expect("null"))  return JsonValue::Null_();     return fail("bad literal"); }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        fail(std::string("unexpected char '") + c + "'");
        return nullptr;
    }

    bool expect(const char* lit) {
        size_t n = 0; while (lit[n]) ++n;
        if (s.compare(i, n, lit) == 0) { i += n; return true; }
        return false;
    }
    JsonPtr fail(const std::string& m) { if (err.empty()) err = m + " @ " + std::to_string(i); return nullptr; }

    JsonPtr parseObject() {
        auto o = JsonValue::Object_();
        ++i; // '{'
        skipWs();
        if (i < s.size() && s[i] == '}') { ++i; return o; }
        while (true) {
            skipWs();
            if (i >= s.size() || s[i] != '"') return fail("expected string key");
            std::string key = parseString();
            skipWs();
            if (i >= s.size() || s[i] != ':') return fail("expected ':'");
            ++i;
            JsonPtr v = parseValue();
            if (!v) return nullptr;
            o->obj[key] = v;
            skipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return o; }
            return fail("expected ',' or '}'");
        }
    }

    JsonPtr parseArray() {
        auto a = JsonValue::Array_();
        ++i; // '['
        skipWs();
        if (i < s.size() && s[i] == ']') { ++i; return a; }
        while (true) {
            JsonPtr v = parseValue();
            if (!v) return nullptr;
            a->arr.push_back(v);
            skipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return a; }
            return fail("expected ',' or ']'");
        }
    }

    std::string parseString() {
        std::string out;
        ++i; // '"'
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') { ++i; return out; }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) break;
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) { err = "bad \\u"; return out; }
                        unsigned code = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i + k];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else { err = "bad hex"; return out; }
                        }
                        i += 4;
                        // UTF-8 encode the code point
                        if (code < 0x80) out += (char)code;
                        else if (code < 0x800) { out += (char)(0xC0 | (code >> 6)); out += (char)(0x80 | (code & 0x3F)); }
                        else { out += (char)(0xE0 | (code >> 12)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F)); }
                        break;
                    }
                    default: err = "bad escape"; return out;
                }
            } else {
                out += c; ++i;
            }
        }
        err = "unterminated string";
        return out;
    }

    JsonPtr parseNumber() {
        size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
        if (i < s.size() && s[i] == '.') { ++i; while (i < s.size() && isdigit((unsigned char)s[i])) ++i; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
        }
        double v = strtod(s.substr(start, i - start).c_str(), nullptr);
        return JsonValue::Number_(v);
    }
};

} // namespace

JsonPtr ParseJson(const std::string& text, std::string* err) {
    Parser p(text);
    JsonPtr v = p.parse();
    if (!v && err) *err = p.err;
    return v;
}

} // namespace map
