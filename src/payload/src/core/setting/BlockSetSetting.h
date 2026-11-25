// BlockSetSetting.h — an ordered set of Minecraft block identifiers.
//
// Stored form is the fully-qualified identifier string ("minecraft:crafting_table").
// The picker widget in vgui receives the current list + a provider that yields
// every registered block on demand, so the search list stays authoritative even
// as mod loaders add blocks at runtime.

#pragma once
#include "core/setting/Setting.h"

#include <functional>
#include <string>
#include <vector>

namespace dt {

class BlockSetSetting : public Setting {
public:
    BlockSetSetting(Module* owner, std::string name,
                    std::vector<std::string> defaultIds = {});

    const std::vector<std::string>& value() const noexcept { return ids_; }
    void set(std::vector<std::string> v) noexcept          { ids_ = std::move(v); }

    bool contains(const std::string& id) const noexcept;
    void toggle(const std::string& id);
    void clear() noexcept { ids_.clear(); }
    std::size_t size() const noexcept { return ids_.size(); }

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;

private:
    std::vector<std::string> ids_;
};

} // namespace dt
