// svx_city — core/value.hpp.
#include "core/value.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/js.hpp"

namespace svx::city {

namespace {

// An array index key ("0", "17"; not "01", "-1"): JS orders these first, ascending.
bool index_key(std::string_view k, uint32_t* out) {
  if (k.empty() || k.size() > 10 || (k.size() > 1 && k[0] == '0')) return false;
  uint64_t v = 0;
  for (char c : k) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  if (v >= 4294967295ull) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

}  // namespace

const Value& Value::undefined() {
  static const Value u;
  return u;
}

Value Value::array(std::vector<Value> items) {
  Value v;
  v.t_ = Type::Array;
  v.a_ = std::make_shared<std::vector<Value>>(std::move(items));
  return v;
}

Value Value::object(std::vector<Member> members) {
  Value v;
  v.t_ = Type::Object;
  v.o_ = std::make_shared<std::vector<Member>>();
  for (auto& m : members) v.set(m.first, std::move(m.second));
  return v;
}

std::vector<Value>& Value::arr_w() {
  if (!a_) a_ = std::make_shared<std::vector<Value>>();
  if (a_.use_count() > 1) a_ = std::make_shared<std::vector<Value>>(*a_);
  return *a_;
}

std::vector<Value::Member>& Value::obj_w() {
  if (!o_) o_ = std::make_shared<std::vector<Member>>();
  if (o_.use_count() > 1) o_ = std::make_shared<std::vector<Member>>(*o_);
  return *o_;
}

bool Value::truthy() const {
  switch (t_) {
    case Type::Undefined:
    case Type::Null: return false;
    case Type::Bool: return b_;
    case Type::Number: return n_ == n_ && n_ != 0;
    case Type::String: return !s_.empty();
    default: return true;
  }
}

double Value::to_number() const {
  switch (t_) {
    case Type::Undefined: return js::kNaN;
    case Type::Null: return 0;
    case Type::Bool: return b_ ? 1 : 0;
    case Type::Number: return n_;
    case Type::String: {
      size_t a = 0, b = s_.size();
      while (a < b && (s_[a] == ' ' || s_[a] == '\t' || s_[a] == '\n' || s_[a] == '\r')) ++a;
      while (b > a && (s_[b - 1] == ' ' || s_[b - 1] == '\t' || s_[b - 1] == '\n' || s_[b - 1] == '\r')) --b;
      return a == b ? 0.0 : js::parse_number(std::string_view(s_).substr(a, b - a));
    }
    default: return js::kNaN;
  }
}

const std::string& Value::str() const {
  static const std::string empty;
  return t_ == Type::String ? s_ : empty;
}

std::string Value::to_string() const {
  switch (t_) {
    case Type::Undefined: return "undefined";
    case Type::Null: return "null";
    case Type::Bool: return b_ ? "true" : "false";
    case Type::Number: return js::num(n_);
    case Type::String: return s_;
    case Type::Array: {
      std::string s;
      for (size_t i = 0; i < a_->size(); ++i) {
        if (i) s += ',';
        if (!(*a_)[i].is_nullish()) s += (*a_)[i].to_string();
      }
      return s;
    }
    default: return "[object Object]";
  }
}

size_t Value::size() const {
  if (t_ == Type::Array) return a_->size();
  if (t_ == Type::Object) return o_->size();
  return 0;
}

const Value& Value::operator[](size_t i) const {
  if (t_ != Type::Array || i >= a_->size()) return undefined();
  return (*a_)[i];
}

const Value& Value::operator[](std::string_view key) const {
  if (t_ != Type::Object) return undefined();
  for (const auto& m : *o_)
    if (m.first == key) return m.second;
  return undefined();
}

bool Value::has(std::string_view key) const {
  if (t_ != Type::Object) return false;
  for (const auto& m : *o_)
    if (m.first == key) return true;
  return false;
}

Value& Value::at_mut(std::string_view key) {
  if (t_ != Type::Object) {
    *this = object();
  }
  auto& o = obj_w();
  for (auto& m : o)
    if (m.first == key) return m.second;
  set(key, Value());
  for (auto& m : obj_w())
    if (m.first == key) return m.second;
  return obj_w().back().second;
}

void Value::set(std::string_view key, Value v) {
  if (t_ != Type::Object) *this = object();
  auto& o = obj_w();
  for (auto& m : o)
    if (m.first == key) {
      m.second = std::move(v);
      return;
    }
  uint32_t idx;
  if (index_key(key, &idx)) {
    // (after the index keys below it, before every other key)
    size_t at = 0;
    while (at < o.size()) {
      uint32_t j;
      if (!index_key(o[at].first, &j) || j > idx) break;
      ++at;
    }
    o.insert(o.begin() + static_cast<std::ptrdiff_t>(at), Member(std::string(key), std::move(v)));
    return;
  }
  o.emplace_back(std::string(key), std::move(v));
}

void Value::push(Value v) {
  if (t_ != Type::Array) *this = array();
  arr_w().push_back(std::move(v));
}

void Value::erase(std::string_view key) {
  if (t_ != Type::Object) return;
  auto& o = obj_w();
  for (size_t i = 0; i < o.size(); ++i)
    if (o[i].first == key) {
      o.erase(o.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
}

const std::vector<Value>& Value::items() const {
  static const std::vector<Value> none;
  return t_ == Type::Array ? *a_ : none;
}

std::vector<Value>& Value::items_mut() {
  if (t_ != Type::Array) *this = array();
  return arr_w();
}

const std::vector<Value::Member>& Value::members() const {
  static const std::vector<Member> none;
  return t_ == Type::Object ? *o_ : none;
}

void Value::deep_merge(const Value& src) {
  for (const auto& [k, v] : src.members()) {
    // (JS: v && typeof v === "object" && !Array.isArray(v) && target[k] && typeof target[k] === "object")
    const Value& cur = (*this)[k];
    if (v.is_object() && (cur.is_object() || cur.is_array())) {
      at_mut(k).deep_merge(v);
    } else {
      set(k, v);
    }
  }
}

Value Value::spread(const Value& a, const Value& b) {
  Value out = object();
  for (const auto& [k, v] : a.members()) out.set(k, v);
  for (const auto& [k, v] : b.members()) out.set(k, v);
  return out;
}

bool Value::operator==(const Value& o) const {
  if (t_ != o.t_) return false;
  switch (t_) {
    case Type::Undefined:
    case Type::Null: return true;
    case Type::Bool: return b_ == o.b_;
    case Type::Number: return n_ == o.n_;
    case Type::String: return s_ == o.s_;
    case Type::Array: return *a_ == *o.a_;
    default: return *o_ == *o.o_;
  }
}

// ---- JSON

namespace {

struct Parser {
  std::string_view t;
  size_t p = 0;
  std::string err;
  void ws() {
    while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) ++p;
  }
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at " + std::to_string(p);
    return false;
  }
  bool lit(std::string_view w) {
    if (t.substr(p, w.size()) != w) return false;
    p += w.size();
    return true;
  }
  bool string(std::string* out) {
    if (p >= t.size() || t[p] != '"') return fail("string expected");
    ++p;
    while (p < t.size() && t[p] != '"') {
      char c = t[p++];
      if (c == '\\') {
        if (p >= t.size()) return fail("bad escape");
        const char e = t[p++];
        switch (e) {
          case 'n': *out += '\n'; break;
          case 't': *out += '\t'; break;
          case 'r': *out += '\r'; break;
          case 'b': *out += '\b'; break;
          case 'f': *out += '\f'; break;
          case 'u': {
            if (p + 4 > t.size()) return fail("bad \\u");
            const unsigned cp = static_cast<unsigned>(std::strtoul(std::string(t.substr(p, 4)).c_str(), nullptr, 16));
            p += 4;
            // (UTF-8; the generator's data is ASCII)
            if (cp < 0x80) {
              *out += static_cast<char>(cp);
            } else if (cp < 0x800) {
              *out += static_cast<char>(0xC0 | (cp >> 6));
              *out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
              *out += static_cast<char>(0xE0 | (cp >> 12));
              *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
              *out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
          }
          default: *out += e;
        }
      } else {
        *out += c;
      }
    }
    if (p >= t.size()) return fail("unterminated string");
    ++p;
    return true;
  }
  bool value(Value* out) {
    ws();
    if (p >= t.size()) return fail("value expected");
    const char c = t[p];
    if (c == '{') {
      ++p;
      *out = Value::object();
      ws();
      if (p < t.size() && t[p] == '}') {
        ++p;
        return true;
      }
      for (;;) {
        ws();
        std::string k;
        if (!string(&k)) return false;
        ws();
        if (p >= t.size() || t[p] != ':') return fail("':' expected");
        ++p;
        Value v;
        if (!value(&v)) return false;
        out->set(k, std::move(v));
        ws();
        if (p < t.size() && t[p] == ',') {
          ++p;
          continue;
        }
        if (p < t.size() && t[p] == '}') {
          ++p;
          return true;
        }
        return fail("',' or '}' expected");
      }
    }
    if (c == '[') {
      ++p;
      *out = Value::array();
      ws();
      if (p < t.size() && t[p] == ']') {
        ++p;
        return true;
      }
      for (;;) {
        Value v;
        if (!value(&v)) return false;
        out->push(std::move(v));
        ws();
        if (p < t.size() && t[p] == ',') {
          ++p;
          continue;
        }
        if (p < t.size() && t[p] == ']') {
          ++p;
          return true;
        }
        return fail("',' or ']' expected");
      }
    }
    if (c == '"') {
      std::string s;
      if (!string(&s)) return false;
      *out = Value(std::move(s));
      return true;
    }
    if (lit("true")) {
      *out = Value(true);
      return true;
    }
    if (lit("false")) {
      *out = Value(false);
      return true;
    }
    if (lit("null")) {
      *out = Value(nullptr);
      return true;
    }
    const size_t a = p;
    while (p < t.size() && (std::isdigit(static_cast<unsigned char>(t[p])) || t[p] == '-' || t[p] == '+' || t[p] == '.' || t[p] == 'e' || t[p] == 'E')) ++p;
    if (a == p) return fail("unexpected character");
    const double n = js::parse_number(t.substr(a, p - a));
    if (n != n) return fail("bad number");
    *out = Value(n);
    return true;
  }
};

void quote(const std::string& s, std::string* out) {
  *out += '"';
  for (char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\t': *out += "\\t"; break;
      case '\r': *out += "\\r"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          *out += b;
        } else {
          *out += c;
        }
    }
  }
  *out += '"';
}

void write(const Value& v, std::string* out) {
  switch (v.type()) {
    case Value::Type::Undefined:
    case Value::Type::Null: *out += "null"; return;
    case Value::Type::Bool: *out += v.truthy() ? "true" : "false"; return;
    case Value::Type::Number: {
      const double n = v.to_number();
      *out += std::isfinite(n) ? js::num(n) : "null";
      return;
    }
    case Value::Type::String: quote(v.str(), out); return;
    case Value::Type::Array: {
      *out += '[';
      bool first = true;
      for (const Value& e : v.items()) {
        if (!first) *out += ',';
        first = false;
        write(e, out);
      }
      *out += ']';
      return;
    }
    case Value::Type::Object: {
      *out += '{';
      bool first = true;
      for (const auto& [k, e] : v.members()) {
        if (e.is_undefined()) continue;
        if (!first) *out += ',';
        first = false;
        quote(k, out);
        *out += ':';
        write(e, out);
      }
      *out += '}';
      return;
    }
  }
}

}  // namespace

bool Value::parse_json(std::string_view text, Value* out, std::string* error) {
  Parser ps;
  ps.t = text;
  Value v;
  if (!ps.value(&v)) {
    if (error) *error = ps.err;
    return false;
  }
  ps.ws();
  if (ps.p != text.size()) {
    if (error) *error = "trailing characters at " + std::to_string(ps.p);
    return false;
  }
  *out = std::move(v);
  return true;
}

std::string Value::json() const {
  std::string s;
  write(*this, &s);
  return s;
}

}  // namespace svx::city
