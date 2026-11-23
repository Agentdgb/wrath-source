#include "core/module/Module.h"
#include "vgui/vgui.h"

namespace vanta::modules {

class StreamProof : public Module {
public:
    StreamProof()
        : Module("StreamProof",
                 "Hides all client visuals from screenshots, OBS, Discord and Game Bar",
                 Category::Misc) {}

protected:
    void onEnable() override {
        vanta::vgui::setStreamProof(true);
    }

    void onDisable() override {
        vanta::vgui::setStreamProof(false);
    }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::StreamProof)
