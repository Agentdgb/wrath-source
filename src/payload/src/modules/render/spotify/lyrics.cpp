#include "lyrics.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <vector>

namespace dt::modules::spotify {

namespace {

/* Reads "[mm:ss.xx]" or "[mm:ss]" starting at `i`. Returns false and leaves `i`
   alone when what is there is not a timestamp — an LRC file's metadata tags
   ("[ar:...]", "[offset:...]") look the same until you try to read digits. */
bool read_stamp(const std::string& s, std::size_t& i, long long& out_ms)
{
    if (i >= s.size() || s[i] != '[') return false;

    std::size_t p = i + 1;

    auto digits = [&](int max_n, long long& v) {
        int n = 0;
        v = 0;
        while (p < s.size() && n < max_n && std::isdigit(static_cast<unsigned char>(s[p]))) {
            v = v * 10 + (s[p] - '0');
            ++p; ++n;
        }
        return n > 0;
    };

    long long mm = 0, ss = 0, frac = 0;

    if (!digits(3, mm)) return false;
    if (p >= s.size() || s[p] != ':') return false;
    ++p;
    if (!digits(2, ss) || ss >= 60) return false;

    int frac_digits = 0;
    if (p < s.size() && (s[p] == '.' || s[p] == ':')) {
        ++p;
        const std::size_t before = p;
        if (!digits(3, frac)) return false;
        frac_digits = static_cast<int>(p - before);
    }

    if (p >= s.size() || s[p] != ']') return false;
    ++p;

    /* Two digits after the dot are centiseconds, three are milliseconds. LRC in
       the wild uses both and neither says which. */
    long long frac_ms = 0;
    if (frac_digits == 1)      frac_ms = frac * 100;
    else if (frac_digits == 2) frac_ms = frac * 10;
    else if (frac_digits == 3) frac_ms = frac;

    out_ms = (mm * 60 + ss) * 1000 + frac_ms;
    i = p;
    return true;
}

/* @accents that arrive taken apart, and quotes that are not quotes.

   Two things in third-party lyrics land outside Latin-1, which is all the HUD's
   atlas bakes, and both come out as the missing-glyph mark:

     - Decomposed accents. "ó" can be written as a plain o followed by a
       combining acute (U+0301). The o draws, the accent does not, and the word
       reads as "o?".
     - Typographic punctuation. Curly quotes, en and em dashes and the ellipsis
       are pasted in from wherever the transcription was typed.

   Recomposing the first and folding the second is a small table and it covers
   what actually shows up in practice. Anything still outside the range is left
   alone rather than guessed at. */
std::string sanitize(std::string_view v)
{
    static const struct { unsigned int base; unsigned int mark; unsigned int out; } k_compose[] = {
        { 'a', 0x0301, 0xE1 }, { 'e', 0x0301, 0xE9 }, { 'i', 0x0301, 0xED },
        { 'o', 0x0301, 0xF3 }, { 'u', 0x0301, 0xFA }, { 'n', 0x0303, 0xF1 },
        { 'A', 0x0301, 0xC1 }, { 'E', 0x0301, 0xC9 }, { 'I', 0x0301, 0xCD },
        { 'O', 0x0301, 0xD3 }, { 'U', 0x0301, 0xDA }, { 'N', 0x0303, 0xD1 },
        { 'u', 0x0308, 0xFC }, { 'U', 0x0308, 0xDC }, { 'a', 0x0300, 0xE0 },
        { 'e', 0x0300, 0xE8 }, { 'c', 0x0327, 0xE7 }, { 'C', 0x0327, 0xC7 },
        { 'T', 0x030C, 0x0164 }, { 't', 0x030C, 0x0165 },
        { 'T', 0x0327, 0x0162 }, { 't', 0x0327, 0x0163 },
        { 'T', 0x0326, 0x021A }, { 't', 0x0326, 0x021B },
        { 'T', 0x0307, 0x1E6A }, { 't', 0x0307, 0x1E6B },
        { 'T', 0x0323, 0x1E6C }, { 't', 0x0323, 0x1E6D },
    };

    /* Decode UTF-8 to codepoints, fold, then re-encode. */
    std::vector<unsigned int> cp;
    cp.reserve(v.size());

    for (std::size_t i = 0; i < v.size(); ) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        unsigned int u = c;
        int extra = 0;

        if      (c < 0x80) { extra = 0; u = c; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; u = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; u = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; u = c & 0x07; }
        else { ++i; continue; }               /* stray continuation byte */

        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= v.size()) { ok = false; break; }
            const unsigned char cc = static_cast<unsigned char>(v[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            u = (u << 6) | (cc & 0x3F);
        }

        if (!ok) { ++i; continue; }

        i += static_cast<std::size_t>(extra) + 1;

        /* Fold the punctuation as it goes past. */
        switch (u) {
        case '^': u = '\n'; break; // Translation/verse separator used by some sources.
        case 0x2018: case 0x2019: u = 0x27; break;
        case 0x201C: case 0x201D: u = '"';  break;
        case 0x2013: case 0x2014: u = '-';  break;
        case 0x2026: cp.push_back('.'); cp.push_back('.'); u = '.'; break;
        case 0x00A0: u = ' '; break;
        default: break;
        }

        /* Recompose a combining mark onto whatever it followed. */
        bool composed = false;
        if (!cp.empty()) {
            for (const auto& e : k_compose) {
                if (e.mark == u && cp.back() == e.base) {
                    cp.back() = e.out;
                    composed = true;
                    break;
                }
            }
        }

        if (!composed)
            cp.push_back(u);
    }

