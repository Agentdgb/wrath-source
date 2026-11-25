#pragma once

/* @dt::hud_editor:
	the fullscreen HUD-editor overlay.

	activated when the sidebar's "HUD" entry is selected. Draws a grid across the
	whole game framebuffer, a rounded "Exit" pill top-left, a wide "HUD Settings"
	button bottom-centre, and a right-side slide-in panel with the list of HUD
	elements (ArrayList, Watermark, …) plus a sub-view for each element's
	settings. The elements themselves become directly draggable on the grid.

	The overlay lives in FRAMEBUFFER pixel space, not the dt canvas — it
	renders on ImGui's foreground draw list which gl_hook skips during the
	panel-scale transform. This is what lets the grid go edge-to-edge on any
	resolution regardless of where the panel sits.
*/

#include <functional>
#include <nlohmann/json.hpp>

struct ImDrawList;

namespace dt::hud_editor
{
	/* @is_active:
		true while the sidebar has "HUD" selected. Read by gl_hook / other views
		to know they should defer to the editor for input.
	*/
	bool is_active();
	void set_active(bool a);

	/* @set_exit_callback:
		wired by c_app so the "Exit" pill can return to whichever category the
		user was on before entering HUD mode. The editor itself has no idea
		what a "category" is.
	*/
	void set_exit_callback(std::function<void()> cb);

	/* @tick:
		advances every animation the editor owns. run once per frame, whether
		or not is_active() — spring returns to rest cleanly on exit.
	*/
	void tick(float delta);

	/* @render:
		paints the overlay on ImGui's foreground draw list, all coordinates in
		framebuffer pixels. Called from c_app::frame after ImGui::NewFrame.
		No-op when not active except for driving the fade-out tail.
	*/
	void render(int fb_w, int fb_h);

	/* @hud_offset:
		global (x, y) offset applied by every HUD-anchored module (ArrayList
		today, Watermark tomorrow). The editor mutates this when the user
		drags the element on the grid.
	*/
	void arraylist_offset(float& out_x, float& out_y);
	void set_arraylist_offset(float x, float y);

	void save_positions(nlohmann::json& out);
	void load_positions(const nlohmann::json& in);
}
