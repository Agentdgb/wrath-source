#pragma once
#include <string>

/* @vanta::intro:
	the sequence the client plays on inject, once, before it hands the screen back.

	Order, and the reason for it: the greeting comes FIRST, on the untouched game,
	because it is addressed to a person and reads as one thing arriving rather than
	as a caption on an effect. Only then does the violet flood the frame, and only
	once the frame is violet is there a surface for the mark to land on. Coming
	out, the letters leave before the colour does — the wordmark is the message and
	the flood is the stage it was standing on, so the stage cannot be struck first.

	It renders in FRAMEBUFFER pixels on ImGui's foreground draw list, which gl_hook
	exempts from the panel-scale transform. That is what lets it go edge to edge on
	any resolution regardless of where the panel sits, or whether the panel is even
	on screen — the sequence plays whether or not the gui is visible.

	Type is set from the arraylist's font pool rather than the vanta atlas: the gui
	atlas only bakes 9.5..16px and a wordmark at splash size would be an upscale of
	small glyphs. The pool already carries proxima bold up to 96px.
*/
namespace vanta::intro
{
    // 0: Minecraft name, 1: logo only, 2: custom greeting name.
    int mode();
    void set_mode(int value);
    std::string custom_name();
    void set_custom_name(const std::string& value);
    std::string custom_wordmark();
    void set_custom_wordmark(const std::string& value);

	/* @begin:
		arms the sequence. Called once, at the end of c_app::initialize.

		It does not start playing immediately: the greeting names the player, and
		the name arrives from the JVM on a worker thread some frames later. The
		sequence waits a moment for it and gives up gracefully if it never lands.
	*/
	void begin();

	/* @just_finished:
		true on the ONE frame the sequence ends, and false ever after.

		A one-shot rather than a flag the caller has to clear: the shell asks once
		per frame and the answer is only ever yes once, so there is no state for
		anyone to forget to reset.
	*/
	bool just_finished();

	/* @active:
		true while anything is still on screen. The shell asks so it can keep
		ticking us on frames where the panel itself is hidden.
	*/
	bool active();

	/* @tick:
		advances the sequence. Safe to call every frame, no-op once finished.
	*/
	void tick(float delta);

	/* @render:
		paints the current frame of the sequence, in framebuffer pixels.
	*/
	void render(int fb_w, int fb_h);
}
