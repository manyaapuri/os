#pragma once

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstdint>
#include <algorithm>

namespace dfs {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::map<std::string, JsonValue> obj_val;

    JsonValue() : type(Type::Null) {}
    JsonValue(bool b) : type(Type::Bool), bool_val(b) {}
    JsonValue(int n) : type(Type::Number), num_val(n) {}
    JsonValue(int64_t n) : type(Type::Number), num_val(static_cast<double>(n)) {}
    JsonValue(uint64_t n) : type(Type::Number), num_val(static_cast<double>(n)) {}
    JsonValue(double n) : type(Type::Number), num_val(n) {}
    JsonValue(const char* s) : type(Type::String), str_val(s ? s : "") {}
    JsonValue(const std::string& s) : type(Type::String), str_val(s) {}

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? bool_val : def; }
    int asInt(int def = 0) const { return isNumber() ? static_cast<int>(num_val) : def; }
    int64_t asInt64(int64_t def = 0) const { return isNumber() ? static_cast<int64_t>(num_val) : def; }
    double asDouble(double def = 0.0) const { return isNumber() ? num_val : def; }
    std::string asString(const std::string& def = "") const { return isString() ? str_val : def; }

    bool contains(const std::string& key) const {
        return isObject() && (obj_val.find(key) != obj_val.end());
    }

    const JsonValue& operator[](const std::string& key) const {
        static JsonValue null_v;
        if (!isObject()) return null_v;
        auto it = obj_val.find(key);
        return (it != obj_val.end()) ? it->second : null_v;
    }

    JsonValue& operator[](const std::string& key) {
        if (!isObject()) {
            type = Type::Object;
            obj_val.clear();
        }
        return obj_val[key];
    }

    const JsonValue& operator[](size_t idx) const {
        static JsonValue null_v;
        if (!isArray() || idx >= arr_val.size()) return null_v;
        return arr_val[idx];
    }

    void push_back(const JsonValue& val) {
        if (!isArray()) {
            type = Type::Array;
            arr_val.clear();
        }
        arr_val.push_back(val);
    }

    std::string serialize(bool pretty = false, int indent = 0) const {
        std::ostringstream ss;
        std::string ind(indent * 2, ' ');
        std::string next_ind((indent + 1) * 2, ' ');

        switch (type) {
            case Type::Null:
                ss << "null";
                break;
            case Type::Bool:
                ss << (bool_val ? "true" : "false");
                break;
            case Type::Number: {
                if (num_val == static_cast<int64_t>(num_val)) {
                    ss << static_cast<int64_t>(num_val);
                } else {
                    ss << num_val;
                }
                break;
            }
            case Type::String:
                ss << "\"" << escapeString(str_val) << "\"";
                break;
            case Type::Array: {
                ss << "[";
                if (pretty && !arr_val.empty()) ss << "\n";
                for (size_t i = 0; i < arr_val.size(); ++i) {
                    if (pretty) ss << next_ind;
                    ss << arr_val[i].serialize(pretty, indent + 1);
                    if (i + 1 < arr_val.size()) ss << ",";
                    if (pretty) ss << "\n";
                }
                if (pretty && !arr_val.empty()) ss << ind;
                ss << "]";
                break;
            }
            case Type::Object: {
                ss << "{";
                if (pretty && !obj_val.empty()) ss << "\n";
                size_t i = 0;
                for (auto it = obj_val.begin(); it != obj_val.end(); ++it, ++i) {
                    if (pretty) ss << next_ind;
                    ss << "\"" << escapeString(it->first) << "\":" << (pretty ? " " : "");
                    ss << it->second.serialize(pretty, indent + 1);
                    if (i + 1 < obj_val.size()) ss << ",";
                    if (pretty) ss << "\n";
                }
                if (pretty && !obj_val.empty()) ss << ind;
                ss << "}";
                break;
            }
        }
        return ss.str();
    }

    static std::string escapeString(const std::string& s) {
        std::ostringstream ss;
        for (char c : s) {
            switch (c) {
                case '\"': ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b"; break;
                case '\f': ss << "\\f"; break;
                case '\n': ss << "\\n"; break;
                case '\r': ss << "\\r"; break;
                case '\t': ss << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        ss << buf;
                    } else {
                        ss << c;
                    }
                    break;
            }
        }
        return ss.str();
    }

    static JsonValue parse(const std::string& str) {
        size_t idx = 0;
        skipWhitespace(str, idx);
        return parseValue(str, idx);
    }

