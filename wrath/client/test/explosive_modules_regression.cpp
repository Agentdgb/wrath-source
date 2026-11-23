// Compile the real modules and settings against an isolated, offline host.
#include "../src/modules/combat/crystal/ExplosiveModules.cpp"

#include <iostream>
#include <limits>
#include <stdexcept>

float alpha_text = 1.f;

namespace {
bool overlay = false;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
jboolean JNICALL sameObject(JNIEnv*, jobject a, jobject b) { return a == b ? JNI_TRUE : JNI_FALSE; }
}

// The fixture substitutes the app/JVM/rotation host, not the explosive module
// constructors, registered settings, pause path or pure action policies.
namespace vanta {
Module::Module(std::string name, std::string description, Category category, int key)
    : name_(std::move(name)), desc_(std::move(description)), category_(category),
      keybind_(this, "Bind", key, BindMode::Toggle) { registerSetting(&keybind_); }
Module::~Module() = default;
void Module::registerSetting(Setting* setting) { settings_.push_back(setting); }
void Module::setEnabled(bool value) { enabled_ = value; if (value) onEnable(); else onDisable(); }
void Module::toggle() { setEnabled(!isEnabled()); }
void Module::drawImGui() {}
void _register_module_factory(ModuleFactory) {}
}
namespace vanta::jvm {
JniScope::JniScope() = default;
JniScope::~JniScope() = default;
jclass findClassAnyLoader(JNIEnv*, const char*) { return nullptr; }
std::string toStdString(JNIEnv*, jstring) { return {}; }
}
namespace vanta::social {
bool isFriend(JNIEnv*, jobject) { return false; }
}
namespace vanta::vgui::bridge {
bool isOverlayVisible() { return overlay; }
bool isEnabled(const std::string&) { return false; }
}
namespace vanta::modules::blockesp {
void set_focus_block(int, int, int, bool) {}
}
namespace vanta::modules::silentaim {
void ensure_installed() {}
void release_action_rotation(const void*) {}
void register_action_callbacks(const void*, const ActionCallbacks&) {}
void unregister_action_callbacks(const void*) {}
bool action_lease_held(const void*) { return false; }
bool rotation_spoof_active() { return false; }
bool silent_rotation_available() { return true; }
float mouse_lattice_deg() { return .15f; }
bool request_action_rotation(const void*, float, float, float, float, bool, const ActionCallbacks*) { return true; }
}

class Probe final : public vanta::modules::Explosive {
public:
    explicit Probe(vanta::modules::Variant variant) : Explosive(variant) {}
    void queue() { plan.kind = vanta::modules::Kind::Place; haveAim = haveLease = true; }
    bool queued() const { return plan.kind != vanta::modules::Kind::None || haveAim || haveLease; }
    void placed() { completedInteraction(vanta::modules::Kind::Place); }
    void charged() { completedInteraction(vanta::modules::Kind::Use); }
    void brokeCrystal() { completedInteraction(vanta::modules::Kind::Attack); }
    void confirmedAnchorGone() { cycleComplete = true; }
    void pausedPump() { tick(); }
    auto pausedExecute() {
        vanta::modules::silentaim::ActionContext context{};
        return execute(context, true);
    }
    bool restoreInDifferentWorld(JNIEnv* env, jobject oldWorld, jobject newWorld, jobject inventory) {
        a.e = env;
        slotRestore.record(1, 4);
        restoreWorldRef = oldWorld;
        restoreInventoryRef = inventory;
        const bool restored = restore(newWorld, inventory, nullptr, 100);
        return restored || slotRestore.active() || restoreWorldRef || restoreInventoryRef;
    }
};

vanta::Setting* setting(vanta::Module& module, std::string_view name) {
    for (auto* item : module.settings()) if (item->name() == name) return item;
    return nullptr;
}

