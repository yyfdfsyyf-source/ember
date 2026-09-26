#pragma once
// minijson - minimal, dependency-free JSON parse/serialize.
// Strings are stored/emitted as UTF-8. Order of object keys is preserved.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mini {

struct Value {
  enum Type { Null, Bool, Int, Double, String, Array, Object };
  Type type = Null;
  bool b = false;
  int64_t i = 0;
  double d = 0.0;
  std::string s;
  std::vector<Value> arr;
  std::vector<std::pair<std::string, Value>> obj;

  static Value makeNull() { return Value{}; }
  static Value makeBool(bool v) { Value x; x.type = Bool; x.b = v; return x; }
  static Value makeInt(int64_t v) { Value x; x.type = Int; x.i = v; return x; }
  static Value makeDouble(double v) { Value x; x.type = Double; x.d = v; return x; }
  static Value makeString(std::string v) { Value x; x.type = String; x.s = std::move(v); return x; }
  static Value makeArray() { Value x; x.type = Array; return x; }
  static Value makeObject() { Value x; x.type = Object; return x; }

  // --- object helpers -----------------------------------------------------
  Value& set(std::string const& key, Value v) {
    for (auto& kv : obj) {
      if (kv.first == key) { kv.second = std::move(v); return kv.second; }
    }
    obj.emplace_back(key, std::move(v));
    return obj.back().second;
  }
  Value* get(char const* key) {
    for (auto& kv : obj)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  Value const* get(char const* key) const {
    for (auto const& kv : obj)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  bool has(char const* key) const { return get(key) != nullptr; }

  // --- typed accessors (safe defaults) -----------------------------------
  int64_t asInt(int64_t def = 0) const {
    switch (type) {
      case Int: return i;
      case Double: return (int64_t)d;
      case Bool: return b ? 1 : 0;
      default: return def;
    }
  }
  double asDouble(double def = 0) const {
    switch (type) {
      case Double: return d;
      case Int: return (double)i;
      default: return def;
    }
  }
  bool asBool(bool def = false) const {
    switch (type) {
      case Bool: return b;
      case Int: return i != 0;
      default: return def;
    }
  }
  std::string asString(std::string const& def = std::string()) const {
    return type == String ? s : def;
  }
  Value const* at(size_t idx) const { return idx < arr.size() ? &arr[idx] : nullptr; }
  size_t size() const {
    if (type == Array) return arr.size();
    if (type == Object) return obj.size();
    return 0;
  }
};

// --- escaping --------------------------------------------------------------
inline void escapeInto(std::string& out, std::string const& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  out += '"';
}

// --- serialize -------------------------------------------------------------
inline void dumpInto(std::string& out, Value const& v) {
  switch (v.type) {
    case Value::Null: out += "null"; break;
    case Value::Bool: out += v.b ? "true" : "false"; break;
    case Value::Int: {
      char buf[24];
      std::snprintf(buf, sizeof buf, "%lld", (long long)v.i);
      out += buf;
      break;
    }
    case Value::Double: {
      char buf[40];
      if (std::isfinite(v.d)) std::snprintf(buf, sizeof buf, "%.17g", v.d);
      else std::snprintf(buf, sizeof buf, "null");
      out += buf;
      break;
    }
    case Value::String: escapeInto(out, v.s); break;
    case Value::Array: {
      out += '[';
      for (size_t i = 0; i < v.arr.size(); i++) {
        if (i) out += ',';
        dumpInto(out, v.arr[i]);
      }
      out += ']';
      break;
    }
    case Value::Object: {
      out += '{';
      for (size_t i = 0; i < v.obj.size(); i++) {
        if (i) out += ',';
        escapeInto(out, v.obj[i].first);
        out += ':';
        dumpInto(out, v.obj[i].second);
      }
      out += '}';
      break;
    }
  }
}

inline std::string dump(Value const& v) {
  std::string out;
  out.reserve(256);
  dumpInto(out, v);
  return out;
}

// --- parse -----------------------------------------------------------------
namespace detail {

struct Parser {
  char const* p;
  char const* end;

  Parser(char const* s, size_t n) : p(s), end(s + n) {}

  void error(char const* msg) {
    throw std::runtime_error(msg);
  }

  void skipWs() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }

  bool eat(char c) {
    if (p < end && *p == c) { p++; return true; }
    return false;
  }

  void expect(char c, char const* msg) {
    if (!eat(c)) error(msg);
  }

  bool expectLiteral(char const* lit, size_t n) {
    if (p + n <= end && std::memcmp(p, lit, n) == 0) { p += n; return true; }
    error("bad literal");
    return false;
  }

  uint32_t hex4() {
    if (p + 4 > end) error("bad \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
      else error("bad hex digit");
    }
    return v;
  }

  void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += (char)cp;
    } else if (cp < 0x800) {
      out += (char)(0xC0 | (cp >> 6));
      out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | (cp >> 18));
      out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
  }

