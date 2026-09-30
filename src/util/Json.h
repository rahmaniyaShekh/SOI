#pragma once
//
// A small, strict JSON reader -- just enough to read a GitHub release.
//
// The rest of the project emits hand-rolled JSON and never had to read any. The
// updater does: it picks an asset id out of a release document that nests
// objects (uploader, reactions) inside every asset, which substring matching
// gets wrong the first time a field it is not looking for contains the name it
// is. So this parses properly and refuses anything malformed.
//
// Numbers are kept as double, which is exact for every id GitHub issues today
// (< 2^53). Strings are decoded to UTF-8, including \u escapes and surrogate
// pairs.
//
#include <string>
#include <string_view>
#include <vector>

namespace soi {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }

    // Accessors return a neutral value on a type mismatch rather than throwing:
    // every caller treats "missing" and "wrong type" the same way.
    const std::string& str() const { return str_; }
    double             num() const { return num_; }
    bool               boolean() const { return bool_; }
    const std::vector<JsonValue>& items() const { return arr_; }

    // Null value when absent, so lookups chain: v["a"]["b"].str().
    const JsonValue& operator[](const std::string& key) const;
    bool has(const std::string& key) const;

    // Parses a complete document. Trailing non-whitespace is an error.
    static bool parse(std::string_view text, JsonValue& out, std::string* error = nullptr);

private:
    friend class JsonParser;
    Type                             type_ = Type::Null;
    bool                             bool_ = false;
    double                           num_  = 0;
    std::string                      str_;
    // Arrays use arr_. Objects use keys_ and arr_ in parallel: std::vector is
    // the one standard container guaranteed to accept an incomplete element
    // type, which JsonValue is inside its own definition.
    std::vector<JsonValue>           arr_;
    std::vector<std::string>         keys_;
};

} // namespace soi
