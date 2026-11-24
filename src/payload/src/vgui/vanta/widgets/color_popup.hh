#pragma once

/* @vanta::colour_popup:
	the colour editor, as a window rather than as part of whatever opened it.

	There is exactly ONE of these in the client, and it is a namespace rather than
	a class for that reason: a second colour editor open at the same time would be
	two answers to the same question. Any control that owns a colour asks this to
	open on it; the shell draws it last, over everything, and the control that
	asked never has to make room for it.

	Why a window at all: the editor used to expand inside the row that owned it,
	which pushed the rest of the list down, could not be moved out of its own way,
	and forced the panel it lived in to be tall enough to contain it. Detaching it
	means the picker can sit wherever it is useful — beside the thing being
	recoloured rather than on top of it — and where the user puts it is where the
	NEXT one opens, because the position outlives any one editing session.
*/
namespace vanta
{
	class c_color_setting;
}

namespace vanta::colour_popup
{
	/* @open:
		points the editor at a setting and shows it. Opening it on the setting it
		is already showing closes it instead, so the control that opens it is also
		the control that puts it away.

		The target is a raw pointer with an owner elsewhere. c_color_setting's
		destructor clears it if it points at itself, which is the same contract the
		hex-field focus is written against — a drawer torn down while its colour is
		being edited cannot leave this dangling.
	*/
	void open(c_color_setting* target, std::string title);
	void close();

	/* @open_for:
		true when the editor is currently showing this particular setting. Callers
		use it to light their row up while it is the one being edited.
	*/
	bool open_for(const c_color_setting* setting);

	/* @forget:
		called by c_color_setting's destructor. Never call it directly.
	*/
	void forget(const c_color_setting* setting);

	/* @render:
		draws the editor and consumes its own input. Called by the shell LAST, on
		the real cursor, after everything the editor is allowed to cover.
	*/
	void render(const frame_input_t& input);

	/* @rect:
		where the window is, in canvas coordinates, as it was drawn last frame.
		False when it is closed.

		Published for the same reason the account panel publishes its own: an
		immediate-mode view under this one would otherwise answer clicks that
		never reached it.
	*/
	bool rect(float& x, float& y, float& w, float& h);
}
