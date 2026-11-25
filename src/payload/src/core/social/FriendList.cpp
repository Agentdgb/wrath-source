#include "core/social/FriendList.h"

#include "core/config/ConfigManager.h"
#include "util/Log.h"

#include <algorithm>

namespace dt {

namespace {

/* ASCII only, deliberately. Minecraft profile names are [A-Za-z0-9_]{3,16} and
   the tab list hands us exactly that, so std::tolower's locale dependence buys
   nothing here and would make the fold differ between machines. */
inline char fold(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

FriendList& FriendList::get()
{
    static FriendList g;
    return g;
}

std::uint64_t FriendList::fold_hash(std::string_view s) noexcept
{
    std::uint64_t h = 1469598103934665603ull;             // FNV-1a 64
    for (char c : s) {
        h ^= static_cast<unsigned char>(fold(c));
        h *= 1099511628211ull;
    }
    return h;
}

bool FriendList::equal_folded(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (fold(a[i]) != fold(b[i])) return false;
    return true;
}

void FriendList::reindex()
{
    index_.clear();
    index_.reserve(names_.size());
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(names_.size()); ++i)
        index_.push_back(Index{ fold_hash(names_[i]), i });

    std::sort(index_.begin(), index_.end(),
              [](const Index& a, const Index& b) { return a.hash < b.hash; });

    count_.store(names_.size(), std::memory_order_relaxed);
    gen_.fetch_add(1, std::memory_order_relaxed);
}

bool FriendList::contains(std::string_view name) const
{
    if (name.empty() || count_.load(std::memory_order_relaxed) == 0) return false;

    const std::uint64_t h = fold_hash(name);

    std::shared_lock<std::shared_mutex> lk(mx_);
    auto it = std::lower_bound(index_.begin(), index_.end(), h,
                               [](const Index& e, std::uint64_t v) { return e.hash < v; });

    /* The hash is the probe; the string is the proof. Skipping the confirm would
       make a 64-bit collision spare a stranger from every module at once, and it
       would be unreproducible. One 16-byte compare, only on a hit. */
    for (; it != index_.end() && it->hash == h; ++it)
        if (equal_folded(names_[it->slot], name)) return true;

    return false;
}

bool FriendList::set(std::string_view name, bool value)
{
    if (name.empty()) return false;

    {
        std::unique_lock<std::shared_mutex> lk(mx_);

        auto it = std::find_if(names_.begin(), names_.end(),
                               [&](const std::string& n) { return equal_folded(n, name); });
        const bool present = (it != names_.end());
        if (present == value) return false;

        if (value) names_.emplace_back(name);
        else       names_.erase(it);

        reindex();
    }

    VLOG_INFO("friends", "{} '{}' ({} total)", value ? "added" : "removed",
              std::string(name), count_.load(std::memory_order_relaxed));

    /* Coalesced by the autosave thread, exactly the way Setting::draw does it.
       No-op before ConfigManager::init(). */
    ConfigManager::get().markDirty();
    return true;
}

std::vector<std::string> FriendList::all() const
{
    std::shared_lock<std::shared_mutex> lk(mx_);
    return names_;
}

void FriendList::replaceAll(std::vector<std::string> names)
{
    {
        std::unique_lock<std::shared_mutex> lk(mx_);
        names_.clear();
        names_.reserve(names.size());
        for (auto& n : names) {
            if (n.empty()) continue;
            const bool dup = std::any_of(names_.begin(), names_.end(),
                [&](const std::string& e) { return equal_folded(e, n); });
            if (!dup) names_.push_back(std::move(n));
        }
        reindex();
    }

    VLOG_INFO("friends", "loaded {} friend(s)", count_.load(std::memory_order_relaxed));
}

} // namespace dt
