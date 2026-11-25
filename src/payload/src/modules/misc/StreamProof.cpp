#include "core/module/Module.h"
#include "vgui/vgui.h"

namespace dt::modules {

class StreamProof : public Module {
public:
    StreamProof()
        : Module("StreamProof",
                 "Hides all client visuals from screenshots, OBS, Discord and Game Bar",
                 Category::Misc) {}

protected:
    void onEnable() override {
        dt::vgui::setStreamProof(true);
    }

    void onDisable() override {
        dt::vgui::setStreamProof(false);
    }
};

} // namespace dt::modules

DT_REGISTER_MODULE(dt::modules::StreamProof)
