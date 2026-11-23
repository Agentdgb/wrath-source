#pragma once

/* @core::cursor_guard:
	frees the mouse pointer while the overlay is open.

	Minecraft (GLFW, CURSOR_DISABLED) keeps the pointer captive by calling
	ClipCursor to pin it to the window and SetCursorPos to re-centre it on every
	input poll. Fighting that through window focus is unreliable — the game
	re-grabs the moment it is activated, which is why the cursor kept snapping
	back to the middle after alt-tabbing.

	Instead we intercept those two user32 entry points inside the game process
	and neutralise them while the panel is visible. GLFW keeps calling them and
	simply has no effect; the moment the panel closes the hooks pass through
	again and the game's own cursor handling resumes untouched.
*/
namespace core::cursor_guard
{
	/* install / remove the user32 detours. Idempotent. */
	bool install();
	void remove();

	/* while active, SetCursorPos and ClipCursor become no-ops */
	void set_active(bool active);
	bool active();
}