    std::string out;
    out.reserve(v.size());

    for (const unsigned int u : cp) {
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else if (u < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }

    return out;
}

std::string trim(std::string_view v)
{
    std::size_t a = 0, b = v.size();
    while (a < b && (v[a] == ' ' || v[a] == '\t' || v[a] == '\r')) ++a;
    while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t' || v[b - 1] == '\r')) --b;
    return sanitize(v.substr(a, b - a));
}

void push_line(lyrics_doc& doc, long long t_ms, const std::string& text)
{
    lyric_line l{};
    l.t_ms = t_ms;
    l.off  = static_cast<std::uint32_t>(doc.blob.size());
    l.len  = static_cast<std::uint32_t>(text.size());
    doc.blob += text;
    doc.lines.push_back(l);
}

} // namespace

int lyrics_doc::index_at(long long ms) const
{
    if (lines.empty()) return -1;
    if (ms < lines.front().t_ms) return -1;

    const auto it = std::upper_bound(lines.begin(), lines.end(), ms,
        [](long long v, const lyric_line& l) { return v < l.t_ms; });

    return static_cast<int>(std::distance(lines.begin(), it)) - 1;
}

lyrics_doc parse_lrc(const std::string& body, const std::string& key)
{
    lyrics_doc doc;
    doc.key = key;
    doc.blob.reserve(body.size());
    doc.lines.reserve(128);

    std::size_t pos = 0;
    long long offset = 0;

    while (pos <= body.size()) {
        std::size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();

        const std::string_view raw(body.data() + pos, nl - pos);
        pos = nl + 1;
        if (raw.starts_with("[offset:")) {
            const auto end = raw.find(']');
            if (end != std::string_view::npos) {
                long long value = 0;
                auto number = raw.substr(8, end - 8);
                if (number.starts_with("+")) number.remove_prefix(1);
                const auto result = std::from_chars(number.data(), number.data()+number.size(), value);
                if (result.ec == std::errc{} && result.ptr == number.data()+number.size())
                    offset = std::clamp(value, -600000LL, 600000LL);
            }
            continue;
        }

        /* A line can carry several timestamps — a repeated chorus is written
           once with every time it occurs stacked at the front. */
        std::size_t i = 0;
        std::vector<long long> stamps;

        for (;;) {
            long long t = 0;
            std::size_t probe = i;
            std::string tmp(raw);
            if (!read_stamp(tmp, probe, t)) break;
            stamps.push_back(t);
            i = probe;
        }

        if (stamps.empty()) continue;   /* metadata tag, or plain prose */

        const std::string text = trim(raw.substr(i));

        for (const long long t : stamps)
            push_line(doc, t, text);

        if (nl == body.size()) break;
    }

    std::stable_sort(doc.lines.begin(), doc.lines.end(),
        [](const lyric_line& a, const lyric_line& b) { return a.t_ms < b.t_ms; });

    // Co-timed translations/duets form one displayed phrase, not overlapping
    // rows with only the final row highlighted. Ignore duplicate empty beats.
    lyrics_doc merged;
    merged.key = key;
    for (std::size_t i = 0; i < doc.lines.size();) {
        const auto stamp = doc.lines[i].t_ms;
        std::string phrase;
        do {
            const auto text = doc.text(i++);
            if (!text.empty() && text != phrase) {
                if (!phrase.empty()) phrase += '\n';
                phrase += text;
            }
        } while (i < doc.lines.size() && doc.lines[i].t_ms == stamp);
        push_line(merged, stamp - offset, phrase);
    }
    doc = std::move(merged);

    doc.synced = !doc.lines.empty();
    return doc;
}

lyrics_doc parse_plain(const std::string& body, const std::string& key)
{
    lyrics_doc doc;
    doc.key = key;
    doc.blob.reserve(body.size());

    std::size_t pos = 0;

    while (pos <= body.size()) {
        std::size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();

        push_line(doc, 0, trim(std::string_view(body.data() + pos, nl - pos)));

        if (nl == body.size()) break;
        pos = nl + 1;
    }

    doc.synced = false;
    return doc;
}

} // namespace dt::modules::spotify
