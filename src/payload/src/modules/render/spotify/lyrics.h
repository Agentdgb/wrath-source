// lyrics.h — time-synced lyrics for the Spotify HUD.
//
// @why not Spotify's own API.
//
// It does not have one. Spotify's lyrics are licensed from Musixmatch and are
// served only by an internal endpoint that wants a user token minted from a
// browser session cookie — not the client-credentials token this worker already
// mints, which cannot reach it at all. Using it would mean shipping a
// reverse-engineered auth flow against an undocumented host that Spotify
// changes without notice, and it would break on their schedule rather than
// ours.
//
// LRCLIB is the honest alternative: no account, no key, no token, and it
// returns LRC — lyrics with a timestamp on every line. That last part is the
// whole point, because it is what makes the highlight track the song instead of
// lagging behind it.
//
// @where the "delay" actually comes from, and how it is removed.
//
// Not from the network — the whole file is fetched once when the track changes
// and then lives in memory. The lag in a lyrics view comes from asking the
// player where it is: SMTC reports a position about twice a second, so a view
// that highlights straight off the last poll is up to half a second stale and
// visibly behind the vocal. The card already extrapolates a smooth position
// between polls for its progress bar, and the highlight reads that same clock —
// so it moves every frame, not twice a second.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dt::modules::spotify {

struct lyric_line
{
    long long     t_ms = 0;   /* when this line starts */
    std::uint32_t off  = 0;   /* into lyrics_doc::blob */
    std::uint32_t len  = 0;
};

/* @one string, and offsets into it.

   Not a vector of std::string: the draw pass walks this every frame to find the
   current line, and a few hundred separate allocations is a cache miss per line
   for no benefit. The text never changes once parsed. */
struct lyrics_doc
{
    std::string             blob;
    std::vector<lyric_line> lines;    /* sorted by t_ms */

    /* False when the source only had a plain text version. The lines are still
       there and still readable; they just cannot be followed. */
    bool synced = false;

    /* Which track this belongs to, so a doc that arrives after the user has
       skipped on is not shown against the wrong song. */
    std::string key;

    bool empty() const { return lines.empty(); }

    std::string_view text(std::size_t i) const
    {
        if (i >= lines.size()) return {};
        return std::string_view(blob).substr(lines[i].off, lines[i].len);
    }

    /* @the line that should be lit at `ms`, or -1 before the first one.

       Binary search rather than a walk: it runs once a frame against a list
       that can be a few hundred lines long. */
    int index_at(long long ms) const;
};

/* @parse:
    builds a doc from an LRC body, or from plain text when `synced` is false.

    Tolerant on purpose — this is third-party data. Lines with no timestamp are
    dropped from a synced parse, a line may carry several timestamps and is
    emitted once per timestamp, and a malformed bracket is skipped rather than
    aborting the file. */
lyrics_doc parse_lrc(const std::string& body, const std::string& key);
lyrics_doc parse_plain(const std::string& body, const std::string& key);

} // namespace dt::modules::spotify
