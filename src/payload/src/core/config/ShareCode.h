#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace dt::config {
struct SharedProfile { std::string name; nlohmann::json settings; };
// Self-contained, versioned codes. The random identifier is not a server key.
std::string encodeShareCode(const SharedProfile& profile);
SharedProfile decodeShareCode(std::string_view code);
}
