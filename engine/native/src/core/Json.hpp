#pragma once

// A small JSON reader for translation patches (thcrap's files are JSON, read as JSON5 by
// thcrap, so this also takes comments, trailing commas, single-quoted strings and bare
// keys), and the deep merge thcrap uses to stack patches: objects merge key by key, any
// other value replaces.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace touhou {

class Json {
  public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static std::optional<Json> parse(std::string_view text, std::string* error = nullptr);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }
    bool isNumber() const { return type_ == Type::Number; }

    const std::string& string() const { return string_; }
    double number() const { return number_; }
    bool boolean() const { return bool_; }
    const std::vector<Json>& array() const { return array_; }
    // Object members (sorted by key).
    const std::map<std::string, Json>& object() const { return object_; }
    // A member, or null when absent (or this is not an object).
    const Json* find(std::string_view key) const;
    std::size_t size() const { return isArray() ? array_.size() : isObject() ? object_.size() : 0; }

    // thcrap's stacking: `over` merged into this (objects recursively; others replace).
    void merge(const Json& over);

    static Json makeString(std::string s);
    static Json makeObject();
    void set(std::string key, Json value);
    void push(Json value);

  private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;
};

// UTF-8 helpers.
void appendUtf8(std::string& out, char32_t cp);
// Cuts `s` to at most `maxBytes` bytes without splitting a character.
std::string_view utf8Prefix(std::string_view s, std::size_t maxBytes);

}  // namespace touhou
