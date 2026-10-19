#include "includes.hh"

#include "bridge.h"

#include "vanta/views/hud_editor.hh"
#include "modules/render/keybindhud/keybindhud.h"

#include "core/config/ConfigManager.h"
#include "core/keybind/KeybindManager.h"
#include "core/module/Module.h"
#include "core/module/ModuleManager.h"
#include "core/setting/BlockSetSetting.h"
#include "core/setting/BoolSetting.h"
#include "core/setting/ColorSetting.h"
#include "core/setting/EnumSetting.h"
#include "core/setting/KeybindSetting.h"
#include "core/setting/NumericSetting.h"
#include "core/setting/RangeSetting.h"
#include "core/setting/Setting.h"
#include "core/setting/SlotMapSetting.h"
#include "core/setting/StringSetting.h"
#include "vgui/vgui.h"

/* Freecam's capture lives next to the input filter in gl_hook.cc but is not
   in gl_hook.hh; these prototypes mirror the definitions there exactly. */
namespace core::gl_hook {
void set_freecam_capture(bool on);
int  take_freecam_wheel();
}

namespace vanta::vgui::bridge {

namespace {

::vanta::Module* findReal(const std::string& name) {
    return ::vanta::ModuleManager::get().find(name);
}

::vanta::Setting* findRealSetting(::vanta::Module* m, const std::string& settingName) {
    if (!m) return nullptr;
    for (auto* s : m->settings()) {
        if (s->name() == settingName) return s;
    }
    return nullptr;
}

// Map our Category enum onto the GUI's category strings (they already match
// after the rename, but going through a function keeps the coupling explicit).
std::string categoryName(::vanta::Category c) {
    return std::string(::vanta::toString(c));
}

} // namespace

std::vector<std::shared_ptr<::vanta::c_module>> buildCardsFromRealModules() {
    std::vector<std::shared_ptr<::vanta::c_module>> cards;

    for (const auto& real : ::vanta::ModuleManager::get().all()) {
        auto card = std::make_shared<::vanta::c_module>(
            std::string(real->name()),
            std::string(real->description()),
            categoryName(real->category()));

        card->set_enabled(real->isEnabled());

        /* And its bind. Without this every card opened showing NONE no matter
           what the saved profile had restored, so the user's own bind looked
           like it had been lost and rebinding it was the only way to see it. */
        card->set_keybind_vk(real->keybind().vk());

        // Mirror every real setting as its GUI counterpart. The keybind
        // setting is skipped: the card renders its own bind pill.
        for (auto* s : real->settings()) {
            const std::string sname(s->name());

            // Row count before this setting is appended, so the visibility
            // predicate below can be attached to whatever the switch adds.
            const std::size_t before = card->settings().size();

            switch (s->type()) {
            case ::vanta::SettingType::Bool: {
                auto* b = static_cast<::vanta::BoolSetting*>(s);
                card->add_setting(std::make_shared<::vanta::c_boolean_setting>(sname, b->value()));
                break;
            }
            case ::vanta::SettingType::Int: {
                auto* i = static_cast<::vanta::IntSetting*>(s);
                card->add_setting(std::make_shared<::vanta::c_slider_setting>(
                    sname, static_cast<double>(i->min()), static_cast<double>(i->max()),
                    static_cast<double>(i->value()), 1.0, ""));
                break;
            }
            case ::vanta::SettingType::Float: {
                auto* f = static_cast<::vanta::FloatSetting*>(s);
                const double range = static_cast<double>(f->max()) - static_cast<double>(f->min());
                const double step  = range > 20.0 ? 1.0 : (range > 4.0 ? 0.1 : 0.05);
                card->add_setting(std::make_shared<::vanta::c_slider_setting>(
                    sname, static_cast<double>(f->min()), static_cast<double>(f->max()),
                    static_cast<double>(f->value()), step, ""));
                break;
            }
            case ::vanta::SettingType::Enum: {
                auto* e = static_cast<::vanta::EnumSetting*>(s);
                std::vector<std::string> modes;
                // EnumSetting stores options internally; expose the current one
                // plus the full list via its public surface.
                for (int idx = 0; ; ++idx) {
                    const int saved = e->index();
                    e->set(idx);
                    if (e->index() != idx) { e->set(saved); break; }
                    modes.emplace_back(e->current());
                    e->set(saved);
                    if (idx > 32) break;   // guard
                }
                if (modes.empty()) modes.emplace_back(std::string(e->current()));
                card->add_setting(std::make_shared<::vanta::c_mode_setting>(
                    sname, std::move(modes), e->index()));
                break;
            }
            case ::vanta::SettingType::Keybind:
                // rendered by the card's own bind pill
                break;
            case ::vanta::SettingType::Color: {
                auto* c = static_cast<::vanta::ColorSetting*>(s);
                const auto v = c->value();
                card->add_setting(std::make_shared<::vanta::c_color_setting>(
                    sname, v.r, v.g, v.b, v.a));
                break;
            }
            case ::vanta::SettingType::String:
                // no GUI row type for this yet; stays config-file only
                break;
            case ::vanta::SettingType::SlotMap: {
                auto* sm = static_cast<::vanta::SlotMapSetting*>(s);
                auto row = std::make_shared<::vanta::c_slot_map_setting>(sname, sm->options());
                // Both providers read the real setting every frame, so the
                // strip has no copy of its own to fall out of date -- ticking
                // an item kind off upstream empties its squares immediately.
                row->set_slot_provider([sm](int i){ return sm->slot(i); });
                row->set_allowed_provider([sm]{ return sm->allowedMask(); });
                card->add_setting(std::move(row));
                break;
            }
            case ::vanta::SettingType::Range: {
                auto* r = static_cast<::vanta::RangeSetting*>(s);
                const std::string suffix = sname.find("Delay") != std::string::npos
                    ? " ms" : "";
                card->add_setting(std::make_shared<::vanta::c_range_slider_setting>(
                    sname,
                    static_cast<double>(r->absoluteMin()), static_cast<double>(r->absoluteMax()),
                    static_cast<double>(r->min()),        static_cast<double>(r->max()),
                    /*step=*/1.0, suffix));
                break;
            }
            case ::vanta::SettingType::BlockSet: {
                auto* bs = static_cast<::vanta::BlockSetSetting*>(s);
                card->add_setting(std::make_shared<::vanta::c_block_picker_setting>(
                    sname, bs->value()));
                break;
            }
            }

            // Carry the real setting's visibility predicate onto the GUI row.
            // The drawer re-evaluates it every frame, so a row that only
            // applies to one mode disappears the moment another is picked.
            // Capturing the raw Setting* is safe: modules and their settings
            // live for the whole process.
            if (card->settings().size() > before) {
                card->settings().back()->set_visible_if([s]{ return s->visible(); });
                // And its description, which the drawer turns into a hover
                // tooltip. Most settings carry none and get no tooltip.
                card->settings().back()->set_description(std::string(s->description()));
            }
        }

        cards.push_back(std::move(card));
    }

    slog::log::info("bridge: built {} GUI cards from real modules", cards.size());
    return cards;
}

void applyToggle(const std::string& moduleName, bool enabled) {
    if (auto* m = findReal(moduleName)) {
        if (m->isEnabled() != enabled) {
            m->setEnabled(enabled);
        }
    }
}

void applyNumeric(const std::string& moduleName, const std::string& settingName, double value) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (!s) return;
    bool changed = false;
    if (s->type() == ::vanta::SettingType::Int) {
        auto* is = static_cast<::vanta::IntSetting*>(s);
        int iv = static_cast<int>(value);
        if (is->value() != iv) {
            is->set(iv);
            changed = true;
        }
    } else if (s->type() == ::vanta::SettingType::Float) {
        auto* fs = static_cast<::vanta::FloatSetting*>(s);
        float fv = static_cast<float>(value);
        if (std::fabs(fs->value() - fv) > 0.0001f) {
            fs->set(fv);
            changed = true;
        }
    }
    if (changed && !::vanta::ConfigManager::get().isLoading()) {
        ::vanta::ConfigManager::get().markDirty();
    }
}

