#include "core/Json.hpp"

#include <cctype>
#include <cstdlib>

namespace th06 {

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string_view utf8Prefix(std::string_view s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // not inside a character
    return s.substr(0, n);
}

class JsonParser {
  public:
    explicit JsonParser(std::string_view t) : t_(t) {}

    std::optional<Json> document(std::string* error) {
        // A UTF-8 byte-order mark is allowed.
        if (t_.substr(0, 3) == "\xEF\xBB\xBF") at_ = 3;
        Json v;
        if (!value(v, 0)) {
            if (error) *error = error_ + " at byte " + std::to_string(at_);
            return std::nullopt;
        }
        space();
        if (at_ != t_.size()) {
            if (error) *error = "trailing data at byte " + std::to_string(at_);
            return std::nullopt;
        }
        return v;
    }

  private:
    std::string_view t_;
    std::size_t at_ = 0;
    std::string error_ = "syntax error";

    bool fail(const char* why) {
        error_ = why;
        return false;
    }

    void space() {
        while (at_ < t_.size()) {
            const char c = t_[at_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++at_;
            } else if (c == '/' && at_ + 1 < t_.size() && t_[at_ + 1] == '/') {
                while (at_ < t_.size() && t_[at_] != '\n') ++at_;
            } else if (c == '/' && at_ + 1 < t_.size() && t_[at_ + 1] == '*') {
                const std::size_t end = t_.find("*/", at_ + 2);
                at_ = end == std::string_view::npos ? t_.size() : end + 2;
            } else {
                break;
            }
        }
    }

    bool hex4(char32_t& out) {
        if (at_ + 4 > t_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[at_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<char32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<char32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<char32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        const char quote = t_[at_++];
        while (at_ < t_.size()) {
            const char c = t_[at_++];
            if (c == quote) return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= t_.size()) break;
            const char e = t_[at_++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '0': out.push_back('\0'); break;
                case '\n': break;  // JSON5 line continuation
                case 'u': {
                    char32_t cp;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp < 0xDC00 && at_ + 6 <= t_.size() && t_[at_] == '\\' && t_[at_ + 1] == 'u') {
                        at_ += 2;
                        char32_t lo;
                        if (!hex4(lo)) return fail("bad \\u escape");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: out.push_back(e); break;  // \" \\ \/ \'
            }
        }
        return fail("unterminated string");
    }

    bool key(std::string& out) {
        space();
        if (at_ >= t_.size()) return fail("unexpected end");
        if (t_[at_] == '"' || t_[at_] == '\'') return string(out);
        // JSON5 bare key.
        while (at_ < t_.size() && (std::isalnum(static_cast<unsigned char>(t_[at_])) || t_[at_] == '_' || t_[at_] == '$')) {
            out.push_back(t_[at_++]);
        }
        return !out.empty() || fail("expected a key");
    }

    bool value(Json& v, int depth) {
        if (depth > 64) return fail("nested too deeply");
        space();
        if (at_ >= t_.size()) return fail("unexpected end");
        const char c = t_[at_];
        if (c == '{') {
            ++at_;
            v.type_ = Json::Type::Object;
            while (true) {
                space();
                if (at_ < t_.size() && t_[at_] == '}') {
                    ++at_;
                    return true;
                }
                std::string k;
                if (!key(k)) return false;
                space();
                if (at_ >= t_.size() || t_[at_] != ':') return fail("expected ':'");
                ++at_;
                Json member;
                if (!value(member, depth + 1)) return false;
                v.object_[std::move(k)] = std::move(member);
                space();
                if (at_ < t_.size() && t_[at_] == ',') {
                    ++at_;
                    continue;
                }
                space();
                if (at_ < t_.size() && t_[at_] == '}') {
                    ++at_;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++at_;
            v.type_ = Json::Type::Array;
            while (true) {
                space();
                if (at_ < t_.size() && t_[at_] == ']') {
                    ++at_;
                    return true;
                }
                Json item;
                if (!value(item, depth + 1)) return false;
                v.array_.push_back(std::move(item));
                space();
                if (at_ < t_.size() && t_[at_] == ',') {
                    ++at_;
                    continue;
                }
                space();
                if (at_ < t_.size() && t_[at_] == ']') {
                    ++at_;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"' || c == '\'') {
            v.type_ = Json::Type::String;
            return string(v.string_);
        }
        if (t_.substr(at_, 4) == "true") {
            at_ += 4;
            v.type_ = Json::Type::Bool;
            v.bool_ = true;
            return true;
        }
        if (t_.substr(at_, 5) == "false") {
            at_ += 5;
            v.type_ = Json::Type::Bool;
            return true;
        }
        if (t_.substr(at_, 4) == "null") {
            at_ += 4;
            return true;
        }
        // A number (hexadecimal too, as JSON5 allows).
        std::size_t end = at_;
        while (end < t_.size() && (std::isalnum(static_cast<unsigned char>(t_[end])) || t_[end] == '-' || t_[end] == '+' || t_[end] == '.')) {
            ++end;
        }
        const std::string num(t_.substr(at_, end - at_));
        if (num.empty()) return fail("unexpected character");
        char* stop = nullptr;
        const bool hex = num.size() > 2 && num[0] == '0' && (num[1] == 'x' || num[1] == 'X');
        const double d = hex ? static_cast<double>(std::strtoll(num.c_str(), &stop, 16)) : std::strtod(num.c_str(), &stop);
        if (!stop || *stop != '\0') return fail("bad number");
        at_ = end;
        v.type_ = Json::Type::Number;
        v.number_ = d;
        return true;
    }
};

std::optional<Json> Json::parse(std::string_view text, std::string* error) { return JsonParser(text).document(error); }

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    const auto it = object_.find(std::string(key));
    return it == object_.end() ? nullptr : &it->second;
}

void Json::merge(const Json& over) {
    if (type_ == Type::Object && over.type_ == Type::Object) {
        for (const auto& [k, v] : over.object_) {
            auto it = object_.find(k);
            if (it == object_.end()) object_.emplace(k, v);
            else it->second.merge(v);
        }
        return;
    }
    *this = over;
}

Json Json::makeString(std::string s) {
    Json j;
    j.type_ = Type::String;
    j.string_ = std::move(s);
    return j;
}

Json Json::makeObject() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

void Json::set(std::string key, Json value) {
    if (type_ != Type::Object) *this = makeObject();
    object_[std::move(key)] = std::move(value);
}

void Json::push(Json value) {
    if (type_ != Type::Array) {
        *this = Json{};
        type_ = Type::Array;
    }
    array_.push_back(std::move(value));
}

}  // namespace th06
