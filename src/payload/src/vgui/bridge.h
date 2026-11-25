#pragma once

// Bridge between the ported GUI (src/vgui, "dt::c_module") and the real
// client module system (src/core, "dt::Module").
//
// The GUI owns presentation-only cards. This header lets the card layer push
// user intent (toggle, slider change, keybind) into the real Module and pull
// state back out, without the GUI ever including core/ headers directly.

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace dt { class Module; class c_module; class c_setting; }

namespace dt::vgui::bridge {

// Build one c_module card per real Module, mirroring its settings.
// Called by c_module_manager::build_default_modules().
std::vector<std::shared_ptr<::dt::c_module>> buildCardsFromRealModules();

// Push a card's toggle state into the real Module.
void applyToggle(const std::string& moduleName, bool enabled);

// Push a card's numeric setting into the real Module.
void applyNumeric(const std::string& moduleName, const std::string& settingName, double value);

// Push a card's boolean setting into the real Module.
void applyBool(const std::string& moduleName, const std::string& settingName, bool value);

// Push a card's range slider (min, max ints) into the real dt::RangeSetting.
void applyRange(const std::string& moduleName, const std::string& settingName,
                int minValue, int maxValue);

// Push a card's enum/mode selection into the real Module.
void applyMode(const std::string& moduleName, const std::string& settingName, const std::string& selected);

// Push a colour picker's RGBA into the real Module. Values are 0..1.
void applyColor(const std::string& moduleName, const std::string& settingName,
                float r, float g, float b, float a);

// Push a keybind (Win32 VK code, 0 = none) into the real Module.
void applyKeybind(const std::string& moduleName, int virtualKey);

// Advance one hotbar square to the next selectable option. The cycle itself
// runs on the real SlotMapSetting rather than in the widget, so the constraint
// on what may be picked is applied where it is defined and the GUI never holds
// a second copy of the value.
void cycleSlot(const std::string& moduleName, const std::string& settingName, int slotIndex);

// Push a full block-picker selection into the real BlockSetSetting. Called
// by the picker widget whenever the user toggles an item — same round-trip
// pattern as the other apply* seams.
void applyBlockSet(const std::string& moduleName, const std::string& settingName,
                   std::vector<std::string> ids);

// A single row in the block search list. Populated at runtime by an engine
// (BlockESP) that enumerated Registries.BLOCK; the widget never touches JNI.
struct BlockPickerRow {
    std::string id;        // "minecraft:crafting_table"
    std::string label;     // human-readable ("Crafting Table"), fallback = id
    std::uint32_t rgb;     // block's map color as 0x00RRGGBB (0 = unknown)
};

// The block picker widget calls this once per frame to fetch the current
// registry snapshot. Whoever provides it (the engine) is responsible for
// keeping the vector sorted / stable. Setting the provider to nullptr means
// "no blocks known yet" — the picker shows an empty list.
void setBlockPickerProvider(std::function<const std::vector<BlockPickerRow>&()> provider);
const std::vector<BlockPickerRow>* blockPickerRows();

// Pull the real Module's enabled flag (so external toggles reflect in the GUI).
bool isEnabled(const std::string& moduleName);
std::string moduleDescription(const std::string& moduleName);
int bindMode(const std::string& moduleName);
int bindKey(const std::string& moduleName);
void applyBindMode(const std::string& moduleName, int mode);

// The panel's own appearance, for HUD elements that have to agree with it.
//
// Two values, published through the same seam every other GUI fact reaches a
// module by. A module cannot include theme.hh: it is part of the gui's single
// translation unit and pulls the whole ImGui/render stack behind it, which is
// exactly the coupling this header exists to prevent.

// True while the panel's "Glass" option is on. A HUD element that frosts its
// own background should do it only when the client as a whole is frosted —
// otherwise one surface is acrylic and the rest are flat cards.
bool isGlassEnabled();

// The accent the user picked in the panel, 0..1. HUD elements that are meant to
// match the client (rather than carry their own colour setting) read this so a
// change in the sidebar reaches them without a second picker to keep in sync.
void clientAccent(float& r, float& g, float& b, float& a);

// True while the HUD editor is open.
//
// A HUD element that only exists under some condition — a target on screen, a
// keybind held — has nothing to publish a bbox from while the editor is open,
// so the editor has nothing to put a drag handle around and the element cannot
// be placed at all. Such an element reads this and draws itself with stand-in
// content instead, which is what makes it positionable.
bool isHudEditorActive();

// True while the ClickGui overlay is on screen and owns the mouse.
//
// Modules that synthesize input (SendInput clicks, key presses) MUST consult
// this and stand down. The overlay is an ImGui surface drawn inside the game's
// GL frame — it is NOT a Minecraft Screen, so MinecraftClient.currentScreen is
// null while it is open and a `currentScreen == null` check does not see it.
// Without this, a synthetic click lands on the panel and toggles modules or
// drags sliders under the user's cursor.
bool isOverlayVisible();

/* @a baked icon from the client's own atlas, by name and by exact pixel size.
 *
 * Returns an ImTextureID as a plain integer so this header stays free of
 * ImGui -- ImTextureID is an ImU64 in this build. Zero when the atlas has no
 * such name or has not started yet, which callers must treat as "draw nothing"
 * rather than as an error.
 *
 * The atlas rasterises vector sources at the size asked for rather than
 * scaling one bake, which is what keeps small geometric marks from breaking
 * up. Ask for the size you are going to draw at. */
unsigned long long iconTexture(const std::string& name, int pixel_size);

/* @where the panel is, in framebuffer pixels.

   A HUD element that answers the mouse needs this: the HUD draws underneath the
   panel, so a click landing inside the panel's rect belongs to the panel and
   must not fall through. Returns false, and leaves the outputs alone, when the
   panel is not up. */
bool panelRect(float& x, float& y, float& w, float& h);

// Swallow the user's input at the window level, before the game sees it.
// Raised by a module that is driving a Minecraft screen on the player's
// behalf. Levels:
//
//   0  off.
//   1  mouse buttons only. The player keeps walking; what they cannot do is
//      land an attack or a use next to the module's own traffic.
//   2  mouse buttons and key-downs. For while a screen is actually held open,
//      or while movement is being suppressed — at that point Windows key
//      repeat has to be stopped too, or it re-presses what was just zeroed.
//
// Mouse MOVEMENT and key RELEASES always pass at every level. ALWAYS pair a
// non-zero level with an eventual 0.
//
// More than one module can hold the filter at once; the window runs at the
// highest level any owner is asking for, and releasing one owner never drops
// another's lock. Use a distinct owner id per module.
enum : int {
    FILTER_OWNER_AUTOTOTEM = 0,   // legacy caller of setInputFilter()
    FILTER_OWNER_REFILL    = 1,
    // Anti Action's window-level fallback: swallows right-clicks while a use
    // it would have blocked is lined up, when the doItemUse hook is unavailable.
    FILTER_OWNER_ANTIACTION = 2,
};

void setInputFilterFor(int owner, int level);

// Equivalent to setInputFilterFor(FILTER_OWNER_AUTOTOTEM, level).
void setInputFilter(int level);

// Freecam's input capture, separate from the filter levels above.
//
// While on, and only while the ClickGui is closed, the window procedure
// swallows mouse button presses, the wheel and the DOWN messages of the fly
// keys (W A S D Space Shift Ctrl) before GLFW sees them; releases, mouse
// movement and every other key pass. The module still reads the swallowed
// keys through GetAsyncKeyState. ALWAYS pair `true` with an eventual `false`.
void setFreecamCapture(bool on);

// Wheel notches accumulated while the capture was on, since the last call.
// Positive = wheel up. Fractional high-resolution deltas carry over.
int takeFreecamWheelNotches();

// Save/load GUI, theme and HUD state into/from the profile JSON
void saveGuiState(nlohmann::json& j);
void loadGuiState(const nlohmann::json& j);
void switchProfile(const std::string& profile);
const std::string& currentProfile();
float consumePendingScale();

// Client activation keybind (default VK_INSERT)
int guiToggleKey();
void setGuiToggleKey(int vk);
std::string vkToString(int vk);
void setMenuKeyBinding(bool active);
bool isMenuKeyBinding();

} // namespace dt::vgui::bridge