void applyBool(const std::string& moduleName, const std::string& settingName, bool value) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::vanta::SettingType::Bool) {
        auto* bs = static_cast<::vanta::BoolSetting*>(s);
        if (bs->value() != value) {
            bs->set(value);
            if (!::vanta::ConfigManager::get().isLoading()) {
                ::vanta::ConfigManager::get().markDirty();
            }
        }
    }
}

void applyRange(const std::string& moduleName, const std::string& settingName,
                int minValue, int maxValue) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::vanta::SettingType::Range) {
        auto* rs = static_cast<::vanta::RangeSetting*>(s);
        if (rs->min() != minValue || rs->max() != maxValue) {
            rs->set(minValue, maxValue);
            if (!::vanta::ConfigManager::get().isLoading()) {
                ::vanta::ConfigManager::get().markDirty();
            }
        }
    }
}

void applyColor(const std::string& moduleName, const std::string& settingName,
                float r, float g, float b, float a) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::vanta::SettingType::Color) {
        auto* cs = static_cast<::vanta::ColorSetting*>(s);
        auto old = cs->value();
        if (old.r != r || old.g != g || old.b != b || old.a != a) {
            cs->set(::vanta::RGBA{ r, g, b, a });
            if (!::vanta::ConfigManager::get().isLoading()) {
                ::vanta::ConfigManager::get().markDirty();
            }
        }
    }
}

