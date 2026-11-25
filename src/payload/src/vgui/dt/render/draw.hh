#pragma once

/* @dt::draw:
	the shape vocabulary the gui is built from, expressed on top of the framework render engine.

	one for one with the primitives the design was authored against: a rounded card, a hairline
	border, a gaussian drop shadow that is cut out under its own shape, a fading hairline divider,
	and tinted icon blits.
*/
namespace dt::draw
{
	/* @rounded_rect:
		solid rounded card
	*/
	void rounded_rect(float x, float y, float w, float h, float radius, hue::c_color color);

	/* @border:
		hairline outline drawn on the outside edge of the given rectangle
	*/
	void border(float x, float y, float w, float h, float radius, float thickness, hue::c_color color);

	/* @shadow:
		gaussian ambient shadow. the area under the shape itself is left untouched so the card
		on top of it never darkens.
	*/
	void shadow(float x, float y, float w, float h, float radius, float spread,
		float offset_x, float offset_y, hue::c_color color);

	/* @circle:
		filled disc
	*/
	void circle(float center_x, float center_y, float radius, hue::c_color color);

	/* @bubble / @bubble_border:
		rounded rectangle with a radius per corner, built as one convex path so a translucent fill
		never seams. this is what lets a chat bubble tuck its corner in towards its avatar.
	*/
	void bubble(float x, float y, float w, float h,
		float top_left, float top_right, float bottom_right, float bottom_left, hue::c_color color);

	void bubble_border(float x, float y, float w, float h,
		float top_left, float top_right, float bottom_right, float bottom_left,
		float thickness, hue::c_color color);

	/* @gradient_line:
		hairline divider whose opacity follows sin(pi * t) ^ 1.8, so it fades out at both ends
	*/
	void gradient_line(float x, float y, float w, float thickness, hue::c_color color);

	/* @gradient_rect:
		rounded card filled with a linear two stop gradient.

		Built by laying the shape down solid and then re-colouring its vertices along
		the gradient axis, rather than by stacking strips: a rounded shape drawn as
		strips seams at every join once the fill is translucent, and its corners stop
		being round. ImGui's own AddRectFilledMultiColor has the same problem — it
		cannot round corners at all.
	*/
	void gradient_rect(float x, float y, float w, float h, float radius,
		hue::c_color from, hue::c_color to, bool vertical = false);

	/* @spectrum_capsule:
		the full hue wheel laid along a rounded rail, ends included. `alpha` scales
		the whole strip so it can fade with the drawer it lives in.
	*/
	void spectrum_capsule(float x, float y, float w, float h, float alpha);

	/* @glow:
		soft circular bloom centred on a point. the light a control gives off, as
		opposed to shadow(), which is the dark a card casts.
	*/
	void glow(float center_x, float center_y, float radius, float spread, hue::c_color color);

	/* @chevron:
		vector arrow used by the collapse and back controls
	*/
	void chevron(float center_x, float center_y, float size, int direction, float thickness, hue::c_color color);

	/* @disclosure_arrow:
		the profile panel's arrow. `facing` is a CONTINUOUS -1 (pointing up) to +1
		(pointing down) rather than a flag, so the control can be drawn mid-rotation
		on any frame of the spring that opens the panel.
	*/
	void disclosure_arrow(float center_x, float center_y, float size, float facing,
		float thickness, hue::c_color color);

	/* @restart_glyph:
		the circular arrow that means "put this back how it was". Drawn rather than
		shipped, because the svg rasteriser has no arc command and the draw list
		does.
	*/
	void restart_glyph(float x, float y, float size, float thickness, hue::c_color color);

	/* @power_glyph:
		a ring broken at the top with a stroke rising through the gap — the mark
		every device has used for "off" since the seventies.
	*/
	void power_glyph(float x, float y, float size, float thickness, hue::c_color color);

	/* @copy_glyph:
		the two offset sheets that mean "copy", stroked rather than filled so it
		reads at the 12px the hex field gives it
	*/
	void copy_glyph(float x, float y, float size, float thickness, hue::c_color color);

	/* @check_glyph:
		the tick the copy button turns into once the value is on the clipboard
	*/
	void check_glyph(float x, float y, float size, float thickness, hue::c_color color);

	/* @texture:
		tinted texture blit with optional corner rounding
	*/
	void texture(ImTextureID id, float x, float y, float w, float h, float radius, hue::c_color color);

	/* @icon:
		looks the texture up in the atlas and blits it with the given tint
	*/
	void icon(const std::string& name, float x, float y, float size, hue::c_color color);

	/* @rounded_icon:
		same as icon() but clipped to a rounded shape, used by the avatar
	*/
	void rounded_icon(const std::string& name, float x, float y, float size, float radius, hue::c_color color);

	/* @push_clip / @pop_clip:
		scissor helpers for scrolling regions
	*/
	void push_clip(float x, float y, float w, float h);
	void pop_clip();
}
