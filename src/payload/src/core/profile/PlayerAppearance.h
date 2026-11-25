#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dt::appearance {
enum class State { loading, ready, unavailable };
struct CapeRecord { std::string id, firstSeen, lastSeen; bool active = false; };
struct NameRecord { std::string name, changedAt; bool accurate = false; };
struct Snapshot {
    State state = State::loading;
    unsigned skin = 0, cape = 0;
    bool slim = false, legacy = false, hasCape = false;
    std::string message = "Reading player profile...";
    std::vector<CapeRecord> capes;
    std::vector<NameRecord> names;
    bool capeHistoryAvailable = false, nameHistoryAvailable = false;
    unsigned historicalCapeTexture = 0;
    bool historicalCapeLoading = false;
};
// Selection and GL uploads belong to the render thread. Network work never does.
void select(const std::string& name);
const std::string& selectedName();
Snapshot get(const std::string& name, const std::string& uuid,
             const std::string& textures, bool detailsReady, const std::string& historyCape = {});
void shutdown();
} // namespace dt::appearance
