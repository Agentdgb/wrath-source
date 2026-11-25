#include "includes.hh"

#include "bridge.h"

#include "dt/views/hud_editor.hh"
#include "dt/views/intro.hh"
#include "modules/render/keybindhud/keybindhud.h"
#include "modules/combat/triggerbot/focus_visual_draw.h"

#include "core/config/ConfigManager.h"
#include "core/keybind/KeybindManager.h"
#include "core/module/Module.h"
#include "core/module/ModuleManager.h"
#include "core/setting/BlockSetSetting.h"
#include "core/setting/BoolSetting.h"
#include "core/setting/ColorSetting.h"
#include "core/setting/EnumSetting.h"
#include "core/setting/KeybindSetting.h"
#include "core/setting/KeyCaptureState.h"
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

namespace dt::vgui::bridge {

namespace {

::dt::Module* findReal(const std::string& name) {
    return ::dt::ModuleManager::get().find(name);
}

::dt::Setting* findRealSetting(::dt::Module* m, const std::string& settingName) {
    if (!m) return nullptr;
    for (auto* s : m->settings()) {
        if (s->name() == settingName) return s;
    }
    return nullptr;
}

// A display-only setting row. It reads the real preferences directly, so the
// preview works even with the module disabled and never needs a game entity.
class FocusPreviewRow final : public ::dt::c_setting {
public:
    explicit FocusPreviewRow(::dt::Module* module)
        : c_setting("Focus Preview"), module_(module) {}

    void update(float) override {}
    float height() const override { return 160.f; }

