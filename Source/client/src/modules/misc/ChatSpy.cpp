#include "core/event/Events.h"
#include "core/module/Module.h"
#include "jvm/PacketAccess.h"
#include "hook/PacketEvents.h"
#include "platform/Win32Process.h"
#include "util/Log.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace vanta::modules {

class ChatSpy : public Module {
public:
    ChatSpy() : Module("ChatSpy",
                       "Logs every chat message to a file",
                       Category::Misc) {
        registerSetting(&logInbound_);
        registerSetting(&logOutbound_);
        registerSetting(&fileName_);
    }

    void onInit() override {
        listen<events::GameLoopEvent>([this] {
            if (isEnabled() && hook::packets::unavailable()) {
                setDescription("Unavailable: packet event hooks could not be installed on this runtime");
                setEnabled(false);
            }
        });
        listen<events::PacketSendEvent>([this](events::PacketSendEvent& e){
            if (!isEnabled() || !logOutbound_.value()) return;
            auto msg = vanta::jvm::packet::chatOutgoingMessage(e.env, e.packet);
            if (!msg.empty()) writeLine("> " + msg);
            auto cmd = vanta::jvm::packet::chatOutgoingCommand(e.env, e.packet);
            if (!cmd.empty()) writeLine(">/ " + cmd);
        }, Priority::Monitor);

        listen<events::PacketReceiveEvent>([this](events::PacketReceiveEvent& e){
            if (!isEnabled() || !logInbound_.value()) return;
            auto s = vanta::jvm::packet::chatIncomingPlainText(e.env, e.packet);
            if (!s.empty()) writeLine("< " + s);
        }, Priority::Monitor);
    }

private:
    BoolSetting   logInbound_ {this, "LogInbound",  true};
    BoolSetting   logOutbound_{this, "LogOutbound", true};
    StringSetting fileName_   {this, "File", "chat.log", 260};
    std::mutex    mx_;

    void writeLine(const std::string& line) {
        std::scoped_lock lk(mx_);
        auto path = std::filesystem::path(vanta::platform::appDataDir()) / fileName_.value();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::app);
        out << line << '\n';
    }
};

} // namespace vanta::modules

VANTA_REGISTER_MODULE(vanta::modules::ChatSpy)
