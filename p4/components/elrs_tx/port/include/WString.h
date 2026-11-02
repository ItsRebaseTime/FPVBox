/* Minimal Arduino String (std::string backed). */
#pragma once
#include <string>
#include <string.h>

class String
{
public:
    String() {}
    String(const char *s) : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    String(char c) : s_(1, c) {}
    String(int v) : s_(std::to_string(v)) {}
    String(unsigned v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    const char *c_str() const { return s_.c_str(); }
    unsigned length() const { return (unsigned)s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    String &operator+=(const String &o) { s_ += o.s_; return *this; }
    String &operator+=(const char *o) { s_ += o ? o : ""; return *this; }
    String &operator+=(char c) { s_ += c; return *this; }
    bool concat(const char *o) { s_ += o ? o : ""; return true; }
    bool concat(const char *o, unsigned n) { s_.append(o, n); return true; }
    bool concat(char c) { s_ += c; return true; }
    bool operator==(const String &o) const { return s_ == o.s_; }
    bool operator==(const char *o) const { return s_ == (o ? o : ""); }
    char operator[](unsigned i) const { return i < s_.size() ? s_[i] : 0; }
    void reserve(unsigned n) { s_.reserve(n); }
    void clear() { s_.clear(); }
    int indexOf(char c) const { auto p = s_.find(c); return p == std::string::npos ? -1 : (int)p; }
    String substring(unsigned from, unsigned to = 0xffffffffu) const
    {
        if (from > s_.size()) from = (unsigned)s_.size();
        if (to > s_.size()) to = (unsigned)s_.size();
        return String(s_.substr(from, to > from ? to - from : 0));
    }
    friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
    friend String operator+(const String &a, const char *b) { return String(a.s_ + (b ? b : "")); }
private:
    std::string s_;
};
