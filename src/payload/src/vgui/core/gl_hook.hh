#pragma once

/* @core::gl_hook:
	renders the client INSIDE Minecraft's own OpenGL frame.

	This replaces the previous design, where the gui lived in a separate D3D11
	window that had to be tracked, z-ordered, focused, clamped and hidden by
	hand. Drawing inside the game's frame makes every one of those problems
	disappear: minimising, alt-tabbing, moving, resizing and fullscreen are the
	game's concern, and the overlay simply is not there when the game is not.

	Mechanism: MinHook detour on opengl32!wglSwapBuffers. On each frame we
	switch to OUR OWN GL context (created sharing the game's), draw, and switch
	back before calling the original. Using a private context — rather than
	drawing on the game's — is what keeps blaze3d / Sodium / Iris state
	completely untouched.
*/

#include <windows.h>

namespace core::gl_hook
{
	/* @install:
		detour wglSwapBuffers and subclass the game window. Safe to call once
		the game window exists; returns false if either step fails.
	*/
	bool install(HWND game_window);

	/* @request_shutdown:
		ask the render thread to tear the gui down. GL objects must be destroyed
		on the thread that owns the context, so teardown cannot be done from the
		caller. Blocks (bounded) until the render thread reports done.
	*/
	void request_shutdown();

	/* @uninstall:
		remove the detour and the window subclass. Call only after
		request_shutdown() has completed.
	*/
	void uninstall();

	/* @set_hotkeys:
		virtual key codes the window subclass swallows unconditionally, so the
		game never reacts to the panel's own toggle and panic keys.
	*/
	void set_hotkeys(int toggle_vk, int panic_vk);

	/* @set_input_filter:
		swallow the user's input at the window level, before the game sees it.

		Raised by a module that is driving a Minecraft screen on the player's
		behalf — AutoTotem's inventory mode opens the inventory for a couple of
		hundred milliseconds, and a stray click in that window would drag one of
		the player's own items instead.

		Levels, least to most intrusive:

		  0 — off.

		  1 — mouse buttons only. Stops the attack/use that must not land next
		      to the module's own container traffic, while leaving the keyboard
		      alone so the player keeps walking.

		  2 — mouse buttons AND key-downs. For while a screen is actually held
		      open, and while movement is being suppressed: there, Windows key
		      REPEAT has to be stopped too or it re-presses what the module just
		      zeroed.

		Mouse MOVEMENT and key RELEASES always pass at every level. Swallowing
		releases would latch whatever was held when the filter went up.
	*/
	void set_input_filter(int level);
	int  input_filter();

	/* true once the gui has been initialised on the render thread */
	bool ready();
	bool installed();

	/* the game's framebuffer size, as observed in the last frame */
	void framebuffer_size(int& w, int& h);

	void set_stream_proof(bool enabled);
	bool stream_proof();
}