    void render(float x, float y, float w, const ::dt::frame_input_t&, float alpha) override {
        if (!g_render || !g_render->draw_list() || alpha <= 0.f || w < 60.f) return;
        ImDrawList* dl = g_render->draw_list();
        const auto opacity = [alpha](int a) { return static_cast<int>(a * std::clamp(alpha, 0.f, 1.f)); };
        dl->AddRectFilled(ImVec2(x, y + 3.f), ImVec2(x + w, y + 154.f),
                          IM_COL32(19, 19, 25, opacity(245)), 8.f);
        dl->AddRect(ImVec2(x, y + 3.f), ImVec2(x + w, y + 154.f),
                    IM_COL32(100, 90, 122, opacity(95)), 8.f);

        // Keep all preview geometry inside both this card and the drawer clip.
        dl->PushClipRect(ImVec2(x, y + 3.f), ImVec2(x + w, y + 154.f), true);

        // A small neutral silhouette keeps the preview inexpensive and does
        // not depend on a live target, player skin, JNI or combat events.
        const float cx = x + w * 0.5f;
        const ImU32 body = IM_COL32(85, 86, 104, opacity(225));
        const ImU32 face = IM_COL32(116, 116, 134, opacity(225));
        dl->AddRectFilled(ImVec2(cx - 12.f, y + 54.f), ImVec2(cx + 12.f, y + 78.f), face, 4.f);
        dl->AddRectFilled(ImVec2(cx - 17.f, y + 80.f), ImVec2(cx + 17.f, y + 116.f), body, 5.f);
        dl->AddRectFilled(ImVec2(cx - 24.f, y + 83.f), ImVec2(cx - 18.f, y + 115.f), body, 3.f);
        dl->AddRectFilled(ImVec2(cx + 18.f, y + 83.f), ImVec2(cx + 24.f, y + 115.f), body, 3.f);
        dl->AddRectFilled(ImVec2(cx - 15.f, y + 118.f), ImVec2(cx - 2.f, y + 144.f), body, 3.f);
        dl->AddRectFilled(ImVec2(cx + 2.f, y + 118.f), ImVec2(cx + 15.f, y + 144.f), body, 3.f);

        const auto read_enum = [&](const char* name, int fallback) {
            auto* setting = findRealSetting(module_, name);
            return setting && setting->type() == ::dt::SettingType::Enum
                ? static_cast<::dt::EnumSetting*>(setting)->index() : fallback;
        };
        const auto read_bool = [&](const char* name, bool fallback) {
            auto* setting = findRealSetting(module_, name);
            return setting && setting->type() == ::dt::SettingType::Bool
                ? static_cast<::dt::BoolSetting*>(setting)->value() : fallback;
        };
        const int first_marker_vertex = dl->VtxBuffer.Size;
        ::dt::modules::triggerbot::focus_visual::draw(
            dl, cx - 28.f, y + 48.f, cx + 28.f, y + 148.f, 6.f,
            read_enum("Focus Style", 0), read_enum("Arrow Style", 0),
            read_bool("Focus Arrow", true), read_bool("Focus Distance", true));
        // The shared world renderer uses full opacity; apply the drawer's
        // opening/closing animation to its preview vertices only.
        for (int i = first_marker_vertex; i < dl->VtxBuffer.Size; ++i) {
            ImU32& color = dl->VtxBuffer[i].col;
            const unsigned a = (color >> IM_COL32_A_SHIFT) & 0xffu;
            color = (color & ~(0xffu << IM_COL32_A_SHIFT)) |
                    (static_cast<ImU32>(opacity(static_cast<int>(a))) << IM_COL32_A_SHIFT);
        }
        dl->PopClipRect();
    }
private:
    ::dt::Module* module_;
};

// Secondary setting binds must not be confused with the module's own toggle pill.
class AuxiliaryKeybindRow final : public ::dt::c_setting {
public:
    explicit AuxiliaryKeybindRow(::dt::KeybindSetting* setting)
        : c_setting(std::string(setting->name())), setting_(setting) {}
    ~AuxiliaryKeybindRow() override { stop(); }
    float height() const override { return 34.f; }
    void update(float) override {
        if (!capture_.active()) return;
        if (!visible() || !isOverlayVisible() || !::dt::c_module::auxiliary_binding_active()) { stop(); return; }
        if (auto result = capture_.poll(snapshot(), guiToggleKey())) {
            if (!result->cancelled) {
                setting_->setVK(result->virtualKey);
                if (!::dt::ConfigManager::get().isLoading()) ::dt::ConfigManager::get().markDirty();
            }
            stop();
        }
    }
    void render(float x,float y,float width,const ::dt::frame_input_t& input,float alpha) override {
        if (!g_render || !g_render->draw_list() || alpha<=0) return;
        const std::string label = capture_.active() ? "Press a key" : keyLabel(setting_->vk());
        const float pillWidth = std::max(78.f, ImGui::CalcTextSize(label.c_str()).x+16.f);
        const float pillX=x+width-pillWidth;
        if (input.hovered(pillX,y+3,pillWidth,23) && input.m_left_clicked && alpha>0.35f) {
            if (capture_.active()) stop();
            else {
                ::dt::c_module::cancel_binding();
                if (active_ && active_!=this) active_->stop();
                capture_.start(snapshot()); active_=this;
                ::dt::c_module::set_auxiliary_binding(true);
            }
        }
        auto* draw=g_render->draw_list();
        const int opacity=static_cast<int>(255*std::clamp(alpha,0.f,1.f));
        draw->AddText(ImGui::GetFont(),12,ImVec2(x,y+8),IM_COL32(208,210,220,opacity),name().c_str());
        float red,green,blue,accentAlpha;clientAccent(red,green,blue,accentAlpha);
        const auto color=capture_.active()?IM_COL32(static_cast<int>(red*255),static_cast<int>(green*255),static_cast<int>(blue*255),opacity):IM_COL32(48,49,59,opacity);
        draw->AddRectFilled(ImVec2(pillX,y+3),ImVec2(x+width,y+26),color,5);
        draw->AddText(ImGui::GetFont(),11,ImVec2(pillX+8,y+8),IM_COL32(240,240,248,opacity),label.c_str());
    }
private:
    static ::dt::KeyCaptureState::Snapshot snapshot() {
        ::dt::KeyCaptureState::Snapshot keys{};
        for(int key=1;key<256;++key) keys[key]=(GetAsyncKeyState(key)&0x8000)!=0;
        return keys;
    }
    static std::string keyLabel(int key) {
        if (key==0) return "NONE";
        if ((key>='A'&&key<='Z') || (key>='0'&&key<='9')) return std::string(1,static_cast<char>(key));
        if (key>=VK_F1&&key<=VK_F24) return "F"+std::to_string(key-VK_F1+1);
        switch(key) {
        case VK_SPACE:return "SPACE";case VK_SHIFT:return "SHIFT";case VK_CONTROL:return "CTRL";
        case VK_MENU:return "ALT";case VK_TAB:return "TAB";case VK_XBUTTON1:return "M4";case VK_XBUTTON2:return "M5";
        case VK_RBUTTON:return "MOUSE 2";case VK_HOME:return "HOME";case VK_END:return "END";
        default:return "KEY "+std::to_string(key);
        }
    }
    void stop() {
        capture_.cancel();
        if (active_==this) { active_=nullptr;::dt::c_module::set_auxiliary_binding(false); }
    }
    ::dt::KeybindSetting* setting_;
    ::dt::KeyCaptureState capture_;
    static inline AuxiliaryKeybindRow* active_=nullptr;
};

// Map our Category enum onto the GUI's category strings (they already match
// after the rename, but going through a function keeps the coupling explicit).
std::string categoryName(::dt::Category c) {
    return std::string(::dt::toString(c));
}

} // namespace

std::vector<std::shared_ptr<::dt::c_module>> buildCardsFromRealModules() {
    std::vector<std::shared_ptr<::dt::c_module>> cards;

    for (const auto& real : ::dt::ModuleManager::get().all()) {
        auto card = std::make_shared<::dt::c_module>(
            std::string(real->name()),
            std::string(real->description()),
            categoryName(real->category()));

        card->set_enabled(real->isEnabled());

        /* And its bind. Without this every card opened showing NONE no matter
           what the saved profile had restored, so the user's own bind looked
           like it had been lost and rebinding it was the only way to see it. */
        card->set_keybind_vk(real->keybind().vk());

        // Mirror every real setting as its GUI counterpart. The keybind
        // module toggle bind is skipped; secondary binds get their own row.
        for (auto* s : real->settings()) {
            const std::string sname(s->name());

            // Row count before this setting is appended, so the visibility
            // predicate below can be attached to whatever the switch adds.
            const std::size_t before = card->settings().size();

            switch (s->type()) {
            case ::dt::SettingType::Bool: {
                auto* b = static_cast<::dt::BoolSetting*>(s);
                card->add_setting(std::make_shared<::dt::c_boolean_setting>(sname, b->value()));
                break;
            }
            case ::dt::SettingType::Int: {
                auto* i = static_cast<::dt::IntSetting*>(s);
                card->add_setting(std::make_shared<::dt::c_slider_setting>(
                    sname, static_cast<double>(i->min()), static_cast<double>(i->max()),
                    static_cast<double>(i->value()), 1.0, ""));
                break;
            }
            case ::dt::SettingType::Float: {
                auto* f = static_cast<::dt::FloatSetting*>(s);
                const double range = static_cast<double>(f->max()) - static_cast<double>(f->min());
                const double fallback = range > 20.0 ? 1.0 : (range > 4.0 ? 0.1 : 0.05);
                const double step = std::isfinite(f->step()) && f->step()>0 ? f->step() : fallback;
                card->add_setting(std::make_shared<::dt::c_slider_setting>(
                    sname, static_cast<double>(f->min()), static_cast<double>(f->max()),
                    static_cast<double>(f->value()), step, ""));
                break;
            }
            case ::dt::SettingType::Enum: {
                auto* e = static_cast<::dt::EnumSetting*>(s);
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
                card->add_setting(std::make_shared<::dt::c_mode_setting>(
                    sname, std::move(modes), e->index()));
                break;
            }
            case ::dt::SettingType::Keybind:
                if (s != &real->keybind()) card->add_setting(std::make_shared<AuxiliaryKeybindRow>(static_cast<::dt::KeybindSetting*>(s)));
                break;
            case ::dt::SettingType::Color: {
                auto* c = static_cast<::dt::ColorSetting*>(s);
                const auto v = c->value();
                card->add_setting(std::make_shared<::dt::c_color_setting>(
                    sname, v.r, v.g, v.b, v.a));
                break;
            }
            case ::dt::SettingType::String:
                // no GUI row type for this yet; stays config-file only
                break;
            case ::dt::SettingType::SlotMap: {
                auto* sm = static_cast<::dt::SlotMapSetting*>(s);
                auto row = std::make_shared<::dt::c_slot_map_setting>(sname, sm->options());
                // Both providers read the real setting every frame, so the
                // strip has no copy of its own to fall out of date -- ticking
                // an item kind off upstream empties its squares immediately.
                row->set_slot_provider([sm](int i){ return sm->slot(i); });
                row->set_allowed_provider([sm]{ return sm->allowedMask(); });
                card->add_setting(std::move(row));
                break;
            }
            case ::dt::SettingType::Range: {
                auto* r = static_cast<::dt::RangeSetting*>(s);
                const std::string suffix = sname.find("Delay") != std::string::npos
                    ? " ms" : "";
                card->add_setting(std::make_shared<::dt::c_range_slider_setting>(
                    sname,
                    static_cast<double>(r->absoluteMin()), static_cast<double>(r->absoluteMax()),
                    static_cast<double>(r->min()),        static_cast<double>(r->max()),
                    /*step=*/1.0, suffix));
                break;
            }
            case ::dt::SettingType::BlockSet: {
                auto* bs = static_cast<::dt::BlockSetSetting*>(s);
                card->add_setting(std::make_shared<::dt::c_block_picker_setting>(
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
                card->settings().back()->set_label(std::string(s->label()));
                // And its description, which the drawer turns into a hover
                // tooltip. Most settings carry none and get no tooltip.
                card->settings().back()->set_description(std::string(s->description()));
            }

            if (real->name() == "Hit Assist" && sname == "Preview Mode" &&
                s->type() == ::dt::SettingType::Bool) {
                auto* enabled = static_cast<::dt::BoolSetting*>(s);
                auto preview = std::make_shared<FocusPreviewRow>(real.get());
                // Keep the row nested under ShowFocusHighlight even when the
                // saved preview preference remains on while its parent hides.
                preview->set_visible_if([enabled] { return enabled->visible() && enabled->value(); });
                card->add_setting(std::move(preview));
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
    if (s->type() == ::dt::SettingType::Int) {
        auto* is = static_cast<::dt::IntSetting*>(s);
        int iv = static_cast<int>(value);
        if (is->value() != iv) {
            is->set(iv);
            changed = true;
        }
    } else if (s->type() == ::dt::SettingType::Float) {
        auto* fs = static_cast<::dt::FloatSetting*>(s);
        float fv = static_cast<float>(value);
        if (std::fabs(fs->value() - fv) > 0.0001f) {
            fs->set(fv);
            changed = true;
        }
    }
    if (changed && !::dt::ConfigManager::get().isLoading()) {
        ::dt::ConfigManager::get().markDirty();
    }
}

void applyBool(const std::string& moduleName, const std::string& settingName, bool value) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::dt::SettingType::Bool) {
        auto* bs = static_cast<::dt::BoolSetting*>(s);
        if (bs->value() != value) {
            bs->set(value);
            if (!::dt::ConfigManager::get().isLoading()) {
                ::dt::ConfigManager::get().markDirty();
            }
        }
    }
}

void applyRange(const std::string& moduleName, const std::string& settingName,
                int minValue, int maxValue) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::dt::SettingType::Range) {
        auto* rs = static_cast<::dt::RangeSetting*>(s);
        if (rs->min() != minValue || rs->max() != maxValue) {
            rs->set(minValue, maxValue);
            if (!::dt::ConfigManager::get().isLoading()) {
                ::dt::ConfigManager::get().markDirty();
            }
        }
    }
}

void applyColor(const std::string& moduleName, const std::string& settingName,
                float r, float g, float b, float a) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::dt::SettingType::Color) {
        auto* cs = static_cast<::dt::ColorSetting*>(s);
        auto old = cs->value();
        if (old.r != r || old.g != g || old.b != b || old.a != a) {
            cs->set(::dt::RGBA{ r, g, b, a });
            if (!::dt::ConfigManager::get().isLoading()) {
                ::dt::ConfigManager::get().markDirty();
            }
        }
    }
}

void applyMode(const std::string& moduleName, const std::string& settingName, const std::string& selected) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::dt::SettingType::Enum) {
        auto* e = static_cast<::dt::EnumSetting*>(s);
        if (e->current() != selected) {
            for (int idx = 0; idx < 64; ++idx) {
                const int saved = e->index();
                e->set(idx);
                if (e->index() != idx) { e->set(saved); break; }
                if (e->current() == selected) {
                    if (!::dt::ConfigManager::get().isLoading()) {
                        ::dt::ConfigManager::get().markDirty();
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
    if (s && s->type() == ::dt::SettingType::SlotMap) {
        static_cast<::dt::SlotMapSetting*>(s)->cycleSlot(slotIndex);
        if (!::dt::ConfigManager::get().isLoading()) {
            ::dt::ConfigManager::get().markDirty();
        }
    }
}

void applyKeybind(const std::string& moduleName, int virtualKey) {
    if (auto* m = findReal(moduleName)) {
        if (m->keybind().vk() != virtualKey) {
            m->keybind().setVK(virtualKey);
            if (!::dt::ConfigManager::get().isLoading()) {
                ::dt::ConfigManager::get().markDirty();
            }
        }
    }
}

void applyBlockSet(const std::string& moduleName, const std::string& settingName,
                   std::vector<std::string> ids) {
    auto* m = findReal(moduleName);
    auto* s = findRealSetting(m, settingName);
    if (s && s->type() == ::dt::SettingType::BlockSet) {
        static_cast<::dt::BlockSetSetting*>(s)->set(std::move(ids));
        if (!::dt::ConfigManager::get().isLoading()) {
            ::dt::ConfigManager::get().markDirty();
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
            if (!::dt::ConfigManager::get().isLoading()) {
                ::dt::ConfigManager::get().markDirty();
            }
        }
    }
}
bool isEnabled(const std::string& moduleName) {
    auto* m = findReal(moduleName);
    return m && m->isEnabled();
}

std::string moduleDescription(const std::string& moduleName) {
    std::lock_guard state(::dt::clientStateMutex());
    auto* m = findReal(moduleName);
    return m ? std::string(m->description()) : std::string{};
}
bool isHudEditorActive() {
    return ::dt::hud_editor::is_active();
}

bool isGlassEnabled() {
    return ::dt::theme::glass_enabled;
}

void clientAccent(float& r, float& g, float& b, float& a) {
    const hue::c_color accent = ::dt::theme::accent_violet;

    r = static_cast<float>(accent.r) / 255.f;
    g = static_cast<float>(accent.g) / 255.f;
    b = static_cast<float>(accent.b) / 255.f;
    a = static_cast<float>(accent.a) / 255.f;
}

unsigned long long iconTexture(const std::string& name, int pixel_size) {
    if (!::dt::g_icons || pixel_size < 1) return 0;
    return static_cast<unsigned long long>(::dt::g_icons->get(name, pixel_size));
}

bool isOverlayVisible() {
    // Plain bool read off the app singleton. The GL hook writes it on the
    // render thread and callers read it from the game thread; that race is
    // benign (worst case a module stands down or resumes one frame late) and
    // is the same access pattern core::cursor_guard already relies on.
    return ::dt::g_app != nullptr && ::dt::g_app->visible();
}

bool panelRect(float& x, float& y, float& w, float& h) {
    if (!::dt::g_app || !::dt::g_app->visible()) return false;

    const float s = ::dt::g_app->panel_scale();

    x = ::dt::g_app->panel_x();
    y = ::dt::g_app->panel_y();
    w = static_cast<float>(::dt::c_app::k_window_width)  * s;
    h = static_cast<float>(::dt::c_app::k_window_height) * s;
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
    return ::dt::KeybindManager::get().guiToggleKey();
}

void setGuiToggleKey(int vk) {
    if (vk <= 0 || vk >= 256) return;
    auto& km = ::dt::KeybindManager::get();
    km.setGuiToggleKey(vk);
    ::dt::vgui::setHotkeys(vk, km.panicKey());
    if (!::dt::ConfigManager::get().isLoading()) {
        ::dt::ConfigManager::get().markDirty();
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
    themeObj["glass"] = ::dt::theme::glass_enabled;
    themeObj["keybinds_hud"] = ::dt::modules::keybindhud::enabled;
    themeObj["preset"] = ::dt::theme::current_preset;

    const auto& acc = ::dt::theme::accent_violet;
    themeObj["accent"] = { {"r", acc.r}, {"g", acc.g}, {"b", acc.b}, {"a", acc.a} };

    const auto& sbg = ::dt::theme::bg_sidebar;
    themeObj["sidebar"] = { {"r", sbg.r}, {"g", sbg.g}, {"b", sbg.b}, {"a", sbg.a} };

    const auto& srch = ::dt::theme::bg_search;
    themeObj["search"] = { {"r", srch.r}, {"g", srch.g}, {"b", srch.b}, {"a", srch.a} };

    const auto& intro = ::dt::theme::intro_colour;
    themeObj["intro"] = { {"r", intro.r}, {"g", intro.g}, {"b", intro.b}, {"a", intro.a} };

    const auto& win = ::dt::theme::bg_window;
    themeObj["window"] = { {"r", win.r}, {"g", win.g}, {"b", win.b}, {"a", win.a} };

    const float scale = ::dt::g_app
        ? ::dt::g_app->m_user_scale
        : (s_pendingScale > 0.f ? s_pendingScale : 1.15f);
    j["#client"] = {
        {"scale", scale},
        {"toggle_vk", ::dt::KeybindManager::get().guiToggleKey()},
        {"intro_mode", ::dt::intro::mode()},
        {"intro_custom_name", ::dt::intro::custom_name()},
        {"intro_custom_wordmark", ::dt::intro::custom_wordmark()}
    };

    ::dt::hud_editor::save_positions(j["#hud"]);
}

void loadGuiState(const nlohmann::json& j) {
    // A profile without intro preferences must not inherit the previous one.
    ::dt::intro::set_mode(0);
    ::dt::intro::set_custom_name("");
    ::dt::intro::set_custom_wordmark("");
    if (j.contains("#theme") && j["#theme"].is_object()) {
        const auto& t = j["#theme"];
        if (t.contains("preset") && t["preset"].is_string()) {
            ::dt::theme::apply_preset_by_id(t["preset"].get<std::string>());
        }
        if (t.contains("glass") && t["glass"].is_boolean()) {
            ::dt::theme::glass_enabled = t["glass"].get<bool>();
        }
        if (t.contains("keybinds_hud") && t["keybinds_hud"].is_boolean()) {
            ::dt::modules::keybindhud::enabled = t["keybinds_hud"].get<bool>();
        }
        if (t.contains("accent") && t["accent"].is_object()) {
            const auto& a = t["accent"];
            ::dt::theme::set_accent(hue::c_color(
                a.value("r", 155), a.value("g", 75), a.value("b", 220), a.value("a", 255)
            ));
        }
        if (t.contains("sidebar") && t["sidebar"].is_object()) {
            const auto& s = t["sidebar"];
            ::dt::theme::bg_sidebar = hue::c_color(
                s.value("r", 12), s.value("g", 12), s.value("b", 14), s.value("a", 255)
            );
        }
        if (t.contains("search") && t["search"].is_object()) {
            const auto& s = t["search"];
            ::dt::theme::bg_search = hue::c_color(
                s.value("r", 18), s.value("g", 18), s.value("b", 22), s.value("a", 255)
            );
        }
        if (t.contains("intro") && t["intro"].is_object()) {
            const auto& i = t["intro"];
            ::dt::theme::intro_colour = hue::c_color(
                std::clamp(i.value("r", 154), 0, 255), std::clamp(i.value("g", 71), 0, 255),
                std::clamp(i.value("b", 245), 0, 255), std::clamp(i.value("a", 255), 40, 255)
            );
        }
        if (t.contains("window") && t["window"].is_object()) {
            const auto& w = t["window"];
            ::dt::theme::bg_window = hue::c_color(
                w.value("r", 15), w.value("g", 15), w.value("b", 18), w.value("a", 255)
            );
        }
    }

    if (j.contains("#client") && j["#client"].is_object()) {
        const auto& c = j["#client"];
        if (c.contains("intro_mode") && c["intro_mode"].is_number_integer() &&
            c["intro_mode"] >= 0 && c["intro_mode"] <= 2) {
            ::dt::intro::set_mode(c["intro_mode"].get<int>());
        }
        if (c.contains("intro_custom_name") && c["intro_custom_name"].is_string()) {
            ::dt::intro::set_custom_name(c["intro_custom_name"].get<std::string>());
        }
        if (c.contains("intro_custom_wordmark") && c["intro_custom_wordmark"].is_string()) {
            ::dt::intro::set_custom_wordmark(c["intro_custom_wordmark"].get<std::string>());
        }
        if (c.contains("scale") && c["scale"].is_number()) {
            float s = c["scale"].get<float>();
            if (s >= 0.5f && s <= 2.5f) {
                s_pendingScale = s;
                if (::dt::g_app) {
                    ::dt::g_app->m_user_scale = s;
                }
            }
        }
        if (c.contains("toggle_vk") && c["toggle_vk"].is_number_integer()) {
            int vk = c["toggle_vk"].get<int>();
            if (vk > 0 && vk < 256) {
                auto& km = ::dt::KeybindManager::get();
                km.setGuiToggleKey(vk);
                ::dt::vgui::setHotkeys(vk, km.panicKey());
            }
        }
    }

    if (j.contains("#hud") && j["#hud"].is_object()) {
        ::dt::hud_editor::load_positions(j["#hud"]);
    }
}

void switchProfile(const std::string& profile) {
    auto& cm = ::dt::ConfigManager::get();
    cm.loadProfile(profile);
    if (::dt::g_app) {
        ::dt::g_app->reload_module_cards();
    }
}

const std::string& currentProfile() {
    return ::dt::ConfigManager::get().currentProfile();
}

} // namespace dt::vgui::bridge