void applyMode(const std::string& moduleName, const std::string& settingName, const std::string& selected) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::vanta::SettingType::Enum) {
        auto* e = static_cast<::vanta::EnumSetting*>(s);
        if (e->current() != selected) {
            for (int idx = 0; idx < 64; ++idx) {
                const int saved = e->index();
                e->set(idx);
                if (e->index() != idx) { e->set(saved); break; }
                if (e->current() == selected) {
                    if (!::vanta::ConfigManager::get().isLoading()) {
                        ::vanta::ConfigManager::get().markDirty();
                    }
                    return;
                }
                e->set(saved);
            }
        }
    }
}

void cycleSlot(const std::string& moduleName, const std::string& settingName, int slotIndex) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::vanta::SettingType::SlotMap) {
        static_cast<::vanta::SlotMapSetting*>(s)->cycleSlot(slotIndex);
        if (!::vanta::ConfigManager::get().isLoading()) {
            ::vanta::ConfigManager::get().markDirty();
        }
    }
}

void applyKeybind(const std::string& moduleName, int virtualKey) {
    if (auto* m = findReal(moduleName)) {
        if (m->keybind().vk() != virtualKey) {
            m->keybind().setVK(virtualKey);
            if (!::vanta::ConfigManager::get().isLoading()) {
                ::vanta::ConfigManager::get().markDirty();
            }
        }
    }
}

void applyBlockSet(const std::string& moduleName, const std::string& settingName,
                   std::vector<std::string> ids) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::vanta::SettingType::BlockSet) {
        static_cast<::vanta::BlockSetSetting*>(s)->set(std::move(ids));
        if (!::vanta::ConfigManager::get().isLoading()) {
            ::vanta::ConfigManager::get().markDirty();
        }
    }
}

namespace {
std::function<const std::vector<BlockPickerRow>&()> g_blockPickerProvider;
const std::vector<BlockPickerRow> g_blockPickerEmpty{};
} // namespace

void setBlockPickerProvider(std::function<const std::vector<BlockPickerRow>&()> p) {
    g_blockPickerProvider = std::move(p);
}

const std::vector<BlockPickerRow>* blockPickerRows() {
    if (!g_blockPickerProvider) return &g_blockPickerEmpty;
    return &g_blockPickerProvider();
}

int bindKey(const std::string& name) {
    auto* m=findReal(name); return m?m->keybind().vk():0;
}
int bindMode(const std::string& name) {
    auto* m=findReal(name); return m?static_cast<int>(m->keybind().mode()):0;
}
void applyBindMode(const std::string& name,int mode) {
    if(mode<0 || mode>3) return;
    if(auto* m=findReal(name)) {
        const auto old=m->keybind().mode();
        if (static_cast<int>(old) != mode) {
            m->keybind().setMode(static_cast<BindMode>(mode));
            if(old==BindMode::Always || mode==1) m->setEnabled(false);
            if (!::vanta::ConfigManager::get().isLoading()) {
                ::vanta::ConfigManager::get().markDirty();
            }
        }
    }
}
bool isEnabled(const std::string& moduleName) {
    auto* m = findReal(moduleName);
    return m && m->isEnabled();
}

std::string moduleDescription(const std::string& moduleName) {
    std::lock_guard state(::vanta::clientStateMutex());
    auto* m = findReal(moduleName);
    return m ? std::string(m->description()) : std::string{};
}
int selectedSwordCosmetic() {
    std::lock_guard state(::vanta::clientStateMutex());
    auto* module = findReal("SwordReplace");
    auto* setting = findRealSetting(module, "Model");
    return setting && setting->type() == ::vanta::SettingType::Enum
        ? static_cast<::vanta::EnumSetting*>(setting)->index() : 0;
}

bool isHudEditorActive() {
    return ::vanta::hud_editor::is_active();
}

bool isGlassEnabled() {
    return ::vanta::theme::glass_enabled;
}

void clientAccent(float& r, float& g, float& b, float& a) {
    const hue::c_color accent = ::vanta::theme::accent_violet;

    r = static_cast<float>(accent.r) / 255.f;
    g = static_cast<float>(accent.g) / 255.f;
    b = static_cast<float>(accent.b) / 255.f;
    a = static_cast<float>(accent.a) / 255.f;
}

