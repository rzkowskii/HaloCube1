#ifndef HALO_STATE_H
#define HALO_STATE_H

// The saved-game state block of config/halo-progress.txt (version 3): one record per line, fields
// separated by single spaces. Numbers are written so they read back exactly (%.9g for floats,
// %.17g for doubles); text fields are "=" followed by the text with every byte at or below a
// space, '%', '=' and every byte above '~' written as %XX, so a field never holds a space or a
// newline. halo.cpp owns the block (its header, line count and checksum); halo_script.cpp,
// halo_vehicle.cpp and halo.cpp each write and read their own records.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

inline void halostatef(std::string &out, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
inline void halostatef(std::string &out, const char *fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    const int length = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if(length < 0) return;
    if(size_t(length) < sizeof(line)) { out.append(line, size_t(length)); return; }
    std::vector<char> big(size_t(length) + 1);
    va_start(args, fmt);
    vsnprintf(big.data(), big.size(), fmt, args);
    va_end(args);
    out.append(big.data(), size_t(length));
}

// A text field: " =<escaped>".
inline void halostateword(std::string &out, const std::string &text)
{
    static const char hex[] = "0123456789ABCDEF";
    out += " =";
    for(unsigned char c : text)
    {
        if(c <= ' ' || c > '~' || c == '%' || c == '=') { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
}

// FNV-1a, as the progress file's check line uses.
inline unsigned halostatehash(const char *text, size_t length, unsigned hash = 2166136261u)
{
    for(size_t i = 0; i < length; ++i) { hash ^= (unsigned char)text[i]; hash *= 16777619u; }
    return hash;
}

// Reads the fields of one record in order; any malformed field clears `ok`, after which every
// read returns 0 or "" (callers check `ok` once at the end of the record).
struct HaloStateReader
{
    const char *p;
    bool ok = true;

    explicit HaloStateReader(const char *line) : p(line) {}

    // The next field, or null at the end of the line.
    const char *field(size_t &length)
    {
        if(!ok) return nullptr;
        while(*p == ' ') ++p;
        if(!*p || *p == '\n') { ok = false; return nullptr; }
        const char *start = p;
        while(*p && *p != ' ' && *p != '\n') ++p;
        length = size_t(p - start);
        return start;
    }

    bool more() { while(*p == ' ') ++p; return ok && *p && *p != '\n'; }

    long long integer()
    {
        size_t n;
        const char *f = field(n);
        if(!f) return 0;
        char *end = nullptr;
        const long long value = strtoll(f, &end, 10);
        if(end != f + n) { ok = false; return 0; }
        return value;
    }
    int i() { const long long v = integer(); if(v < -2147483647LL - 1 || v > 2147483647LL) { ok = false; return 0; } return int(v); }
    bool b() { const int v = i(); if(v != 0 && v != 1) ok = false; return v == 1; }
    unsigned x()
    {
        size_t n;
        const char *f = field(n);
        if(!f) return 0;
        char *end = nullptr;
        const unsigned long value = strtoul(f, &end, 16);
        if(end != f + n || value > 0xffffffffUL) { ok = false; return 0; }
        return unsigned(value);
    }
    double d()
    {
        size_t n;
        const char *f = field(n);
        if(!f) return 0;
        char *end = nullptr;
        const double value = strtod(f, &end);
        // strtod reads "nan" and "inf"; no saved number is either, so a damaged record fails.
        if(end != f + n || !std::isfinite(value)) { ok = false; return 0; }
        return value;
    }
    float f()
    {
        const double v = d();
        if(v < -3.0e38 || v > 3.0e38) { ok = false; return 0; }
        return float(v);
    }
    std::string word()
    {
        size_t n;
        const char *f = field(n);
        if(!f) return std::string();
        if(*f != '=') { ok = false; return std::string(); }
        std::string out;
        for(size_t k = 1; k < n; ++k)
        {
            if(f[k] != '%') { out += f[k]; continue; }
            if(k + 2 >= n) { ok = false; return std::string(); }
            char digits[3] = {f[k + 1], f[k + 2], 0};
            char *end = nullptr;
            const long c = strtol(digits, &end, 16);
            if(end != digits + 2) { ok = false; return std::string(); }
            out += char(c);
            k += 2;
        }
        return out;
    }
    // True when the record's key is `key` (the first field), which is then consumed.
    bool key(const char *key)
    {
        const size_t n = strlen(key);
        const char *at = p;
        while(*at == ' ') ++at;
        if(strncmp(at, key, n) || (at[n] != ' ' && at[n] != '\n' && at[n])) return false;
        p = at + n;
        return true;
    }
};

#endif
