// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/appui/text.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <utility>

namespace bt::appui {
namespace {
#include "display_text.inc"
} // namespace

bool display_text_refused(uint32_t cp)
{
    if (cp > 0x10ffff) return true;
    // The last range that starts at or before cp.
    auto it{std::upper_bound(std::begin(DISPLAY_TEXT_REFUSED), std::end(DISPLAY_TEXT_REFUSED), cp,
                             [](uint32_t c, const std::pair<uint32_t, uint32_t>& r) { return c < r.first; })};
    if (it == std::begin(DISPLAY_TEXT_REFUSED)) return false;
    --it;
    return cp <= it->second;
}

const char* display_text_unicode_version() { return DISPLAY_TEXT_UNICODE; }

std::string display_text_problem(std::string_view s)
{
    for (size_t i{0}; i < s.size();) {
        const auto c{static_cast<unsigned char>(s[i])};
        uint32_t cp;
        size_t n;
        if (c < 0x80) {
            cp = c, n = 1;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f, n = 2;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f, n = 3;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07, n = 4;
        } else {
            return "Not valid UTF-8 text";
        }
        if (i + n > s.size()) return "Not valid UTF-8 text";
        for (size_t k{1}; k < n; ++k) {
            const auto d{static_cast<unsigned char>(s[i + k])};
            if ((d & 0xc0) != 0x80) return "Not valid UTF-8 text";
            cp = (cp << 6) | (d & 0x3f);
        }
        const bool overlong{(n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)};
        if (overlong || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return "Not valid UTF-8 text";
        if (display_text_refused(cp)) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "U+%04X", static_cast<unsigned>(cp));
            return std::string{"No control, format or invisible character ("} + hex + ")";
        }
        i += n;
    }
    return {};
}

std::string display_text_shown(std::string_view s)
{
    if (display_text_problem(s).empty()) return std::string{s};
    std::string out;
    char tag[16];
    for (size_t i{0}; i < s.size();) {
        const auto c{static_cast<unsigned char>(s[i])};
        size_t n{c < 0x80 ? 1u : (c & 0xe0) == 0xc0 ? 2u : (c & 0xf0) == 0xe0 ? 3u : (c & 0xf8) == 0xf0 ? 4u : 0u};
        bool ok{n > 0 && i + n <= s.size()};
        uint32_t cp{n == 1 ? c : n == 2 ? c & 0x1fu : n == 3 ? c & 0x0fu : c & 0x07u};
        for (size_t k{1}; ok && k < n; ++k) {
            const auto d{static_cast<unsigned char>(s[i + k])};
            ok = (d & 0xc0) == 0x80;
            cp = (cp << 6) | (d & 0x3f);
        }
        if (ok) ok = !((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff));
        if (!ok) {
            std::snprintf(tag, sizeof(tag), "<0x%02X>", static_cast<unsigned>(c));
            out += tag;
            ++i;
            continue;
        }
        if (display_text_refused(cp)) {
            std::snprintf(tag, sizeof(tag), "<U+%04X>", static_cast<unsigned>(cp));
            out += tag;
        } else {
            out.append(s.substr(i, n));
        }
        i += n;
    }
    return out;
}

} // namespace bt::appui
