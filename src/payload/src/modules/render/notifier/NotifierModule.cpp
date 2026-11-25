// NotifierModule.cpp — the Visuals module wrapper for the toggle toasts.

#include "notifier.h"

#include "core/module/Module.h"
#include "core/module/ModuleManager.h"

#include <string>

namespace dt::modules {

class Notifier : public Module {
public:
    Notifier()
        : Module("Notifier",
                 "Pops up when a module toggles",
                 Category::Visuals)
    {
        registerSetting(&scale_);
        registerSetting(&rounding_);
        registerSetting(&duration_);

        notifier::g_settings_provider = [this](notifier::settings& out) {
            out.scale = scale_.value();
            out.rounding = rounding_.value();
            out.duration = duration_.value();
        };

        /* @the event already existed, which is why there is no hook to add.

           Module::setEnabled has posted a ModuleToggleEvent since long before
           this module: it carries the name and the new state, and the category
           is one lookup away. Subscribing is strictly better than patching a
           callback into setEnabled -- it cannot be forgotten by a future module,
           and it costs the toggle path nothing when nobody is listening.

           The subscription lives for the module's lifetime, not just while it is
           on, so the enabled test has to be here. */
        listen<events::ModuleToggleEvent>([this](events::ModuleToggleEvent& e) {
            if (!isEnabled() || e.moduleName == nullptr)
                return;

            const std::string name{ e.moduleName };

            /* @its own "Disabled" is deliberately never shown.

               setEnabled clears the flag before it posts, so by the time this
               runs isEnabled() is already false and the test above has dropped
               it. That is the right outcome rather than a missing case: a module
               announcing things after being switched off is exactly what being
               switched off is supposed to stop -- and onDisable sends whatever is
               still on screen out through its fade. */
            std::string category{ "Misc" };

            if (const Module* m = ModuleManager::get().find(name))
                category = std::string{ toString(m->category()) };

            notifier::push(std::move(category), name, e.nowEnabled);
        });
    }

    ~Notifier() override
    {
        notifier::g_settings_provider = {};
        notifier::g_enabled = false;
    }

private:
    FloatSetting scale_    { this, "Scale",    1.0f, 0.5f, 2.5f,  0.05f };
    FloatSetting rounding_ { this, "Rounding", 1.0f, 0.0f, 2.0f,  0.1f  };
    FloatSetting duration_ { this, "Duration", 2.5f, 0.5f, 10.0f, 0.5f  };

    void onEnable() override
    {
        notifier::g_enabled = true;
    }

    void onDisable() override
    {
        notifier::g_enabled = false;

        /* Switched off means nothing left announcing itself. Without this the
           cards already up keep running their whole life -- ten seconds at the
           top of the slider -- after the module that owns them is gone. Every
           other HUD module here clears on disable; this one has to as well. */
        notifier::reset();
    }
};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::Notifier)