unsigned long long iconTexture(const std::string& name, int pixel_size) {
    if (!::vanta::g_icons || pixel_size < 1) return 0;
    return static_cast<unsigned long long>(::vanta::g_icons->get(name, pixel_size));
}

bool isOverlayVisible() {
    // Plain bool read off the app singleton. The GL hook writes it on the
    // render thread and callers read it from the game thread; that race is
    // benign (worst case a module stands down or resumes one frame late) and
    // is the same access pattern core::cursor_guard already relies on.
    return ::vanta::g_app != nullptr && ::vanta::g_app->visible();
}

bool panelRect(float& x, float& y, float& w, float& h) {
    if (!::vanta::g_app || !::vanta::g_app->visible()) return false;

    const float s = ::vanta::g_app->panel_scale();

    x = ::vanta::g_app->panel_x();
    y = ::vanta::g_app->panel_y();
    w = static_cast<float>(::vanta::c_app::k_window_width)  * s;
    h = static_cast<float>(::vanta::c_app::k_window_height) * s;
    return true;
}

namespace {
// One slot per owner, and the window filter runs at the highest any owner is
// asking for. Without this the two modules that drive a container would clobber
// each other: whichever released first would drop the other's lock mid-cycle.
// Owner 0 is the legacy caller (AutoTotem), so its behaviour is unchanged.
constexpr int kFilterOwners = 4;
int g_filterLevels[kFilterOwners] = { 0, 0, 0, 0 };
} // namespace

void setInputFilterFor(int owner, int level) {
    if (owner < 0 || owner >= kFilterOwners) return;
    g_filterLevels[owner] = level;

    int effective = 0;
    for (int lvl : g_filterLevels) if (lvl > effective) effective = lvl;
    ::core::gl_hook::set_input_filter(effective);
}

void setInputFilter(int level) {
    setInputFilterFor(FILTER_OWNER_AUTOTOTEM, level);
}

void setFreecamCapture(bool on) {
    ::core::gl_hook::set_freecam_capture(on);
}

int takeFreecamWheelNotches() {
    // Raw WHEEL_DELTA units; a high-resolution wheel reports fractions of 120
    // per message, so the remainder is kept rather than rounded away.
    static int s_remainder = 0;
    s_remainder += ::core::gl_hook::take_freecam_wheel();
    const int notches = s_remainder / WHEEL_DELTA;
    s_remainder -= notches * WHEEL_DELTA;
    return notches;
}

namespace {
float s_pendingScale = -1.f;
std::atomic<bool> s_menuKeyBinding{false};
} // namespace

float consumePendingScale() {
    float s = s_pendingScale;
    s_pendingScale = -1.f;
    return s;
}

int guiToggleKey() {
    return ::vanta::KeybindManager::get().guiToggleKey();
}

void setGuiToggleKey(int vk) {
    if (vk <= 0 || vk >= 256) return;
    auto& km = ::vanta::KeybindManager::get();
    km.setGuiToggleKey(vk);
    ::vanta::vgui::setHotkeys(vk, km.panicKey());
    if (!::vanta::ConfigManager::get().isLoading()) {
        ::vanta::ConfigManager::get().markDirty();
    }
}

std::string vkToString(int vk) {
    if (vk <= 0 || vk >= 256) return "NONE";
    switch (vk) {
        case VK_INSERT:   return "INSERT";
        case VK_DELETE:   return "DELETE";
        case VK_HOME:     return "HOME";
        case VK_END:      return "END";
        case VK_PRIOR:    return "PAGE UP";
        case VK_NEXT:     return "PAGE DOWN";
        case VK_RSHIFT:   return "RSHIFT";
        case VK_LSHIFT:   return "LSHIFT";
        case VK_RCONTROL: return "RCTRL";
        case VK_LCONTROL: return "LCTRL";
        case VK_RMENU:    return "RALT";
        case VK_LMENU:    return "LALT";
        case VK_UP:       return "UP";
        case VK_DOWN:     return "DOWN";
        case VK_LEFT:     return "LEFT";
        case VK_RIGHT:    return "RIGHT";
        default: break;
    }
    char buffer[64]{};
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (scan != 0 && GetKeyNameTextA(static_cast<LONG>(scan << 16), buffer, sizeof(buffer)) > 0) {
        std::string name{ buffer };
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return name;
    }
    return "KEY " + std::to_string(vk);
}

