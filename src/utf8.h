#ifndef UTF8_H
#define UTF8_H

#include <string>
#include <nlohmann/json.hpp>

inline bool utf8_valid(const std::string& s) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        size_t need = 0;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) need = 2;
        else if (c >= 0xF0 && c <= 0xF4) need = 3;
        else return false;
        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        if ((c == 0xE0 && b1 < 0xA0) ||
            (c == 0xED && b1 > 0x9F) ||
            (c == 0xF0 && b1 < 0x90) ||
            (c == 0xF4 && b1 > 0x8F)) return false;
        i += need + 1;
    }
    return true;
}

// Replaces every invalid UTF-8 byte with U+FFFD. Output is always valid UTF-8.
inline std::string utf8_sanitize(const std::string& s) {
    static const std::string kReplacement = "\xEF\xBF\xBD";
    if (utf8_valid(s)) return s;
    std::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        size_t need = 0;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) need = 2;
        else if (c >= 0xF0 && c <= 0xF4) need = 3;
        else {
            out += kReplacement;
            ++i;
            continue;
        }
        if (i + need >= n) {
            out += kReplacement;
            break;
        }
        bool ok = true;
        for (size_t k = 1; k <= need; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { ok = false; break; }
        }
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        if (ok) {
            if ((c == 0xE0 && b1 < 0xA0) ||
                (c == 0xED && b1 > 0x9F) ||
                (c == 0xF0 && b1 < 0x90) ||
                (c == 0xF4 && b1 > 0x8F)) ok = false;
        }
        if (!ok) {
            out += kReplacement;
            ++i;
            continue;
        }
        out.append(s, i, need + 1);
        i += need + 1;
    }
    return out;
}

inline void sanitize_json_strings(nlohmann::json& j) {
    if (j.is_string()) {
        j = utf8_sanitize(j.get_ref<const std::string&>());
    } else if (j.is_array()) {
        for (auto& value : j) sanitize_json_strings(value);
    } else if (j.is_object()) {
        for (auto& item : j.items()) sanitize_json_strings(item.value());
    }
}

#endif