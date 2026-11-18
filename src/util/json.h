#pragma once
// A small JSON reader and writer (enough for the connector protocol). Not a general-purpose library:
// numbers are doubles, depth is limited, and input larger than 8 MB is refused.
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wezt::json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  Value() = default;
  static Value null() { return Value(); }
  static Value boolean(bool b) { Value v; v.t_ = Type::Bool; v.b_ = b; return v; }
  static Value number(double d) { Value v; v.t_ = Type::Number; v.n_ = d; return v; }
  static Value string(std::string s) { Value v; v.t_ = Type::String; v.s_ = std::move(s); return v; }
  static Value array() { Value v; v.t_ = Type::Array; return v; }
  static Value object() { Value v; v.t_ = Type::Object; return v; }

  Type type() const { return t_; }
  bool isNull() const { return t_ == Type::Null; }
  bool isString() const { return t_ == Type::String; }
  bool isNumber() const { return t_ == Type::Number; }
  bool isBool() const { return t_ == Type::Bool; }
  bool isArray() const { return t_ == Type::Array; }
  bool isObject() const { return t_ == Type::Object; }

  const std::string& str() const { return s_; }
  double num() const { return n_; }
  bool boolean() const { return b_; }
  std::vector<Value>& items() { return a_; }
  const std::vector<Value>& items() const { return a_; }
  std::map<std::string, Value>& members() { return o_; }
  const std::map<std::string, Value>& members() const { return o_; }

  // Lookup helpers: return the default when the key is missing or has another type.
  const Value* get(const std::string& k) const { auto it = o_.find(k); return t_ == Type::Object && it != o_.end() ? &it->second : nullptr; }
  std::string getString(const std::string& k, const std::string& d = {}) const { auto* v = get(k); return v && v->isString() ? v->s_ : d; }
  double getNumber(const std::string& k, double d = 0) const { auto* v = get(k); return v && v->isNumber() ? v->n_ : d; }
  bool getBool(const std::string& k, bool d = false) const { auto* v = get(k); return v && v->isBool() ? v->b_ : d; }

  Value& set(const std::string& k, Value v) { t_ = Type::Object; o_[k] = std::move(v); return *this; }
  Value& push(Value v) { t_ = Type::Array; a_.push_back(std::move(v)); return *this; }

  std::string dump() const;

 private:
  Type t_ = Type::Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<Value> a_;
  std::map<std::string, Value> o_;
};

inline void escapeInto(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; } else out += static_cast<char>(c);
    }
  }
  out += '"';
}

inline std::string Value::dump() const {
  std::string o;
  switch (t_) {
    case Type::Null: o = "null"; break;
    case Type::Bool: o = b_ ? "true" : "false"; break;
    case Type::Number: {
      if (!std::isfinite(n_)) { o = "null"; break; }
      char b[40];
      if (n_ == std::floor(n_) && std::fabs(n_) < 1e15) std::snprintf(b, sizeof b, "%.0f", n_); else std::snprintf(b, sizeof b, "%.10g", n_);
      o = b; break;
    }
    case Type::String: escapeInto(o, s_); break;
    case Type::Array: { o = "["; bool f = true; for (auto& v : a_) { if (!f) o += ','; f = false; o += v.dump(); } o += ']'; break; }
    case Type::Object: { o = "{"; bool f = true; for (auto& [k, v] : o_) { if (!f) o += ','; f = false; escapeInto(o, k); o += ':'; o += v.dump(); } o += '}'; break; }
  }
  return o;
}

namespace detail {
struct Parser {
  const std::string& s;
  std::size_t i = 0;
  std::string err;
  int depth = 0;
  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
  bool fail(const char* m) { if (err.empty()) err = m; return false; }
  bool lit(const char* w) { std::size_t n = std::char_traits<char>::length(w); if (s.compare(i, n, w) == 0) { i += n; return true; } return false; }
  static void utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
  }
  bool hex4(unsigned& out) {
    if (i + 4 > s.size()) return false;
    out = 0;
    for (int k = 0; k < 4; ++k) { const char c = s[i++]; out <<= 4; if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0'); else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10); else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10); else return false; }
    return true;
  }
  bool str(std::string& out) {
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    while (i < s.size()) {
      const char c = s[i++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') { out += c; continue; }
      if (i >= s.size()) break;
      const char e = s[i++];
      switch (e) {
        case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
        case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return fail("bad \\u escape");
          if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') { i += 2; unsigned lo; if (!hex4(lo)) return fail("bad \\u escape"); cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
          utf8(out, cp); break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }
  bool value(Value& v) {
    ws();
    if (i >= s.size()) return fail("unexpected end");
    if (++depth > 64) return fail("nested too deeply");
    struct Guard { int& d; ~Guard() { --d; } } g{depth};
    const char c = s[i];
    if (c == '{') {
      ++i; v = Value::object(); ws();
      if (i < s.size() && s[i] == '}') { ++i; return true; }
      for (;;) {
        ws(); std::string k; if (!str(k)) return false;
        ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':'");
        ++i; Value x; if (!value(x)) return false;
        v.members()[k] = std::move(x);
        ws(); if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i; v = Value::array(); ws();
      if (i < s.size() && s[i] == ']') { ++i; return true; }
      for (;;) {
        Value x; if (!value(x)) return false;
        v.items().push_back(std::move(x));
        ws(); if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { std::string t; if (!str(t)) return false; v = Value::string(std::move(t)); return true; }
    if (lit("true")) { v = Value::boolean(true); return true; }
    if (lit("false")) { v = Value::boolean(false); return true; }
    if (lit("null")) { v = Value::null(); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      char* end = nullptr;
      const double d = std::strtod(s.c_str() + i, &end);
      if (end == s.c_str() + i) return fail("bad number");
      i = static_cast<std::size_t>(end - s.c_str());
      v = Value::number(d); return true;
    }
    return fail("unexpected character");
  }
};
}  // namespace detail

inline bool parse(const std::string& text, Value& out, std::string* error = nullptr) {
  if (text.size() > 8u * 1024u * 1024u) { if (error) *error = "input too large"; return false; }
  detail::Parser p{text, 0, {}, 0};
  Value v;
  if (!p.value(v)) { if (error) *error = p.err; return false; }
  p.ws();
  if (p.i != text.size()) { if (error) *error = "trailing characters"; return false; }
  out = std::move(v);
  return true;
}

}  // namespace wezt::json
