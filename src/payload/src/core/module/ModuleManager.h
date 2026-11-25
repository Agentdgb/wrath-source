#pragma once
#include "core/module/Category.h"
#include "core/module/Module.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace dt {

class ModuleManager {
public:
    static ModuleManager& get();

    static void registerFactory(ModuleFactory f);

    void initAll();
    void shutdownAll();

    const std::vector<std::unique_ptr<Module>>& all()      const noexcept { return modules_; }
    std::vector<Module*>                        byCategory(Category c) const;
    Module*                                     find(std::string_view name) const;

    nlohmann::json saveAll() const;
    void           loadAll(const nlohmann::json& j);

    // Panic — disable every module. Bound to right-shift by default.
    void panic();
    bool isPanicking() const noexcept { return panicking_.load(); }
    bool hasPanicked() const noexcept { return panicked_.load(); }

private:
    std::vector<std::unique_ptr<Module>> modules_;
    std::atomic<bool> panicking_{false};
    std::atomic<bool> panicked_{false};
};

} // namespace dt
