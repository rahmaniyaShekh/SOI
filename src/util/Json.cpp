#include "util/Json.h"

#include <cstdlib>
#include <cstring>

namespace soi {

namespace {
const JsonValue& nullValue() {
    static const JsonValue v;
    return v;
}
} // namespace

const JsonValue& JsonValue::operator[](const std::string& key) const {
    if (type_ != Type::Object) return nullValue();
    // Last one wins on a duplicate key, as in every mainstream parser.
    for (size_t i = keys_.size(); i-- > 0;)
        if (keys_[i] == key) return arr_[i];
    return nullValue();
}

bool JsonValue::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& k : keys_)
        if (k == key) return true;
    return false;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view s) : s_(s) {}

    bool document(JsonValue& out) {
        skipWs();
        if (!value(out, 0)) return false;
        skipWs();
        if (pos_ != s_.size()) return fail("trailing characters after the document");
        return true;
    }

    const std::string& error() const { return error_; }

private:
    // Deep enough for any real document, shallow enough that a hostile one
    // cannot exhaust the stack.
    static constexpr int kMaxDepth = 64;

    bool fail(const char* why) {
        if (error_.empty()) error_ = std::string(why) + " at offset " + std::to_string(pos_);
        return false;
    }

    void skipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool literal(const char* word) {
        const size_t n = std::strlen(word);
        if (s_.substr(pos_, n) != word) return fail("invalid literal");
        pos_ += n;
        return true;
    }

    bool value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        const char c = s_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') { out.type_ = JsonValue::Type::String; return string(out.str_); }
        if (c == 't') { out.type_ = JsonValue::Type::Bool; out.bool_ = true;  return literal("true"); }
        if (c == 'f') { out.type_ = JsonValue::Type::Bool; out.bool_ = false; return literal("false"); }
        if (c == 'n') { out.type_ = JsonValue::Type::Null; return literal("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }

    bool object(JsonValue& out, int depth) {
        out.type_ = JsonValue::Type::Object;
        ++pos_;   // {
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected a key");
            std::string key;
            if (!string(key)) return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            skipWs();
            JsonValue v;
            if (!value(v, depth + 1)) return false;
            out.keys_.push_back(std::move(key));
            out.arr_.push_back(std::move(v));
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool array(JsonValue& out, int depth) {
        out.type_ = JsonValue::Type::Array;
        ++pos_;   // [
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            skipWs();
            JsonValue v;
            if (!value(v, depth + 1)) return false;
            out.arr_.push_back(std::move(v));
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool number(JsonValue& out) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        auto digits = [&] {
            const size_t d = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            return pos_ > d;
        };
        if (!digits()) return fail("invalid number");
        if (pos_ < s_.size() && s_[pos_] == '.') { ++pos_; if (!digits()) return fail("invalid number"); }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (!digits()) return fail("invalid number");
        }
        const std::string text(s_.substr(start, pos_ - start));
        out.type_ = JsonValue::Type::Number;
        out.num_  = std::strtod(text.c_str(), nullptr);
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& cp) {
        if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        ++pos_;   // opening quote
        for (;;) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') { out += c; continue; }

            if (pos_ >= s_.size()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // A high surrogate must be followed by \u and a low one.
                        if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                            return fail("unpaired surrogate");
                        pos_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    std::string_view s_;
    size_t           pos_ = 0;
    std::string      error_;
};

bool JsonValue::parse(std::string_view text, JsonValue& out, std::string* error) {
    out = JsonValue{};
    JsonParser p(text);
    if (p.document(out)) return true;
    if (error) *error = p.error();
    out = JsonValue{};
    return false;
}

} // namespace soi
