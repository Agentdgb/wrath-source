#pragma once
#include "core/setting/Setting.h"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

namespace vanta {

// String-backed enum. Modules typically declare a static array of option names
// and pass indices in/out.
class EnumSetting : public Setting {
public:
    EnumSetting(Module* owner, std::string name,
                std::initializer_list<const char*> options, int defaultIndex = 0);

    // Same setting, options handed over as a run of pointers rather than a
    // braced literal. An initializer_list can only be written out by hand, so a
    // setting whose options come from somewhere else — a font pool, a config, a
    // registry — either duplicates that list here or dangles trying to return
    // one from a helper. This overload is how such a setting is declared.
    EnumSetting(Module* owner, std::string name,
                const char* const* options, std::size_t count, int defaultIndex = 0);

    int  index() const noexcept { return index_; }
    void set(int i) noexcept    { index_ = options_.empty() ? 0 : (i < 0 ? 0 : (i >= static_cast<int>(options_.size()) ? static_cast<int>(options_.size()) - 1 : i)); }

    std::string_view current() const noexcept { return options_.empty() ? std::string_view{} : std::string_view(options_[index_]); }

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;

    template <class E>
    void setEnum(E e) { set(static_cast<int>(e)); }
    template <class E>
    E   asEnum() const { return static_cast<E>(index_); }
private:
    std::vector<std::string> options_;
    int                      index_;
};

} // namespace vanta
