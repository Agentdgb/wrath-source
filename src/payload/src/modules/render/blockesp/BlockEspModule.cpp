// BlockEspModule.cpp — ClickGui glue for the BlockESP engine.
//
// Follows the same shape as ChamsModule / AnimationsModule: the engine
// (blockesp.cpp) owns all state, JNI, and drawing; this class only owns
// the settings and mirrors them into the engine's settings POD each tick.

#include "core/event/Events.h"
#include "core/module/Module.h"
#include "core/setting/BlockSetSetting.h"
#include "core/setting/BoolSetting.h"
#include "core/setting/ColorSetting.h"
#include "core/setting/EnumSetting.h"
#include "core/setting/NumericSetting.h"
#include "jvm/JvmContext.h"

#include "modules/render/blockesp/blockesp.h"

namespace dt::modules {

class BlockEsp : public Module {
public:
    BlockEsp() : Module("Block Outline",
                        "Highlight blocks in the world by identifier. Anti-xray "
                        "resistant via packet-reveal cache.",
                        Category::Visuals) {
        registerSetting(&blocks_);
        registerSetting(&range_);
        registerSetting(&fillAlpha_);
    }

    ~BlockEsp() override { blockesp::shutdown(); }

    void onInit() override {
        blockesp::install();

        listen<events::GameLoopEvent>([this](events::GameLoopEvent&) {
            mirror();
            ::dt::jvm::JniScope s;
            if (s) blockesp::tick(s.get());
        });
    }

    void onEnable() override {
        mirror();
        blockesp::g_enabled.store(true, std::memory_order_relaxed);
        // Seed the palette-walker with every currently-loaded chunk in a
        // spiral around the player, so we don't have to wait for chunk-load
        // packets to build up the cache. Time-budget in tick keeps this
        // from spiking FPS on enable.
        blockesp::enqueue_loaded_chunks();
    }
    void onDisable() override { blockesp::g_enabled.store(false, std::memory_order_relaxed); }

private:
    void mirror() {
        std::scoped_lock lk(blockesp::g_settings_mx);
        auto& g = blockesp::g_settings;
        g.range         = range_.value();
        g.fill_alpha    = fillAlpha_.value();
        if (g.target_ids != blocks_.value()) {
            g.target_ids = blocks_.value();
        }
    }

    // Removed on purpose:
    //   ColorMode / Custom — colour is always the block's own (curated mineral
    //     colour, then MapColor, then identifier hash). A single user-picked
    //     colour made every selected block render identically.
    //   PersistReveal — only drove a 5 s decay inside live_scan, which is
    //     gated off, so it did nothing. Entries now persist unconditionally.
    //   ClearCache — redundant. World/server change, target change, movement
    //     prune and the nearby refresh already cover cache invalidation.
    //   AntiXray — its three modes stopped meaning three different things
    //     once the live scanner was gated off: "Live only" and "Live +
    //     reveal" behaved identically, and only "Reveal only" changed
    //     anything (it disabled the packet listener, i.e. made the module
    //     worse). Packet reveals are now unconditionally on.
    //   LineWidth — pinned to a 1 px hairline. Every larger value made a
    //     distant block's twelve strokes overlap into a blob, and integer
    //     widths pushed ImGui onto a texture-sampling line path that read
    //     from other modules' atlases on the shared draw list.
    BlockSetSetting blocks_      {this, "Blocks"};
    IntSetting      range_       {this, "Range", 48, 8, 128};
    FloatSetting    fillAlpha_   {this, "FillAlpha", 0.25f, 0.f, 1.f, 0.05f};
    // No Debug switch in the panel, by request. The engine's desktop log
    // (blockesp::g_debug) stays off; flip it in code when diagnosing.
};

DT_REGISTER_MODULE(BlockEsp);

} // namespace dt::modules