void setMenuKeyBinding(bool active) {
    s_menuKeyBinding.store(active, std::memory_order_relaxed);
}

bool isMenuKeyBinding() {
    return s_menuKeyBinding.load(std::memory_order_relaxed);
}

void saveGuiState(nlohmann::json& j) {
    auto& themeObj = j["#theme"] = nlohmann::json::object();
    themeObj["glass"] = ::vanta::theme::glass_enabled;
    themeObj["keybinds_hud"] = ::vanta::modules::keybindhud::enabled;

    const auto& acc = ::vanta::theme::accent_violet;
    themeObj["accent"] = { {"r", acc.r}, {"g", acc.g}, {"b", acc.b}, {"a", acc.a} };

    const auto& sbg = ::vanta::theme::bg_sidebar;
    themeObj["sidebar"] = { {"r", sbg.r}, {"g", sbg.g}, {"b", sbg.b}, {"a", sbg.a} };

    const auto& srch = ::vanta::theme::bg_search;
    themeObj["search"] = { {"r", srch.r}, {"g", srch.g}, {"b", srch.b}, {"a", srch.a} };

    const auto& win = ::vanta::theme::bg_window;
    themeObj["window"] = { {"r", win.r}, {"g", win.g}, {"b", win.b}, {"a", win.a} };

    const float scale = ::vanta::g_app
        ? ::vanta::g_app->m_user_scale
        : (s_pendingScale > 0.f ? s_pendingScale : 1.15f);
    j["#client"] = {
        {"scale", scale},
        {"toggle_vk", ::vanta::KeybindManager::get().guiToggleKey()}
    };

    ::vanta::hud_editor::save_positions(j["#hud"]);
}

void loadGuiState(const nlohmann::json& j) {
    if (j.contains("#theme") && j["#theme"].is_object()) {
        const auto& t = j["#theme"];
        if (t.contains("glass") && t["glass"].is_boolean()) {
            ::vanta::theme::glass_enabled = t["glass"].get<bool>();
        }
        if (t.contains("keybinds_hud") && t["keybinds_hud"].is_boolean()) {
            ::vanta::modules::keybindhud::enabled = t["keybinds_hud"].get<bool>();
        }
        if (t.contains("accent") && t["accent"].is_object()) {
            const auto& a = t["accent"];
            ::vanta::theme::set_accent(hue::c_color(
                a.value("r", 155), a.value("g", 75), a.value("b", 220), a.value("a", 255)
            ));
        }
        if (t.contains("sidebar") && t["sidebar"].is_object()) {
            const auto& s = t["sidebar"];
            ::vanta::theme::bg_sidebar = hue::c_color(
                s.value("r", 12), s.value("g", 12), s.value("b", 14), s.value("a", 255)
            );
        }
        if (t.contains("search") && t["search"].is_object()) {
            const auto& s = t["search"];
            ::vanta::theme::bg_search = hue::c_color(
                s.value("r", 18), s.value("g", 18), s.value("b", 22), s.value("a", 255)
            );
        }
        if (t.contains("window") && t["window"].is_object()) {
            const auto& w = t["window"];
            ::vanta::theme::bg_window = hue::c_color(
                w.value("r", 15), w.value("g", 15), w.value("b", 18), w.value("a", 255)
            );
        }
    }

    if (j.contains("#client") && j["#client"].is_object()) {
        const auto& c = j["#client"];
        if (c.contains("scale") && c["scale"].is_number()) {
            float s = c["scale"].get<float>();
            if (s >= 0.5f && s <= 2.5f) {
                s_pendingScale = s;
                if (::vanta::g_app) {
                    ::vanta::g_app->m_user_scale = s;
                }
            }
        }
        if (c.contains("toggle_vk") && c["toggle_vk"].is_number_integer()) {
            int vk = c["toggle_vk"].get<int>();
            if (vk > 0 && vk < 256) {
                auto& km = ::vanta::KeybindManager::get();
                km.setGuiToggleKey(vk);
                ::vanta::vgui::setHotkeys(vk, km.panicKey());
            }
        }
    }

    if (j.contains("#hud") && j["#hud"].is_object()) {
        ::vanta::hud_editor::load_positions(j["#hud"]);
    }
}

void switchProfile(const std::string& profile) {
    auto& cm = ::vanta::ConfigManager::get();
    cm.loadProfile(profile);
    if (::vanta::g_app) {
        ::vanta::g_app->reload_module_cards();
    }
}

const std::string& currentProfile() {
    return ::vanta::ConfigManager::get().currentProfile();
}

} // namespace vanta::vgui::bridge
