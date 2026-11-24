#pragma once
#include "core/event/EventBus.h"
#include "core/event/Events.h"
#include "core/event/Priority.h"
#include "core/module/Category.h"
#include "core/setting/BoolSetting.h"
#include "core/setting/ColorSetting.h"
#include "core/setting/EnumSetting.h"
#include "core/setting/KeybindSetting.h"
#include "core/setting/NumericSetting.h"
#include "core/setting/Setting.h"
#include "core/setting/StringSetting.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vanta {

class Module {
public:
    Module(std::string name, std::string desc, Category cat, int defaultKey = 0);
    virtual ~Module();
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    // Called by ModuleManager during ModuleManager::initAll. Modules must not
    // touch JNI in the ctor because bootstrap order does not guarantee an env.
    virtual void onInit() {}

    void toggle();
    void setEnabled(bool e);
    bool isEnabled() const noexcept { return enabled_.load(); }

    std::string_view name()        const noexcept { return name_; }
    std::string_view description() const noexcept { return desc_; }
    Category         category()    const noexcept { return category_; }
    KeybindSetting&  keybind()     noexcept { return keybind_; }

    const std::vector<Setting*>& settings() const noexcept { return settings_; }

    // Optional short secondary text for the ArrayList, shown in gray when
    // Show Module Settings is enabled. Empty falls back to the first visible
    // enum. Called every frame the ArrayList draws, so keep it cheap.
    virtual std::string displayInfo() const { return {}; }
    // Stateful actions keep a Click bind active until their interaction completes.
    virtual bool deferredClick() const noexcept { return false; }

    // Default drawImGui iterates settings; modules override to add custom rows.
    virtual void drawImGui();

protected:
    void setDescription(std::string description) {
        std::lock_guard state(clientStateMutex());
        desc_ = std::move(description);
    }
    virtual void onEnable()  {}
    virtual void onDisable() {}

    void registerSetting(Setting* s);

    // listen<E>(handler[, priority]) — RAII: unsubscribed automatically when
    // the module is destroyed.
    template <class E, class Fn>
    void listen(Fn&& fn, Priority p = Priority::Normal) {
        listeners_.push_back(EventBus::get().subscribe<E>(std::forward<Fn>(fn), p));
    }

    template <class E, class Self>
    void listen(void (Self::*m)(E&), Priority p = Priority::Normal) {
        auto* self = static_cast<Self*>(this);
        listeners_.push_back(EventBus::get().subscribe<E>([self, m](E& e){ (self->*m)(e); }, p));
    }

private:
    std::string                                  name_;
    std::string                                  desc_;
    Category                                     category_;
    std::atomic<bool>                            enabled_{false};
    std::vector<Setting*>                        settings_;
    std::vector<std::unique_ptr<IListenerToken>> listeners_;
    KeybindSetting                               keybind_;
};

// ─── Registration ─────────────────────────────────────────────────────────────
// Each module TU calls VANTA_REGISTER_MODULE(YourType) at global scope; this
// pre-main static-init pushes the factory into ModuleManager.
using ModuleFactory = std::unique_ptr<Module>(*)();

void _register_module_factory(ModuleFactory f);

// Global-scope static-init factory registration. The identifier is built from
// __LINE__ + __COUNTER__ so a fully-qualified type like
// `vanta::modules::AimAssist` works — a naive `_reg_##T` would produce an
// invalid identifier containing `::`.
#define VANTA_CAT2_(a, b) a##b
#define VANTA_CAT_(a, b)  VANTA_CAT2_(a, b)
#define VANTA_REGISTER_MODULE(T)                                                    \
    namespace {                                                                     \
        static const int VANTA_CAT_(_vanta_reg_mod_, __COUNTER__) = [](){           \
            ::vanta::_register_module_factory([]() -> std::unique_ptr<::vanta::Module> { \
                return std::make_unique<T>();                                       \
            });                                                                     \
            return 0;                                                               \
        }();                                                                        \
    }

} // namespace vanta
