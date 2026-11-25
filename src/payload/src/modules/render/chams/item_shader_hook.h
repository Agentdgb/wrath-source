#pragma once
namespace dt::modules::itemshader {
// Attaches the HeldItemRenderer.renderFirstPersonItem detour once per process
// (idempotent; a repeat call is a load). Safe to call with the ItemShader module
// off: installing says nothing about the glow, which follows set_wanted /
// set_effect in item_shader_gl.h and stays the module's own business.
bool install_hook();
void shutdown_hook();
void shutdown_gl();

// Process-wide "draw no first-person hands" switch, used by Freecam. While set
// the detour returns without calling the original, which on 1.21 .. 1.21.11
// removes both hands: renderItem (method_22976) reaches the arm and the held
// item only through renderFirstPersonItem (method_3228) -- the empty-hand arm
// (renderArmHoldingItem, method_3219) is called from inside it, never from
// renderItem directly (javap, 1.21.4 and 1.21.11). Independent of the glow:
// clearing it never touches the ItemShader module's state.
void set_hands_hidden(bool on);
bool hands_hidden();
}