private:
    static void skipWhitespace(const std::string& str, size_t& idx) {
        while (idx < str.size() && (str[idx] == ' ' || str[idx] == '\t' || str[idx] == '\r' || str[idx] == '\n')) {
            idx++;
        }
    }

    static JsonValue parseValue(const std::string& str, size_t& idx) {
        skipWhitespace(str, idx);
        if (idx >= str.size()) return JsonValue();

        char c = str[idx];
        if (c == '{') return parseObject(str, idx);
        if (c == '[') return parseArray(str, idx);
        if (c == '\"') return parseString(str, idx);
        if (c == 't' || c == 'f') return parseBool(str, idx);
        if (c == 'n') return parseNull(str, idx);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(str, idx);

        return JsonValue();
    }

    static JsonValue parseObject(const std::string& str, size_t& idx) {
        JsonValue val;
        val.type = Type::Object;
        idx++; // skip '{'

        while (idx < str.size()) {
            skipWhitespace(str, idx);
            if (idx < str.size() && str[idx] == '}') {
                idx++;
                return val;
            }

            JsonValue key = parseString(str, idx);
            skipWhitespace(str, idx);
            if (idx < str.size() && str[idx] == ':') {
                idx++;
            }
            JsonValue elem = parseValue(str, idx);
            val.obj_val[key.str_val] = elem;

            skipWhitespace(str, idx);
            if (idx < str.size() && str[idx] == ',') {
                idx++;
            } else if (idx < str.size() && str[idx] == '}') {
                idx++;
                return val;
            }
        }
        return val;
    }

    static JsonValue parseArray(const std::string& str, size_t& idx) {
        JsonValue val;
        val.type = Type::Array;
        idx++; // skip '['

        while (idx < str.size()) {
            skipWhitespace(str, idx);
            if (idx < str.size() && str[idx] == ']') {
                idx++;
                return val;
            }

            JsonValue elem = parseValue(str, idx);
            val.arr_val.push_back(elem);

            skipWhitespace(str, idx);
            if (idx < str.size() && str[idx] == ',') {
                idx++;
            } else if (idx < str.size() && str[idx] == ']') {
                idx++;
                return val;
            }
        }
        return val;
    }

    static JsonValue parseString(const std::string& str, size_t& idx) {
        JsonValue val;
        val.type = Type::String;
        idx++; // skip opening '\"'

        std::string s;
        while (idx < str.size()) {
            char c = str[idx++];
            if (c == '\"') {
                val.str_val = s;
                return val;
            }
            if (c == '\\' && idx < str.size()) {
                char esc = str[idx++];
                switch (esc) {
                    case '\"': s += '\"'; break;
                    case '\\': s += '\\'; break;
                    case '/':  s += '/'; break;
                    case 'b':  s += '\b'; break;
                    case 'f':  s += '\f'; break;
                    case 'n':  s += '\n'; break;
                    case 'r':  s += '\r'; break;
                    case 't':  s += '\t'; break;
                    case 'u': {
                        // 4 hex digits
                        if (idx + 4 <= str.size()) {
                            std::string hex_str = str.substr(idx, 4);
                            idx += 4;
                            unsigned int codepoint = 0;
                            std::stringstream ss;
                            ss << std::hex << hex_str;
                            ss >> codepoint;
                            if (codepoint < 0x80) s += static_cast<char>(codepoint);
                            else s += '?';
                        }
                        break;
                    }
                    default: s += esc; break;
                }
            } else {
                s += c;
            }
        }
        val.str_val = s;
        return val;
    }

    static JsonValue parseBool(const std::string& str, size_t& idx) {
        if (str.compare(idx, 4, "true") == 0) {
            idx += 4;
            return JsonValue(true);
        }
        if (str.compare(idx, 5, "false") == 0) {
            idx += 5;
            return JsonValue(false);
        }
        return JsonValue();
    }

    static JsonValue parseNull(const std::string& str, size_t& idx) {
        if (str.compare(idx, 4, "null") == 0) {
            idx += 4;
        }
        return JsonValue();
    }

    static JsonValue parseNumber(const std::string& str, size_t& idx) {
        size_t start = idx;
        if (str[idx] == '-') idx++;
        while (idx < str.size() && (str[idx] >= '0' && str[idx] <= '9')) idx++;
        if (idx < str.size() && str[idx] == '.') {
            idx++;
            while (idx < str.size() && (str[idx] >= '0' && str[idx] <= '9')) idx++;
        }
        if (idx < str.size() && (str[idx] == 'e' || str[idx] == 'E')) {
            idx++;
            if (idx < str.size() && (str[idx] == '+' || str[idx] == '-')) idx++;
            while (idx < str.size() && (str[idx] >= '0' && str[idx] <= '9')) idx++;
        }

        std::string num_str = str.substr(start, idx - start);
        double val = std::stod(num_str);
        return JsonValue(val);
    }
};

} // namespace dfs
