// svx_city — a JavaScript value as data: the generator's configuration and presets (voxel_city
// config/*.js) are plain JS objects - nested, sparse, merged deeply, read with ?. and ??. A Value
// is undefined, null, a boolean, a number, a string, an array or an object; objects keep JS's key
// order (integer-like keys ascending first, then the others in insertion order). Reading a
// missing key gives undefined, never an error. Values are parsed from and written to JSON.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace svx::city {

class Value {
 public:
  enum class Type : uint8_t { Undefined, Null, Bool, Number, String, Array, Object };
  using Member = std::pair<std::string, Value>;

  Value() = default;
  Value(std::nullptr_t) : t_(Type::Null) {}                         // NOLINT
  Value(bool b) : t_(Type::Bool), b_(b) {}                          // NOLINT
  Value(double n) : t_(Type::Number), n_(n) {}                      // NOLINT
  Value(int n) : t_(Type::Number), n_(n) {}                         // NOLINT
  Value(long n) : t_(Type::Number), n_(static_cast<double>(n)) {}   // NOLINT
  Value(long long n) : t_(Type::Number), n_(static_cast<double>(n)) {}  // NOLINT
  Value(unsigned n) : t_(Type::Number), n_(n) {}                    // NOLINT
  Value(const char* s) : t_(Type::String), s_(s) {}                 // NOLINT
  Value(std::string s) : t_(Type::String), s_(std::move(s)) {}      // NOLINT
  static Value array(std::vector<Value> items = {});
  static Value object(std::vector<Member> members = {});
  static const Value& undefined();

  Type type() const { return t_; }
  bool is_undefined() const { return t_ == Type::Undefined; }
  bool is_null() const { return t_ == Type::Null; }
  bool is_nullish() const { return t_ == Type::Undefined || t_ == Type::Null; }  // (?? replaces these)
  bool is_bool() const { return t_ == Type::Bool; }
  bool is_number() const { return t_ == Type::Number; }
  bool is_string() const { return t_ == Type::String; }
  bool is_array() const { return t_ == Type::Array; }
  bool is_object() const { return t_ == Type::Object; }

  // JS truthiness (undefined, null, false, 0, NaN and "" are falsy; objects and arrays truthy).
  bool truthy() const;
  // ToNumber (undefined: NaN, null: 0, booleans 0 / 1, strings parsed, objects NaN).
  double to_number() const;
  // The number, or def when this is undefined or null (x ?? def).
  double num(double def) const { return is_nullish() ? def : to_number(); }
  // x ?? def for any value.
  const Value& or_nullish(const Value& def) const { return is_nullish() ? def : *this; }
  // x || def for numbers.
  double num_or(double def) const { return truthy() ? to_number() : def; }
  bool boolean() const { return truthy(); }
  const std::string& str() const;  // ("" unless a string)
  // String(x).
  std::string to_string() const;

  // arrays and objects
  size_t size() const;
  const Value& operator[](size_t i) const;
  const Value& operator[](std::string_view key) const;
  const Value& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
  const Value& operator[](const std::string& key) const { return (*this)[std::string_view(key)]; }
  bool has(std::string_view key) const;  // (key in obj: present, even when undefined)
  Value& at_mut(std::string_view key);   // (made: undefined, when missing; this becomes an object if it is not)
  void set(std::string_view key, Value v);
  void push(Value v);
  void erase(std::string_view key);
  const std::vector<Value>& items() const;
  const std::vector<Member>& members() const;
  std::vector<Value>& items_mut();

  // makeConfig's deepMerge: src's keys onto this - objects (not arrays) merged key by key, everything
  // else (an undefined too) replacing.
  void deep_merge(const Value& src);
  // {...a, ...b}: a shallow copy of an object with another's keys on top (undefined values copied).
  static Value spread(const Value& a, const Value& b);

  // JSON (JSON.stringify: undefined members left out, NaN and infinities null, numbers as JS
  // prints them).
  static bool parse_json(std::string_view text, Value* out, std::string* error = nullptr);
  std::string json() const;

  bool operator==(const Value& o) const;

 private:
  Type t_ = Type::Undefined;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::shared_ptr<std::vector<Value>> a_;   // (copy on write)
  std::shared_ptr<std::vector<Member>> o_;  // (copy on write)
  std::vector<Value>& arr_w();
  std::vector<Member>& obj_w();
};

}  // namespace svx::city
