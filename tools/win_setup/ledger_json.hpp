// JSON reader for the local installer ledger. Rejects duplicate keys and trailing data.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ledger_json {

struct Value {
    enum class Type { Null, Other, Number, String, Array, Object };
    Type type = Type::Null;
    std::string text;
    std::vector<Value> elements;
    std::map<std::string, Value> fields;

    const Value& at(const char* key) const { return fields.at(key); }
    const std::string& string() const
    {
        if (type != Type::String) throw std::runtime_error("expected JSON string");
        return text;
    }
};

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    Value parse()
    {
        auto result = value(0);
        skip_space();
        if (!input_.empty()) fail();
        return result;
    }

private:
    std::string_view input_;
    [[noreturn]] static void fail() { throw std::runtime_error("invalid ledger JSON"); }
    void skip_space()
    {
        while (!input_.empty() && std::string_view(" \t\n\r").find(input_.front()) != std::string_view::npos)
            input_.remove_prefix(1);
    }
    bool take(char c)
    {
        if (input_.empty() || input_.front() != c) return false;
        input_.remove_prefix(1);
        return true;
    }
    void expect(char c) { if (!take(c)) fail(); }
    char next()
    {
        if (input_.empty()) fail();
        char c = input_.front();
        input_.remove_prefix(1);
        return c;
    }
    bool digit() const { return !input_.empty() && input_.front() >= '0' && input_.front() <= '9'; }
    void digits()
    {
        if (!digit()) fail();
        do { next(); } while (digit());
    }
    unsigned hex4()
    {
        unsigned value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = next();
            if (c >= '0' && c <= '9') value = value * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') value = value * 16 + c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value = value * 16 + c - 'A' + 10;
            else fail();
        }
        return value;
    }
    static void append_utf8(std::string& out, unsigned code)
    {
        if (code < 0x80) out += static_cast<char>(code);
        else {
            if (code >= 0x10000) out += static_cast<char>(0xf0 | (code >> 18));
            if (code >= 0x800)
                out += static_cast<char>((code < 0x10000 ? 0xe0 : 0x80) | ((code >> 12) & 0x3f));
            out += static_cast<char>((code < 0x800 ? 0xc0 : 0x80) | ((code >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (code & 0x3f));
        }
    }
    std::string string()
    {
        std::string result;
        expect('"');
        for (unsigned char c; (c = static_cast<unsigned char>(next())) != '"';) {
            if (c < 0x20) fail();
            if (c != '\\') { result += static_cast<char>(c); continue; }
            switch (next()) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                unsigned code = hex4();
                if (code >= 0xd800 && code <= 0xdbff) {
                    expect('\\');
                    expect('u');
                    const unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) fail();
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code <= 0xdfff) fail();
                append_utf8(result, code);
                break;
            }
            default: fail();
            }
        }
        return result;
    }
    Value value(unsigned depth)
    {
        if (depth > 32) fail();
        skip_space();
        Value v;
        if (take('{')) {
            v.type = Value::Type::Object;
            skip_space();
            if (take('}')) return v;
            do {
                skip_space();
                auto key = string();
                skip_space();
                expect(':');
                if (!v.fields.emplace(key, value(depth + 1)).second) fail();
                skip_space();
            } while (take(','));
            expect('}');
        } else if (take('[')) {
            v.type = Value::Type::Array;
            skip_space();
            if (take(']')) return v;
            do {
                v.elements.push_back(value(depth + 1));
                skip_space();
            } while (take(','));
            expect(']');
        } else if (!input_.empty() && input_.front() == '"') {
            v.type = Value::Type::String;
            v.text = string();
        } else {
            for (std::string_view literal : {"null", "true", "false"}) {
                if (input_.substr(0, literal.size()) == literal) {
                    input_.remove_prefix(literal.size());
                    v.type = literal == "null" ? Value::Type::Null : Value::Type::Other;
                    v.text = std::string(literal);
                    return v;
                }
            }
            const auto start = input_;
            take('-');
            if (!take('0')) digits();
            if (take('.')) digits();
            if (take('e') || take('E')) {
                if (!take('+')) take('-');
                digits();
            }
            v.type = Value::Type::Number;
            v.text = start.substr(0, start.size() - input_.size());
        }
        return v;
    }
};

} // namespace ledger_json
