// notifier.h — the toast that says what you just switched on or off.
//
// One line, three parts: the module's category, the module, and what happened
// to it, separated by dots quiet enough that the eye reads the words and not
// the punctuation.
//
// Split into two passes like every other HUD element here, and for the same
// reason: the frosted pane is a raw-GL shader pass that has to run against the
// game's framebuffer BEFORE any ImGui state exists this frame, while the words
// are ImGui draw commands that have to run inside it. The background pass owns
// the clock, the queue and the layout; the text pass only reads what it left
// behind, so the two halves cannot disagree about where a card is.
//
// @the queue crosses a thread boundary, which is the one sharp edge.
//
// Module::setEnabled runs on whichever thread flipped the switch, and it is
// worth naming which ones those really are. A keybind is NOT one of them: binds
// are read by the hotkey poll, which posts through the event bus, and the GUI's
// own toggles run on the render thread. The two that genuinely come from
// outside are the bootstrap thread, which switches on everything the saved
// profile had enabled, and whichever thread runs the unload, which switches all
// of it back off. Either can post while the draw pass is halfway through the
// list it is drawing, so arrivals land in a mutex-guarded pending queue that the
// background pass drains once per frame; everything animated is owned by the
// render thread alone and never touched from outside it.

#pragma once

#include <functional>
#include <string>

namespace dt::modules::notifier {

extern bool g_enabled;

struct settings
{
    float scale = 1.0f;      /* 0.5 .. 2.5 */
    float rounding = 1.0f;   /* 0 .. 2, multiplies the base corner radius */

    /* Seconds a toast stays at full strength before it starts to leave. */
    float duration = 2.5f;
};

extern settings g_settings;

/* Installed by the module so the renderer can pull live values without the
   render pass ever touching a Setting object. */
extern std::function<void(settings&)> g_settings_provider;

/* @push:
    queues one toast. Safe from any thread.

    Dropped silently until the overlay has drawn at least one frame: loading a
    config switches every saved module on, and without that gate the client would
    open onto a stack of eighteen notifications for things the user did not just
    do. */
void push(std::string category, std::string module, bool enabled);

/* The clock, the queue, the layout and the frosted panes. Raw GL, before
   ImGui::NewFrame, straight onto the bound framebuffer. */
void render_background(int display_w, int display_h, unsigned int bound_fb);

/* The words, on ImGui's foreground draw list in framebuffer pixels. Draws only
   what the background pass laid out. */
void render_text(int display_w, int display_h);

/* HUD editor drag hooks. The bbox is the union of the cards currently on
   screen — or of the stand-in card the editor is given when there are none. */
void query_bbox(float& x, float& y, float& w, float& h);
void set_top_left(float x, float y);

/* @reset:
    drops the queue and sends everything already on screen into its fade.

    Called when the module is switched off. It deliberately does NOT clear the
    animated list outright: that list belongs to the render thread, and emptying
    it from the thread that flipped the switch would pull it out from under the
    draw pass mid-iteration. The request is left under the lock and actioned at
    drain time, where the render thread already owns the list. */
void reset();

} // namespace dt::modules::notifier
