#include "VisualPorts.h"
#include "core/module/Module.h"
#include "modules/visuals/bloom/DeathEffects.h"
namespace vanta::modules::visualports {
SwingConfig swing;
SpiritConfig spirit;
CharacterConfig character;
bool blurEnabled = false;
float blurStrength = .5f;
class MotionBlur final : public Module {
    FloatSetting strength_{this, "Strength", 50, 10, 90, 5};

  public:
    MotionBlur() : Module("MotionBlur", "Temporal world motion blur", Category::Visuals) {
        registerSetting(&strength_);
        listen<events::GameLoopEvent>([this](auto &) {
            if (isStopping())
                return;
            blurEnabled = isEnabled();
            blurStrength = strength_.value() / 100;
            tick();
            if (isEnabled())
                setDescription(worldStatus());
        });
    }
    void onDisable() override { blurEnabled = false; }
};
class SwingAnimation final : public Module {
    EnumSetting style_{this, "Style", {"Slash", "Chop", "Hook", "Spin", "Double Tap", "Push"}, 0};
    FloatSetting tempo_{this, "Tempo", 1, .4f, 2.5f, .05f}, intensity_{this, "Intensity", 1, .2f, 2, .05f};
    BoolSetting offhand_{this, "Offhand Too", true}, combat_{this, "Combat Only", false};

  public:
    SwingAnimation() : Module("SwingAnimation", "Custom first-person swing animations", Category::Visuals) {
        for (auto *s : std::initializer_list<Setting *>{&style_, &tempo_, &intensity_, &offhand_, &combat_})
            registerSetting(s);
        listen<events::GameLoopEvent>([this](auto &) {
            if (isStopping())
                return;
            swing.enabled = isEnabled();
            swing.style = style_.index();
            swing.tempo = tempo_.value();
            swing.intensity = intensity_.value();
            swing.offhand = offhand_.value();
            swing.combatOnly = combat_.value();
            if (isEnabled())
                setDescription(swingStatus());
        });
    }
    void onDisable() override { swing.enabled = false; }
};
class SlowSwing final : public Module {
    FloatSetting speed_{this, "Speed", .25f, .1f, 2, .05f};

  public:
    SlowSwing() : Module("SlowSwing", "Control hand swing speed", Category::Misc) {
        registerSetting(&speed_);
        listen<events::GameLoopEvent>([this](auto &) {
            if (isStopping())
                return;
            swing.slow = isEnabled();
            swing.speed = speed_.value();
        });
    }
    void onDisable() override { swing.slow = false; }
};
class TotemSpirit final : public Module {
    FloatSetting life_{this, "Ka Time", 2.4f, .6f, 6, .1f}, ascent_{this, "Ascent", 1.6f, 0, 4, .1f},
        opacity_{this, "Opacity", .65f, .15f, 1, .05f};
    EnumSetting look_{this, "Look", {"Glass", "Wire"}, 0}, color_{this, "Color", {"Theme", "Manual"}, 0};
    BoolSetting pose_{this, "Ka Pose", true}, crest_{this, "Ankh Crest", true}, seal_{this, "Ground Seal", true},
        own_{this, "Own Pops", false}, walls_{this, "Through Walls", false};
    ColorSetting body_{this, "Body Color", RGBA{74.f / 255, 138.f / 255, 245.f / 255, 1}},
        aura_{this, "Aura Color", RGBA{29.f / 255, 64.f / 255, 163.f / 255, 1}};

  public:
    TotemSpirit() : Module("TotemSpirit", "A glowing spirit rises on a totem pop", Category::Visuals) {
        for (auto *s : std::initializer_list<Setting *>{&life_, &ascent_, &look_, &opacity_, &pose_, &crest_, &seal_,
                                                        &own_, &walls_, &color_, &body_, &aura_})
            registerSetting(s);
        body_.setVisibleIf([this] { return color_.index() == 1; });
        aura_.setVisibleIf([this] { return color_.index() == 1; });
        listen<events::PacketReceiveEvent>([this](auto &e) {
            if (isEnabled() || bloom::deathInterested())
                receive(e.env, e.packet);
        });
        listen<events::GameLoopEvent>([this](auto &) {
            if (isStopping())
                return;
            spirit = {isEnabled(),      pose_.value(),       crest_.value(), seal_.value(), own_.value(),
                      walls_.value(),   color_.index() == 1, look_.index(),  life_.value(), ascent_.value(),
                      opacity_.value(), body_.value(),       aura_.value()};
            if (isEnabled())
                setDescription(worldStatus());
        });
    }
    void onDisable() override { spirit.enabled = false; }
};
class Character3D final : public Module {
    EnumSetting model_{
        this, "Character", {"Sakura", "Luna", "Mika", "Aoi", "Miku", "Tung Tung Tung Sahur", "Tralalero Tralala"}, 0},
        detail_{this, "Detail", {"Low", "Medium", "High"}, 1};
    BoolSetting friends_{this, "On Friends", false}, held_{this, "Held Items", false};

  public:
    Character3D() : Module("3D Character", "Replace your player with a 3D character", Category::Visuals) {
        for (auto *s : std::initializer_list<Setting *>{&model_, &friends_, &detail_, &held_})
            registerSetting(s);
        listen<events::GameLoopEvent>([this](auto &) {
            if (isStopping())
                return;
            character = {isEnabled(), friends_.value(), held_.value(), model_.index(), detail_.index()};
            if (isEnabled())
                setDescription(characterStatus());
        });
    }
    void onDisable() override { character.enabled = false; }
};
} // namespace vanta::modules::visualports
VANTA_REGISTER_MODULE(vanta::modules::visualports::MotionBlur)
VANTA_REGISTER_MODULE(vanta::modules::visualports::SwingAnimation)
VANTA_REGISTER_MODULE(vanta::modules::visualports::SlowSwing)
VANTA_REGISTER_MODULE(vanta::modules::visualports::TotemSpirit)
VANTA_REGISTER_MODULE(vanta::modules::visualports::Character3D)