  std::string parseString() {
    expect('"', "expected string");
    std::string out;
    while (p < end) {
      unsigned char c = (unsigned char)*p++;
      if (c == '"') return out;
      if (c == '\\') {
        if (p >= end) error("bad escape");
        unsigned char e = (unsigned char)*p++;
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
            uint32_t cp = hex4();
            if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end && *p == '\\' && *(p + 1) == 'u') {
              p += 2;
              uint32_t lo = hex4();
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                appendUtf8(out, cp);
                cp = lo;
              }
            }
            appendUtf8(out, cp);
            break;
          }
          default: error("bad escape char");
        }
      } else if (c < 0x20) {
        error("control char in string");
      } else {
        out += (char)c;
      }
    }
    error("unterminated string");
    return out;
  }

  Value parseNumber() {
    bool neg = false;
    if (eat('-')) neg = true;
    if (p >= end || !(*p >= '0' && *p <= '9')) error("bad number");
    bool isDouble = false;
    int64_t intPart = 0;
    bool anyDigit = false;
    while (p < end && *p >= '0' && *p <= '9') {
      intPart = intPart * 10 + (*p - '0');
      p++;
      anyDigit = true;
    }
    double frac = 0;
    if (eat('.')) {
      isDouble = true;
      double scale = 0.1;
      while (p < end && *p >= '0' && *p <= '9') {
        frac += (*p - '0') * scale;
        scale *= 0.1;
        p++;
        anyDigit = true;
      }
    }
    int exp = 0;
    bool expNeg = false;
    if (p < end && (*p == 'e' || *p == 'E')) {
      p++;
      isDouble = true;
      if (p < end && (*p == '+' || *p == '-')) { expNeg = (*p == '-'); p++; }
      while (p < end && *p >= '0' && *p <= '9') {
        exp = exp * 10 + (*p - '0');
        p++;
      }
    }
    (void)anyDigit;
    double val = (double)intPart + frac;
    if (exp) val *= std::pow(10.0, expNeg ? -exp : exp);
    if (neg) val = -val;
    if (!isDouble) {
      Value v = Value::makeInt(neg ? -intPart : intPart);
      return v;
    }
    return Value::makeDouble(val);
  }

  Value parseValue() {
    skipWs();
    if (p >= end) error("unexpected end");
    char c = *p;
    if (c == '{') return parseObject();
    if (c == '[') return parseArray();
    if (c == '"') { Value v = Value::makeString(parseString()); return v; }
    if (c == 't') { expectLiteral("true", 4); return Value::makeBool(true); }
    if (c == 'f') { expectLiteral("false", 5); return Value::makeBool(false); }
    if (c == 'n') { expectLiteral("null", 4); return Value::makeNull(); }
    return parseNumber();
  }

  Value parseArray() {
    expect('[', "expected [");
    Value arr = Value::makeArray();
    skipWs();
    if (eat(']')) return arr;
    while (true) {
      arr.arr.push_back(parseValue());
      skipWs();
      if (eat(']')) return arr;
      expect(',', "expected , or ] in array");
      skipWs();
    }
  }

  Value parseObject() {
    expect('{', "expected {");
    Value obj = Value::makeObject();
    skipWs();
    if (eat('}')) return obj;
    while (true) {
      skipWs();
      std::string key = parseString();
      skipWs();
      expect(':', "expected :");
      obj.obj.emplace_back(std::move(key), parseValue());
      skipWs();
      if (eat('}')) return obj;
      expect(',', "expected , or } in object");
    }
  }
};

}  // namespace detail

inline Value parse(char const* text, size_t len) {
  detail::Parser ps(text, len);
  Value v = ps.parseValue();
  ps.skipWs();
  if (ps.p != ps.end) throw std::runtime_error("trailing data after json value");
  return v;
}

inline Value parse(std::string const& text) { return parse(text.data(), text.size()); }

inline bool tryParse(std::string const& text, Value& out) {
  try {
    out = parse(text);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace mini