int main() {
    using namespace vanta::modules::explosive_actions;
    try {
        // Charge and detonation must be distinct live inventory operations.
        require(matchesItem(Action::AnchorCharge, "block.minecraft.glowstone", false), "Glowstone charge rejected");
        require(!matchesItem(Action::AnchorCharge, "item.minecraft.diamond_sword", false), "Moved charge item used a sword");
        require(!matchesItem(Action::AnchorPlace, "block.minecraft.respawn_anchor", true), "Empty placement stack accepted");
        require(!matchesItem(Action::CrystalPlace, "block.minecraft.obsidian", false), "Crystal placement accepted the wrong material");
        require(matchesItem(Action::AnchorDetonate, "block.minecraft.air", true, 1), "Empty-hand detonation rejected");
        require(!matchesItem(Action::AnchorDetonate, "block.minecraft.glowstone", false, 1), "Detonation would add another charge");
        require(matchesItem(Action::AnchorDetonate, "block.minecraft.glowstone", false, 4), "Full anchor could not detonate with glowstone");
        require(!matchesItem(Action::AnchorDetonate, "block.minecraft.respawn_anchor", false, 4), "Detonation would place another anchor");
        require(!matchesItem(Action::Shield, "block.minecraft.glowstone", false, 0, false), "Changed shield preference ignored");

        TickGate gate;
        require(gate.commit(0) && !gate.commit(0), "Two modules acted on the same tick");
        require(gate.commit(1), "Next tick remained blocked");
        require(gate.commit(std::numeric_limits<unsigned>::max()) && gate.commit(0), "Tick counter wrap broke actions");
        ActionClock pace;
        require(pace.ready(0), "New sequence was delayed");
        pace.mark(0, 1);
        require(!pace.ready(50) && !pace.ready(100) && pace.ready(125), "Normal charged/detonated at the old rapid cadence");
        pace.mark(125, 1);
        require(!pace.ready(175) && pace.ready(250), "Pacing applied only to the first step");
        require(cadence(0).actionMs > cadence(1).actionMs && cadence(1).actionMs > cadence(2).actionMs,
                "Speed presets are not ordered");
        require(cadence(0).cycleMs > cadence(1).cycleMs && cadence(1).cycleMs > cadence(2).cycleMs,
                "Cycle presets are not ordered");

        SlotRestore restore;
        restore.record(0, 2); restore.record(2, 5);
        require(restore.target(5) == 0, "Charge/detonate lost the starting slot");
        restore.record(3, 7);
        require(restore.target(7) == 3, "Module restored a slot from before the user's manual choice");
        require(!restore.target(4) && restore.observe(4) && !restore.active(), "Restore overrode manual selection");
        restore.record(0, 9);
        require(!restore.active(), "Invalid hotbar slot was retained");

        std::vector<int> candidates(200);
        for (int i = 0; i < 200; ++i) candidates[i] = i;
        std::size_t cursor = 0;
        const auto evaluate = [](int candidate) { return candidate == 90 ? ScanDecision::Accept : ScanDecision::Skip; };
        require(!scan(candidates, cursor, 48, evaluate) && cursor == 48, "First search did not honor its budget");
        require(scan(candidates, cursor, 48, evaluate) == 90, "Valid base beyond the first 48 candidates was starved");
        cursor = 90;
        require(!scan(candidates, cursor, 48, [](int) { return ScanDecision::Yield; }) && cursor == 90,
                "Ray budget discarded an unexamined base");
        require(scan(candidates, cursor, 48, evaluate) == 90, "Ray-limited search failed to resume");

        for (auto variant : {vanta::modules::Variant::Crystal, vanta::modules::Variant::Anchor, vanta::modules::Variant::SafeAnchor}) {
            Probe module{variant};
            require(setting(module, "Velocidad") && setting(module, "Velocidad")->save() == "Normal", "Default speed is not Normal");
            require(!setting(module, "Min Delay") && !setting(module, "Max Delay"), "Old delay controls remain registered");
            int visible = 0;
            for (auto* item : module.settings()) if (item->visible()) ++visible;
            require(visible == 5, "Default panel still has too many controls");
            setting(module, "Avanzado")->load(true);
            int expanded = 0;
            for (auto* item : module.settings()) if (item->visible()) ++expanded;
            require(expanded > visible, "Advanced controls cannot be accessed");
            module.setEnabled(true);
            overlay = true;
            module.queue(); module.pausedPump();
            require(!module.queued(), "Opening the UI left a pending action in the pump");
            module.queue();
            require(module.pausedExecute() == vanta::modules::silentaim::ActionResult::Abort && !module.queued(),
                    "Paused callback retained a stale placement");
            module.keybind().setMode(vanta::BindMode::Once);
            module.placed(); module.pausedPump();
            require(module.isEnabled(), "Single-click auto cycle stopped after placement");
            if(variant == vanta::modules::Variant::Crystal) module.brokeCrystal();
            else {
                module.charged(); module.pausedPump();
                require(module.isEnabled(), "Single-click anchor stopped before detonation");
                module.confirmedAnchorGone();
            }
            module.pausedPump();
            require(!module.isEnabled(), "Single-click cycle stayed enabled after completion");
            overlay = false;
            module.setEnabled(false);
        }

        JNINativeInterface_ functions{};
        functions.IsSameObject = sameObject;
        JNIEnv env{&functions};
        _jobject oldWorld, newWorld, inventory;
        Probe context{vanta::modules::Variant::Anchor};
        require(!context.restoreInDifferentWorld(&env, &oldWorld, &newWorld, &inventory),
                "Slot restore crossed worlds or retained its stale context");
        std::cout << "Explosives: simple settings, per-step pacing, item validation, tick ownership, slots, search continuation and pause cancellation OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
