// AutoTotemModule.cpp — ClickGui glue for AutoTotem.
//
// The state machine runs from GameLoopEvent, which is pumped from the
// wglSwapBuffers detour — and in MC 1.21 that thread is the same OS thread
// that runs the game loop, so the work lands on the game thread with no
// bytecode retransform anywhere.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "core/setting/RangeSetting.h"
#include "util/Log.h"
#include "vgui/bridge.h"   // isOverlayVisible — stand down while the panel is up

#include "autototem.h"
#include "autototem_tick.h"

namespace dt::modules {

class AutoTotem : public Module {
public:
    AutoTotem() : Module("Totem Assist",
                         "Keeps a totem in your offhand",
                         Category::Combat) {
        registerSetting(&mode_);
        registerSetting(&cooldown_);
        registerSetting(&randomization_);
        registerSetting(&openMode_);
        registerSetting(&autoClose_);
        registerSetting(&silentMove_);
        registerSetting(&returnOriginal_);

        using ::dt::modules::autototem::Mode;
        using ::dt::modules::autototem::OpenMode;

        const auto inventoryMode = [this] {
            return mode_.index() == static_cast<int>(Mode::Inventory);
        };

        // Mode-specific rows hide themselves when another mode is picked, so
        // the drawer only ever shows knobs that actually do something.
        openMode_.setVisibleIf(inventoryMode);

        // Auto Close only means anything when the USER opens the inventory —
        // Auto Open already closes after itself unconditionally.
        autoClose_.setVisibleIf([this, inventoryMode] {
            return inventoryMode()
                && openMode_.index() == static_cast<int>(OpenMode::OnOpen);
        });

        silentMove_.setVisibleIf(
            [this] { return mode_.index() == static_cast<int>(Mode::Silent); });

        returnOriginal_.setVisibleIf(
            [this] { return mode_.index() == static_cast<int>(Mode::Hotbar); });
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e){
            mirror();
            ::dt::modules::autototem::ensure_installed();
            ::dt::modules::autototem_tick::pump(e.deltaMs);
        });
    }

    void onEnable() override {
        ::dt::modules::autototem::settings::enabled.store(true, std::memory_order_relaxed);
        mirror();
    }

    void onDisable() override {
        ::dt::modules::autototem::settings::enabled.store(false, std::memory_order_relaxed);
        // Do NOT reset() here. The pump has to run at least once more to hand
        // the movement keys back if a Silent session was live — resetting from
        // this thread would clear the flag that tells it to.
    }

private:
    // Render thread writes, game thread reads — hence the atomic mirror.
    void mirror() {
        using S = ::dt::modules::autototem::settings;

        S::mode.store(mode_.index(),                       std::memory_order_relaxed);
        S::open_mode.store(openMode_.index(),              std::memory_order_relaxed);
        S::auto_close.store(autoClose_.value(),            std::memory_order_relaxed);
        S::return_original.store(returnOriginal_.value(),  std::memory_order_relaxed);
        S::randomization.store(randomization_.value(),     std::memory_order_relaxed);

        // RangeSetting already guarantees min <= max.
        S::reaction_min_ms.store(cooldown_.min(),          std::memory_order_relaxed);
        S::reaction_max_ms.store(cooldown_.max(),          std::memory_order_relaxed);
        S::silent_move_min_ms.store(silentMove_.min(),     std::memory_order_relaxed);
        S::silent_move_max_ms.store(silentMove_.max(),     std::memory_order_relaxed);

        S::overlay_open.store(::dt::vgui::bridge::isOverlayVisible(),
                              std::memory_order_relaxed);
    }

    // Where the totem is taken from.
    //
    //   Hotbar    — totem sits in slots 0-8. Selects its slot and lets
    //               vanilla's F keybind emit the swap. Two packets, both from
    //               vanilla's own call sites, and NO inventory traffic at all.
    //               Since it never sends a ClickSlot, the MultiActions family
    //               has nothing to look at. This is the quiet path.
    //
    //   Inventory — totem sits in the main inventory. Needs the player screen
    //               and a real ClickSlot, which is the exact surface the
    //               inventory-side checks watch.
    //
    //   Silent    — same click, no screen shown, movement suppressed for the
    //               session.
    EnumSetting mode_             {this, "Mode", {"Hotbar", "Inventory", "Silent"}, 0};

    // Reaction bounds in milliseconds; every cycle draws a flat random value
    // in between. Wide beats narrow: a tight range gives consecutive intervals
    // an almost identical spread, which is precisely what the stddev-based
    // totem checks are built to notice.
    ::dt::RangeSetting cooldown_ {this, "Cooldown (ms)", 60, 660, 20, 1500};

    // Master switch: flat random reaction + random totem pick. Off = fixed
    // midpoint and always the leftmost totem.
    BoolSetting randomization_    {this, "Randomization", true};

    // Inventory mode only.
    EnumSetting openMode_         {this, "Open Mode", {"Auto Open", "On Open"}, 0};

    // Inventory + On Open only. Note this gives up On Open's main addtge:
    // closing emits a CloseHandledScreen, the extra traffic On Open avoids.
    BoolSetting autoClose_        {this, "Auto Close", false};

    // Silent mode only: how long movement stays suppressed after the session.
    ::dt::RangeSetting silentMove_ {this, "Silent Move Delay (ms)", 100, 200, 50, 400};

    // Hotbar mode only: switch back to the slot you were holding.

    BoolSetting returnOriginal_   {this, "Return Original", true};
};

DT_REGISTER_MODULE(AutoTotem);

} // namespace dt::modules
