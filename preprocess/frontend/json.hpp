/* ============================================================
 * json.hpp — 極簡 JSON 讀取器（只支援設定檔需要的子集）
 * BSTM Agent A / preprocess/frontend
 * 無外部相依，C++17。
 * ============================================================ */
#ifndef BSTM_JSON_HPP
#define BSTM_JSON_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <sstream>

namespace mjson {

struct Value;
using Array  = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    Type        type = NUL;
    bool        b    = false;
    double      num  = 0.0;
    std::string str;
    Array       arr;
    Object      obj;

    bool is_null() const { return type == NUL; }
    bool has(const std::string& k) const {
        return type == OBJ && obj.find(k) != obj.end();
    }
    const Value& get(const std::string& k) const {
        static const Value nil;
        if (type != OBJ) return nil;
        auto it = obj.find(k);
        return it == obj.end() ? nil : it->second;
    }
    const Value& operator[](const std::string& k) const { return get(k); }

    long as_int(long dflt = 0) const {
        switch (type) {
            case NUM:  return (long)num;
            case BOOL: return b ? 1 : 0;
            case STR:  return strtol(str.c_str(), nullptr, 0);
            default:   return dflt;
        }
    }
    double as_double(double dflt = 0.0) const {
        if (type == NUM)  return num;
        if (type == BOOL) return b ? 1.0 : 0.0;
        return dflt;
    }
    std::string as_str(const std::string& dflt = "") const {
        return type == STR ? str : dflt;
    }
    long geti(const std::string& k, long dflt) const {
        return has(k) ? get(k).as_int(dflt) : dflt;
    }
    double getd(const std::string& k, double dflt) const {
        return has(k) ? get(k).as_double(dflt) : dflt;
    }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s), i_(0) {}

    Value parse() {
        skip();
        Value v = value();
        skip();
        return v;
    }

private:
    const std::string& s_;
    size_t             i_;

    [[noreturn]] void fail(const std::string& msg) {
        std::ostringstream o;
        o << "json: " << msg << " (offset " << i_ << ")";
        throw std::runtime_error(o.str());
    }
    bool more() const { return i_ < s_.size(); }
    char cur() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    void skip() {
        while (more()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') { i_++; continue; }
            /* 允許 // 與 # 註解，設定檔常用 */
            if (c == '#') { while (more() && s_[i_] != '\n') i_++; continue; }
            if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                while (more() && s_[i_] != '\n') i_++;
                continue;
            }
            break;
        }
    }

    Value value() {
        skip();
        if (!more()) fail("unexpected end of input");
        char c = cur();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') { Value v; v.type = Value::STR; v.str = string(); return v; }
        if (!strncmp_at("true"))  { i_ += 4; Value v; v.type = Value::BOOL; v.b = true;  return v; }
        if (!strncmp_at("false")) { i_ += 5; Value v; v.type = Value::BOOL; v.b = false; return v; }
        if (!strncmp_at("null"))  { i_ += 4; Value v; v.type = Value::NUL;  return v; }
        return number();
    }

    int strncmp_at(const char* lit) {
        size_t n = 0;
        while (lit[n]) n++;
        if (i_ + n > s_.size()) return 1;
        return s_.compare(i_, n, lit);
    }

    Value object() {
        Value v;
        v.type = Value::OBJ;
        i_++; /* '{' */
        skip();
        while (more() && cur() != '}') {
            std::string k = string();
            skip();
            if (cur() != ':') fail("expected ':'");
            i_++;
            v.obj[k] = value();
            skip();
        }
        if (cur() != '}') fail("expected '}'");
        i_++;
        return v;
    }

    Value array() {
        Value v;
        v.type = Value::ARR;
        i_++; /* '[' */
        skip();
        while (more() && cur() != ']') {
            v.arr.push_back(value());
            skip();
        }
        if (cur() != ']') fail("expected ']'");
        i_++;
        return v;
    }

    std::string string() {
        skip();
        if (cur() != '"') fail("expected string");
        i_++;
        std::string out;
        while (more() && cur() != '"') {
            char c = s_[i_++];
            if (c == '\\' && more()) {
                char e = s_[i_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': { /* 只取低位元組，設定檔用不到 unicode */
                        if (i_ + 4 <= s_.size()) {
                            std::string h = s_.substr(i_, 4);
                            i_ += 4;
                            out += (char)strtol(h.c_str(), nullptr, 16);
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        if (cur() != '"') fail("unterminated string");
        i_++;
        return out;
    }

    Value number() {
        size_t start = i_;
        if (cur() == '+' || cur() == '-') i_++;
        bool hex = (i_ + 1 < s_.size() && s_[i_] == '0' && (s_[i_ + 1] == 'x' || s_[i_ + 1] == 'X'));
        if (hex) {
            i_ += 2;
            while (more() && isxdigit((unsigned char)cur())) i_++;
        } else {
            while (more() && (isdigit((unsigned char)cur()) || cur() == '.' ||
                              cur() == 'e' || cur() == 'E' || cur() == '+' || cur() == '-')) i_++;
        }
        if (i_ == start) fail("bad number");
        std::string t = s_.substr(start, i_ - start);
        Value v;
        v.type = Value::NUM;
        v.num  = hex ? (double)strtoull(t.c_str(), nullptr, 0) : strtod(t.c_str(), nullptr);
        return v;
    }
};

inline Value parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open config file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    return Parser(text).parse();
}

} /* namespace mjson */
#endif /* BSTM_JSON_HPP */
