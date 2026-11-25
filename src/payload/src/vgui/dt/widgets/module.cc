#include "../../includes.hh"
#include "../../bridge.h"   // bridge::applyToggle — plumb card toggle → real Module::setEnabled

#include "bind_popup.hh"
#include "core/setting/NumericPrecision.h"
#include <algorithm>
#include <cctype>
#include "../../framework/render/stb/stb_image.hh"

namespace dt
{
	/* ------------------------------------------------------------------ */
	/* slider                                                              */
	/* ------------------------------------------------------------------ */

	namespace
	{
		/* @slider_fill:
			the coloured half of a track.

			Not a flat accent: the fill runs from a deep, quiet violet at its start
			up to the full accent at its leading edge, so the eye is pulled towards
			the end of the bar where the value actually lives. The strip in front of
			the ball is where the colour is brightest and where the bloom is centred,
			which is what makes the two read as one object rather than as a bar with
			a dot parked on it.
		*/
		void slider_fill(float from_x, float to_x, float track_y, float track_height,
			float radius, float engaged, float alpha)
		{
			const float width = std::max(track_height, (to_x - from_x) + radius);

			if (engaged > 0.05f)
			{
				// the filled half glows while the row is engaged
				draw::shadow(from_x, track_y, width, track_height, radius, 10.f * engaged, 0.f, 0.f,
					theme::accent_violet.alpha(static_cast<int>(70.f * engaged * alpha)));
			}

			/* deep at the tail, full accent at the head */
			draw::gradient_rect(from_x, track_y, width, track_height, radius,
				theme::accent_violet_deep.lerp(hue::c_color(0, 0, 0, 255), 0.28f).modulate(alpha),
				theme::accent_violet.modulate(alpha));
		}

		/* @slider_knob:
			the white ball, and the diffusion of accent that gathers behind it.

			The bloom is three passes at falling opacity and rising radius rather
			than one: a single shadow gives a visible ring where its falloff ends,
			and the whole point of this shape is that the colour has no edge.
		*/
		void slider_knob(float center_x, float center_y, float radius, float engaged, float alpha)
		{
			/* @three passes, but only when there is something to look at.

			   Each pass is an AddShadowCircle, and a shadow is not a cheap call —
			   it tessellates the outline and emits roughly eleven vertices per
			   segment. Three of them per ball, on every ball, on every frame, is a
			   few thousand vertices per drawer spent on a bloom nobody is looking
			   at: a row at rest is a row the cursor is nowhere near.

			   So the falloff is only built out while the row is engaged. At rest one
			   pass carries the whole (much fainter) bloom, which is
			   indistinguishable at that opacity and a third of the cost. */
			const float strength = 0.55f + 0.45f * engaged;
			const int passes = (engaged > 0.02f) ? 3 : 1;

			for (int pass = 0; pass < passes; ++pass)
			{
				const float t = static_cast<float>(pass) * 0.5f;

				draw::glow(center_x, center_y, radius + 1.f + 5.f * t, 11.f + 13.f * t,
					theme::accent_violet.alpha(static_cast<int>((72.f - 34.f * t) * strength * alpha)));
			}

			/* grows a hair when engaged, and carries a soft dark rim so it stays
			   legible sitting on top of a bright fill */
			const float ball = radius - 0.5f + 1.2f * engaged;

			draw::circle(center_x, center_y, ball + 1.3f, hue::c_color(0, 0, 0, static_cast<int>(80.f * alpha)));
			draw::circle(center_x, center_y, ball, theme::text_white.modulate(alpha));
		}
	}

	c_slider_setting::c_slider_setting(std::string name, double minimum, double maximum, double value, double step, std::string suffix)
		: c_setting(std::move(name)), m_minimum(minimum), m_maximum(maximum), m_value(value), m_step(step), m_suffix(std::move(suffix))
	{
		this->m_visual_value = static_cast<float>((value - minimum) / (maximum - minimum));
	}

	c_slider_setting::~c_slider_setting()
	{
		if (s_value_focus == this)
			s_value_focus = nullptr;
	}

	void c_slider_setting::clear_value_focus()
	{
		if (s_value_focus != nullptr)
			s_value_focus->submit_value();

		s_value_focus = nullptr;
	}

	void c_slider_setting::begin_value_edit()
	{
		if (s_value_focus != nullptr && s_value_focus != this)
			s_value_focus->submit_value();

		s_value_focus = this;

		/* Opens EMPTY rather than pre-filled with the current number. The reason to
		   click a value is almost always to replace it, and a field that starts
		   full makes that a select-all before it is a typing gesture. The old value
		   stays on screen behind the caret until the first keystroke, so nothing is
		   lost by it. */
		this->m_typed.clear();
	}

	bool c_slider_setting::apply_typed(const std::string& text)
	{
		if (text.empty())
			return false;

		double parsed = 0.0;

		try
		{
			parsed = std::stod(text);
		}
		catch (...)
		{
			return false;
		}

		if (!std::isfinite(parsed))
			return false;

		const double snapped = (this->m_step > 0.0)
			? std::round(parsed / this->m_step) * this->m_step
			: parsed;

		this->m_value = std::clamp(snapped, this->m_minimum, this->m_maximum);
		return true;
	}

	void c_slider_setting::on_character(char character)
	{
		if (s_value_focus != this)
			return;

		const bool digit = (character >= '0' && character <= '9');
		const bool point = (character == '.' || character == ',');
		const bool sign = (character == '-' && this->m_typed.empty() && this->m_minimum < 0.0);

		if (!digit && !point && !sign)
			return;

		/* one decimal point, and never a second */
		if (point && this->m_typed.find('.') != std::string::npos)
			return;

		if (this->m_typed.size() >= 9)
			return;

		this->m_typed.push_back(point ? '.' : character);
	}

	void c_slider_setting::on_backspace()
	{
		if (s_value_focus != this || this->m_typed.empty())
			return;

		this->m_typed.pop_back();
	}

	void c_slider_setting::submit_value()
	{
		this->apply_typed(this->m_typed);
		this->m_typed.clear();
	}

	void c_slider_setting::update(float delta)
	{
		const float target = static_cast<float>((this->m_value - this->m_minimum) / (this->m_maximum - this->m_minimum));
		this->m_visual_value = anim::lerp(this->m_visual_value, target, 16.f, delta);
	}

	void c_slider_setting::render(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		const float label_y = y + 9.f;

		/* track, a fully rounded capsule */
		const float track_y = y + 27.f;
		constexpr float track_height = 8.f;
		constexpr float track_radius = track_height * 0.5f;

		const float track_center_y = track_y + track_height * 0.5f;

		/* @travel:
			the ball is wider than the ribbon it rides, so the value travels between
			the two points where it still sits fully inside the track rather than
			between the track's own edges.

			Deliberately a CONSTANT radius and not the animated one below: the drag
			maps the cursor onto this same span, and a span that breathed with the
			hover would slide the value out from under the cursor every time the
			pointer entered the row.
		*/
		constexpr float knob_radius = 7.f;

		const float travel_x = x + knob_radius;
		const float travel_w = std::max(1.f, w - knob_radius * 2.f);

		/* the whole row is the grab area, not just the 8px ribbon */
		const bool hovered = input.hovered(x, y, w, this->height() - 4.f);
		this->m_hover = anim::lerp(this->m_hover, hovered ? 1.f : 0.f, 16.f, input.m_delta);

		const bool was_dragging = this->m_dragging;

		/* Started by a fresh press only — see the note in c_color_setting. A button
		   already held when the cursor wanders onto a row must not take it over,
		   or one drag across the drawer rewrites every value it crosses. */
		/* the chip sits inside the row's grab area, so a click meant for the field
		   would otherwise also start a drag and overwrite the value being typed */
		const bool on_chip = (s_value_focus == this);

		if (hovered && input.m_left_clicked && !on_chip)
		{
			this->m_grab.kick();
			this->m_dragging = true;
		}

		if (!input.m_left_down)
			this->m_dragging = false;

		/* capture the desktop anchor on the frame the grab starts */
		if (this->m_dragging && !was_dragging && this->m_screen_anchored)
		{
			GetCursorPos(&this->m_anchor_cursor);
			this->m_anchor_value = this->m_value;

			// Travel in desktop pixels at grab time. Using the effective scale
			// (not the requested one) keeps this honest when the panel is
			// clamped to fit the game window.
			const float s = dt::g_app->panel_scale();
			this->m_anchor_track_px = std::max(1.f, travel_w * s);
		}

		if (this->m_dragging)
		{
			double percent;

			if (this->m_screen_anchored)
			{
				/* Resolve in DESKTOP pixels. A logical-space drag would feed
				   its own result back into its input here — the value resizes
				   the panel, the panel re-maps logical -> screen, and the
				   handle crawls away from the cursor. Absolute cursor motion
				   has no such loop. */
				POINT now{};
				GetCursorPos(&now);

				const double delta_px = static_cast<double>(now.x - this->m_anchor_cursor.x);
				const double anchor_pct =
					(this->m_anchor_value - this->m_minimum) / (this->m_maximum - this->m_minimum);

				percent = std::clamp<double>(anchor_pct + delta_px / this->m_anchor_track_px, 0.0, 1.0);
			}
			else
			{
				percent = std::clamp<double>((input.m_cursor.x - travel_x) / travel_w, 0.0, 1.0);
			}

			const double raw = this->m_minimum + (this->m_maximum - this->m_minimum) * percent;

			this->m_value = std::round(raw / this->m_step) * this->m_step;
			this->m_value = std::clamp(this->m_value, this->m_minimum, this->m_maximum);
		}

		const float engaged = std::max(this->m_dragging ? 1.f : 0.f, this->m_hover);

		/* label brightens as the row is engaged, so the active setting is obvious */
		draw_text(this->label(), x, label_y, font_face::regular, 12.f,
			theme::text_secondary.lerp(theme::text_white, engaged * 0.6f).modulate(alpha),
			align_left | align_middle);

		/* @the value chip, which is also a field.

		   A slider is a coarse instrument: it is the right control for "a bit more"
		   and the wrong one for "exactly 37". The number was already on screen, so
		   the cheapest way to offer precision is to let the readout be typed into
		   rather than to add a second control beside it. */
		{
			const bool editing = (s_value_focus == this);

			const std::string shown = editing
				? (this->m_typed.empty() ? std::string{} : this->m_typed)
				: std::format("{:.{}f}{}", this->m_value, ::dt::numericDecimalPlaces(this->m_step), this->m_suffix);

			/* the resting number decides the width even while empty, so the chip
			   does not collapse the moment the first character is deleted */
			const std::string measured = std::format("{:.{}f}{}", this->m_value, ::dt::numericDecimalPlaces(this->m_step), this->m_suffix);

			constexpr float chip_height = 18.f;
			const float chip_width = std::max(34.f, text_width(measured, font_face::bold, 11.5f) + 16.f);
			const float chip_x = x + w - chip_width;
			const float chip_y = label_y - chip_height * 0.5f;

			const bool chip_hovered = input.hovered(chip_x, chip_y, chip_width, chip_height);

			this->m_chip_hover = anim::lerp(this->m_chip_hover, chip_hovered ? 1.f : 0.f, 16.f, input.m_delta);
			this->m_chip_focus = anim::lerp(this->m_chip_focus, editing ? 1.f : 0.f, 15.f, input.m_delta);

			if (input.m_left_clicked)
			{
				if (chip_hovered)
					this->begin_value_edit();
				else if (editing)
					clear_value_focus();
			}

			const float focus = anim::saturate(this->m_chip_focus);

			if (focus > 0.02f)
			{
				draw::shadow(chip_x, chip_y, chip_width, chip_height, 5.f, 10.f * focus, 0.f, 1.f,
					theme::accent_violet.alpha(static_cast<int>(70.f * focus * alpha)));
			}

			draw::rounded_rect(chip_x, chip_y, chip_width, chip_height, 5.f,
				theme::bg_element.lerp(theme::bg_card, this->m_chip_hover).modulate(alpha));

			draw::border(chip_x, chip_y, chip_width, chip_height, 5.f, 1.f,
				theme::border
					.lerp(theme::accent_violet, std::max(engaged * 0.5f, focus))
					.modulate(alpha));

			if (!shown.empty())
			{
				draw_text(shown, chip_x + chip_width * 0.5f, label_y, font_face::bold, 11.5f,
					theme::text_white.modulate(alpha), align_center | align_middle);
			}

			if (focus > 0.02f)
			{
				const float caret_x = chip_x + chip_width * 0.5f
					+ text_width(shown, font_face::bold, 11.5f) * 0.5f + 2.f;

				draw::rounded_rect(caret_x, label_y - 6.f, 1.5f, 12.f, 0.75f,
					theme::accent_violet.modulate(alpha * focus));
			}
		}

		const float knob_x = travel_x + travel_w * this->m_visual_value;

		const hue::c_color track_bg = theme::bg_element.lerp(theme::accent_violet_deep, 0.22f)
			.lerp(theme::bg_element.lerp(theme::text_white, 0.08f), engaged).modulate(alpha);
		const hue::c_color track_border = theme::border.lerp(theme::accent_violet, 0.20f).modulate(alpha);

		draw::rounded_rect(x, track_y, w, track_height, track_radius, track_bg);
		draw::border(x, track_y, w, track_height, track_radius, 1.f, track_border);

		slider_fill(x, knob_x, track_y, track_height, track_radius, engaged, alpha);

		if (const float grab = this->m_grab.update(input.m_delta); grab > 0.01f)
		{
			// the ripple leaves from the ball, so the eye is told where the value now lives
			draw::glow(knob_x, track_center_y, knob_radius + 2.f, 10.f + 22.f * (1.f - grab),
				theme::accent_violet.alpha(static_cast<int>(150.f * grab * alpha)));
		}

		slider_knob(knob_x, track_center_y, knob_radius, engaged, alpha);
	}

	void c_slider_setting::propagate_if_changed(const std::string& module_name)
	{
		/* Push the current slider value through to the real dt::FloatSetting
		   / IntSetting only when it actually changed. The initial NaN sentinel
		   guarantees the first call after construction syncs whatever value
		   the GUI came up with — otherwise a card built with e.g. 3.5 by
		   the demo path but backed by a real setting whose default is 3.0
		   would silently keep 3.0 until the user touched it. */
		if (this->m_value == this->m_last_pushed) return;
		this->m_last_pushed = this->m_value;
		dt::vgui::bridge::applyNumeric(module_name, this->m_name, this->m_value);
	}

	/* ------------------------------------------------------------------ */
	/* range slider                                                        */
	/* ------------------------------------------------------------------ */

	c_range_slider_setting::c_range_slider_setting(std::string name,
		double absolute_min, double absolute_max,
		double value_min, double value_max,
		double step, std::string suffix)
		: c_setting(std::move(name)),
		  m_absolute_min(absolute_min), m_absolute_max(absolute_max),
		  m_min(value_min), m_max(value_max),
		  m_step(step), m_suffix(std::move(suffix))
	{
		const double span = m_absolute_max - m_absolute_min;
		if (span > 0.0) {
			m_visual_min = static_cast<float>((m_min - m_absolute_min) / span);
			m_visual_max = static_cast<float>((m_max - m_absolute_min) / span);
		}
	}

	void c_range_slider_setting::update(float delta)
	{
		const double span = m_absolute_max - m_absolute_min;
		if (span <= 0.0) return;

		const float tgt_lo = static_cast<float>((m_min - m_absolute_min) / span);
		const float tgt_hi = static_cast<float>((m_max - m_absolute_min) / span);
		m_visual_min = anim::lerp(m_visual_min, tgt_lo, 16.f, delta);
		m_visual_max = anim::lerp(m_visual_max, tgt_hi, 16.f, delta);
	}

	void c_range_slider_setting::render(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		const float label_y = y + 9.f;

		constexpr float track_height = 8.f;
		constexpr float track_radius = track_height * 0.5f;
		constexpr float knob_radius = 7.f;

		const float track_y = y + 27.f;
		const float track_center_y = track_y + track_height * 0.5f;

		/* same inset travel as the single slider — see the note there */
		const float travel_x = x + knob_radius;
		const float travel_w = std::max(1.f, w - knob_radius * 2.f);

		const bool hovered = input.hovered(x, y, w, this->height() - 4.f);
		this->m_hover = anim::lerp(this->m_hover, hovered ? 1.f : 0.f, 16.f, input.m_delta);

		const double span = m_absolute_max - m_absolute_min;
		auto snap = [&](double raw) {
			const double snapped = std::round(raw / m_step) * m_step;
			return std::clamp(snapped, m_absolute_min, m_absolute_max);
		};
		auto pct_to_val = [&](double p) {
			return m_absolute_min + span * std::clamp(p, 0.0, 1.0);
		};

		// The ball centres, computed from the ANIMATED positions so the hit test
		// tracks what the user actually sees.
		const float thumb_x_low  = travel_x + travel_w * m_visual_min;
		const float thumb_x_high = travel_x + travel_w * m_visual_max;

		// Grab whichever ball the cursor is closer to. If they perfectly
		// overlap (min == max), pick the one that yields motion — dragging
		// left picks the low thumb, dragging right picks the high thumb.
		/* fresh press only, same reason as the single slider above */
		if (input.m_left_clicked && m_dragging == thumb::none && hovered) {
			const float cx = static_cast<float>(input.m_cursor.x);
			const float d_low  = std::abs(cx - thumb_x_low);
			const float d_high = std::abs(cx - thumb_x_high);
			if (std::abs(d_low - d_high) < 0.5f) {
				m_dragging = (cx < thumb_x_low) ? thumb::low : thumb::high;
			} else {
				m_dragging = (d_low < d_high) ? thumb::low : thumb::high;
			}
			(m_dragging == thumb::low ? m_grab_low : m_grab_high).kick();
		}
		if (!input.m_left_down) m_dragging = thumb::none;

		if (m_dragging != thumb::none) {
			const double percent = std::clamp<double>((input.m_cursor.x - travel_x) / travel_w, 0.0, 1.0);
			const double raw = pct_to_val(percent);
			const double snapped = snap(raw);
			if (m_dragging == thumb::low) {
				m_min = std::min(snapped, m_max);
			} else {
				m_max = std::max(snapped, m_min);
			}
		}

		const float engaged = std::max(m_dragging != thumb::none ? 1.f : 0.f, m_hover);

		draw_text(this->label(), x, label_y, font_face::regular, 12.f,
			theme::text_secondary.lerp(theme::text_white, engaged * 0.6f).modulate(alpha),
			align_left | align_middle);

		// Chip on the right, showing "min – max<suffix>"
		{
			const std::string value = std::format("{:.0f} - {:.0f}{}", m_min, m_max, m_suffix);
			constexpr float chip_height = 17.f;
			const float chip_width = text_width(value, font_face::bold, 11.5f) + 14.f;
			const float chip_x = x + w - chip_width;
			const float chip_y = label_y - chip_height * 0.5f;

			draw::rounded_rect(chip_x, chip_y, chip_width, chip_height, 5.f,
				hue::c_color(18, 18, 18).modulate(alpha));
			draw::border(chip_x, chip_y, chip_width, chip_height, 5.f, 1.f,
				hue::c_color(28, 28, 28).lerp(theme::accent_violet, engaged * 0.5f).modulate(alpha));

			draw_text(value, chip_x + chip_width * 0.5f, label_y, font_face::bold, 11.5f,
				theme::text_white.modulate(alpha), align_center | align_middle);
		}

		// Track
		draw::rounded_rect(x, track_y, w, track_height, track_radius,
			hue::c_color(20, 20, 20).lerp(hue::c_color(27, 27, 27), engaged).modulate(alpha));
		draw::border(x, track_y, w, track_height, track_radius, 1.f, hue::c_color(30, 30, 30).modulate(alpha));

		// Fill from the low ball to the high one
		slider_fill(thumb_x_low, thumb_x_high, track_y, track_height, track_radius, engaged, alpha);

		// The two balls, each with its own grab ripple
		auto draw_thumb = [&](float thumb_x, anim::pulse_t& pulse) {
			if (const float grab = pulse.update(input.m_delta); grab > 0.01f) {
				draw::glow(thumb_x, track_center_y, knob_radius + 2.f, 10.f + 22.f * (1.f - grab),
					theme::accent_violet.alpha(static_cast<int>(150.f * grab * alpha)));
			}
			slider_knob(thumb_x, track_center_y, knob_radius, engaged, alpha);
		};
		draw_thumb(thumb_x_low,  m_grab_low);
		draw_thumb(thumb_x_high, m_grab_high);
	}

	void c_range_slider_setting::propagate_if_changed(const std::string& module_name)
	{
		if (this->m_min == this->m_last_pushed_min
		 && this->m_max == this->m_last_pushed_max) return;
		this->m_last_pushed_min = this->m_min;
		this->m_last_pushed_max = this->m_max;
		dt::vgui::bridge::applyRange(module_name, this->m_name,
			static_cast<int>(this->m_min), static_cast<int>(this->m_max));
	}

	/* ------------------------------------------------------------------ */
	/* colour                                                              */
	/* ------------------------------------------------------------------ */

	namespace
	{
		void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b)
		{
			h = h - std::floor(h);
			const float i  = std::floor(h * 6.f);
			const float f  = h * 6.f - i;
			const float p  = v * (1.f - s);
			const float q  = v * (1.f - s * f);
			const float t  = v * (1.f - s * (1.f - f));
			switch (static_cast<int>(i) % 6) {
				case 0: r = v; g = t; b = p; break;
				case 1: r = q; g = v; b = p; break;
				case 2: r = p; g = v; b = t; break;
				case 3: r = p; g = q; b = v; break;
				case 4: r = t; g = p; b = v; break;
				case 5: r = v; g = p; b = q; break;
				default: r = g = b = 0.f;
			}
		}

		void rgb_to_hsv(float r, float g, float b, float& h, float& s, float& v)
		{
			const float mx = std::max({ r, g, b });
			const float mn = std::min({ r, g, b });
			const float d  = mx - mn;
			v = mx;
			s = (mx > 0.f) ? d / mx : 0.f;
			if (d < 1e-6f) { h = 0.f; return; }
			if      (mx == r) h = ((g - b) / d) + (g < b ? 6.f : 0.f);
			else if (mx == g) h = ((b - r) / d) + 2.f;
			else              h = ((r - g) / d) + 4.f;
			h /= 6.f;
		}

		/* @picker_geom:
			the sub-rectangles of the colour picker in row-local coordinates.
			Kept here (not in the header) because every access needs the same
			numbers, and duplicating them across render / hit-test would drift. */
		struct picker_geom
		{
			float sv_y = 26.f,     sv_h = 92.f;
			float hue_y = 128.f,   hue_h = 12.f;
			float alpha_y = 148.f, alpha_h = 12.f;
			float hex_y = 172.f,   hex_h = 28.f;
		};

		constexpr picker_geom k_picker{};

		/* the rails wear the same white ball the sliders do, so the whole client
		   has one handle rather than one per widget */
		constexpr float k_rail_knob = 7.f;

		char hex_digit(int value)
		{
			return static_cast<char>(value < 10 ? ('0' + value) : ('A' + (value - 10)));
		}

		int hex_value(char character)
		{
			if (character >= '0' && character <= '9') return character - '0';
			if (character >= 'a' && character <= 'f') return 10 + (character - 'a');
			if (character >= 'A' && character <= 'F') return 10 + (character - 'A');
			return -1;
		}

		std::string channel_hex(float channel)
		{
			const int value = std::clamp(static_cast<int>(std::lround(channel * 255.f)), 0, 255);
			return std::string{ hex_digit(value >> 4), hex_digit(value & 0xF) };
		}
	}

	c_color_setting::c_color_setting(std::string name, float r, float g, float b, float a)
		: c_setting(std::move(name)), m_r(r), m_g(g), m_b(b), m_a(a)
	{
		this->refresh_hsv_from_rgb();
	}

	c_color_setting::~c_color_setting()
	{
		/* a drawer torn down mid-edit must not leave the shell typing into freed
		   memory — this is the whole reason the focus is a raw pointer with an
		   owner rather than a bare flag on each widget */
		if (s_hex_focus == this)
			s_hex_focus = nullptr;

		colour_popup::forget(this);
	}

	void c_color_setting::clear_hex_focus()
	{
		if (s_hex_focus != nullptr)
			s_hex_focus->submit_hex();

		s_hex_focus = nullptr;
	}

	void c_color_setting::refresh_hsv_from_rgb()
	{
		float h, s, v;
		rgb_to_hsv(this->m_r, this->m_g, this->m_b, h, s, v);
		/* Preserve hue when saturation collapses to zero — the rgb round-trip
		   would otherwise reset the wheel to 0 (red) every time the user
		   drags value down to grey. */
		if (s > 0.f) this->m_hue = h;
		this->m_sat = s;
		this->m_val = v;
	}

	void c_color_setting::refresh_rgb_from_hsv()
	{
		hsv_to_rgb(this->m_hue, this->m_sat, this->m_val, this->m_r, this->m_g, this->m_b);
	}

	void c_color_setting::set_rgba(float r, float g, float b, float a)
	{
		this->m_r = std::clamp(r, 0.f, 1.f);
		this->m_g = std::clamp(g, 0.f, 1.f);
		this->m_b = std::clamp(b, 0.f, 1.f);
		this->m_a = std::clamp(a, 0.f, 1.f);
		this->refresh_hsv_from_rgb();

		/* @adopted, not dragged to.

		   The handles ease towards their values, which is right when the value is
		   being changed BY one of them — the others should travel rather than jump.
		   It is wrong when the value arrives from outside: a reset that takes a
		   quarter of a second to slide the square, three rails and the hex field
		   into place reads as the picker deciding for itself, not as the reset
		   having happened. Dropping the ready flag makes the next update land them
		   all where they now belong. */
		this->m_visual_ready = false;
		this->m_hex_buffer = this->hex();
	}

	std::string c_color_setting::hex() const
	{
		return channel_hex(this->m_r) + channel_hex(this->m_g) + channel_hex(this->m_b);
	}

	bool c_color_setting::apply_hex(const std::string& text)
	{
		int digits[6]{};
		std::size_t count = 0;

		for (const char character : text)
		{
			const int value = hex_value(character);
			if (value < 0)
				continue;

			if (count >= 6)
				return false;

			digits[count++] = value;
		}

		float channels[3]{};

		if (count == 3)
		{
			/* #RGB is #RRGGBB with every digit doubled, so F becomes FF and not F0
			   — the shorthand has to land on the same colour the long form would,
			   or typing "fff" would give a mid grey instead of white. */
			for (int index = 0; index < 3; ++index)
				channels[index] = static_cast<float>(digits[index] * 17) / 255.f;
		}
		else if (count == 6)
		{
			for (int index = 0; index < 3; ++index)
				channels[index] = static_cast<float>(digits[index * 2] * 16 + digits[index * 2 + 1]) / 255.f;
		}
		else
		{
			return false;
		}

		this->m_r = channels[0];
		this->m_g = channels[1];
		this->m_b = channels[2];
		this->refresh_hsv_from_rgb();

		return true;
	}

	void c_color_setting::begin_hex_edit()
	{
		if (s_hex_focus != nullptr && s_hex_focus != this)
			s_hex_focus->submit_hex();

		s_hex_focus = this;
		this->m_hex_buffer = this->hex();
	}

	void c_color_setting::on_character(char character)
	{
		if (s_hex_focus != this)
			return;

		if (character == '#')
			return;

		if (hex_value(character) < 0 || this->m_hex_buffer.size() >= 6)
			return;

		this->m_hex_buffer.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));

		/* Applied the moment the buffer is a whole colour, so the square, the
		   rails and the swatch all follow the typing. apply_hex refuses anything
		   partial, which is what stops "1" being read as #010101 on the way to
		   "1A2B3C". */
		this->apply_hex(this->m_hex_buffer);
	}

	void c_color_setting::on_backspace()
	{
		if (s_hex_focus != this || this->m_hex_buffer.empty())
			return;

		this->m_hex_buffer.pop_back();
		this->apply_hex(this->m_hex_buffer);
	}

	bool c_color_setting::paste_clipboard()
	{
		const char* clipboard = ImGui::GetClipboardText();
		if (clipboard == nullptr)
			return false;

		std::string text(clipboard);

		/* trim, then drop one optional hash */
		const auto first = text.find_first_not_of(" \t\r\n");
		const auto last = text.find_last_not_of(" \t\r\n");

		if (first == std::string::npos)
			return false;

		text = text.substr(first, last - first + 1);

		if (!text.empty() && text.front() == '#')
			text.erase(text.begin());

		if (text.size() != 3 && text.size() != 6)
			return false;

		for (const char character : text)
		{
			if (hex_value(character) < 0)
				return false;
		}

		std::transform(text.begin(), text.end(), text.begin(),
			[](unsigned char character) { return static_cast<char>(std::toupper(character)); });

		if (!this->apply_hex(text))
			return false;

		this->m_hex_buffer = this->hex();
		this->m_copy_pulse.kick();

		return true;
	}

	void c_color_setting::copy_clipboard()
	{
		/* the value people actually want on the clipboard is the one that pastes
		   straight back into any other tool, so it carries its hash */
		const std::string payload = "#" + this->hex();
		ImGui::SetClipboardText(payload.c_str());

		this->m_copied_hold = 1.15f;
		this->m_copy_pulse.kick();
	}

	void c_color_setting::submit_hex()
	{
		/* an incomplete buffer is discarded rather than half applied: the widget
		   still holds whatever colour it had before the edit began */
		this->apply_hex(this->m_hex_buffer);
		this->m_hex_buffer = this->hex();
	}

	void c_color_setting::update(float delta)
	{
		/* once per frame, whoever asks first — see m_anim_frame */
		const int frame = ImGui::GetFrameCount();
		if (this->m_anim_frame == frame)
			return;

		this->m_anim_frame = frame;

		if (!this->m_visual_ready)
		{
			this->m_visual_ready = true;
			this->m_visual_hue = this->m_hue;
			this->m_visual_sat = this->m_sat;
			this->m_visual_val = this->m_val;
			this->m_visual_alpha = this->m_a;
			return;
		}

		/* @why every handle is eased:
			only one control is ever being dragged, but a drag on any of them moves
			the others' markers — sliding the hue rail sweeps the square's colour,
			typing a hex moves all four at once. Easing every handle makes those
			secondary moves travel instead of teleporting, and it costs the dragged
			handle nothing: that one is already under the cursor.

			The hue wraps, so it is eased the SHORT way round the wheel; lerping
			0.98 -> 0.02 numerically would race the marker back across the whole
			spectrum. */
		float hue_target = this->m_hue;

		if (std::fabs(hue_target - this->m_visual_hue) > 0.5f)
			hue_target += (this->m_visual_hue > hue_target) ? 1.f : -1.f;

		this->m_visual_hue = anim::lerp(this->m_visual_hue, hue_target, 22.f, delta);
		this->m_visual_hue -= std::floor(this->m_visual_hue);

		this->m_visual_sat = anim::lerp(this->m_visual_sat, this->m_sat, 22.f, delta);
		this->m_visual_val = anim::lerp(this->m_visual_val, this->m_val, 22.f, delta);
		this->m_visual_alpha = anim::lerp(this->m_visual_alpha, this->m_a, 22.f, delta);

		/* the "copied" tick holds for a beat and then eases back to the sheets */
		if (this->m_copied_hold > 0.f)
			this->m_copied_hold = std::max(0.f, this->m_copied_hold - delta);

		this->m_copied = anim::lerp(this->m_copied, this->m_copied_hold > 0.f ? 1.f : 0.f, 12.f, delta);
	}

	void c_color_setting::render(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		/* @the compact row.

		   All this has to do is name the colour, show it, and be the handle that
		   opens the editor. Everything else moved into the window — which is why a
		   drawer full of colours is now a list instead of a wall. */
		constexpr float row_h = 34.f;

		const bool hovered = input.hovered(x, y, w, row_h);
		const bool editing = colour_popup::open_for(this);

		this->m_row_hover = anim::lerp(this->m_row_hover, hovered ? 1.f : 0.f, 16.f, input.m_delta);

		if (hovered && input.m_left_clicked)
			colour_popup::open(this, this->m_name);

		const float lit = std::max(this->m_row_hover, editing ? 1.f : 0.f);

		if (lit > 0.01f)
		{
			draw::rounded_rect(x, y, w, row_h, 8.f,
				hue::c_color(20, 20, 24).modulate(lit * alpha));
		}

		const float centre_y = y + row_h * 0.5f;

		draw_text(this->label(), x + 11.f, centre_y, font_face::regular, 12.f,
			theme::text_secondary.lerp(theme::text_white, lit).modulate(alpha),
			align_left | align_middle);

		const hue::c_color current(
			static_cast<int>(this->m_r * 255.f),
			static_cast<int>(this->m_g * 255.f),
			static_cast<int>(this->m_b * 255.f),
			255);

		{
			constexpr float swatch_w = 36.f;
			constexpr float swatch_h = 18.f;

			const float swatch_x = x + w - swatch_w - 11.f;
			const float swatch_y = centre_y - swatch_h * 0.5f;

			if (lit > 0.01f)
			{
				draw::shadow(swatch_x, swatch_y, swatch_w, swatch_h, 5.f, 9.f * lit, 0.f, 1.f,
					current.alpha(static_cast<int>(90.f * lit * alpha)));
			}

			draw::rounded_rect(swatch_x, swatch_y, swatch_w, swatch_h, 5.f,
				current.alpha(static_cast<int>(255.f * this->m_a * alpha)));
			draw::border(swatch_x, swatch_y, swatch_w, swatch_h, 5.f, 1.f,
				hue::c_color(40, 40, 46).modulate(alpha));
		}

		/* No chevron. It was there to say the row opens something, which the row
		   already says by lighting up and by being a swatch — and four of them
		   stacked down the panel read as a column of clutter rather than as four
		   affordances. */
	}

	void c_color_setting::render_editor(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		const bool row_hovered = input.hovered(x, y, w, editor_height - 4.f);
		this->m_hover = anim::lerp(this->m_hover, row_hovered ? 1.f : 0.f, 16.f, input.m_delta);

		const hue::c_color current(
			static_cast<int>(this->m_r * 255.f),
			static_cast<int>(this->m_g * 255.f),
			static_cast<int>(this->m_b * 255.f),
			255);

		/* No label and no swatch up here any more: the window's own title bar
		   names the colour, and the square below shows it at a hundred times the
		   size a chip would. */

		/* ---------------------------------------------------------------- */
		/* saturation / value square                                         */
		/* ---------------------------------------------------------------- */
		const float sv_x = x;
		const float sv_y = y + k_picker.sv_y;
		const float sv_w = w;
		const float sv_h = k_picker.sv_h;

		{
			float hr, hg, hb;
			hsv_to_rgb(this->m_visual_hue, 1.f, 1.f, hr, hg, hb);

			const hue::c_color pure(
				static_cast<int>(hr * 255.f), static_cast<int>(hg * 255.f), static_cast<int>(hb * 255.f), 255);

			/* Two rounded passes rather than one four-corner fill: ImGui's
			   multi-colour rect cannot round its corners, and the square shares a
			   radius family with every other card in the drawer. White -> hue
			   across, then transparent -> black down, composites to exactly the
			   same square. */
			draw::gradient_rect(sv_x, sv_y, sv_w, sv_h, 7.f,
				theme::text_white.modulate(alpha), pure.modulate(alpha));

			draw::gradient_rect(sv_x, sv_y, sv_w, sv_h, 7.f,
				hue::c_color(0, 0, 0, 0), hue::c_color(0, 0, 0, static_cast<int>(255.f * alpha)), true);

			draw::border(sv_x, sv_y, sv_w, sv_h, 7.f, 1.f, hue::c_color(34, 34, 40).modulate(alpha));
		}

		/* ---------------------------------------------------------------- */
		/* hue + alpha rails                                                 */
		/* ---------------------------------------------------------------- */
		const float rail_x = x + k_rail_knob;
		const float rail_w = std::max(1.f, w - k_rail_knob * 2.f);

		const float hue_y = y + k_picker.hue_y;
		const float alpha_y = y + k_picker.alpha_y;

		draw::spectrum_capsule(x, hue_y, w, k_picker.hue_h, alpha);
		draw::border(x, hue_y, w, k_picker.hue_h, k_picker.hue_h * 0.5f, 1.f,
			hue::c_color(30, 30, 34).modulate(alpha));

		{
			/* the alpha rail carries the colour itself, fading out to nothing on
			   the left, so it reads as "how much of this" rather than as a second
			   brightness control */
			draw::rounded_rect(x, alpha_y, w, k_picker.alpha_h, k_picker.alpha_h * 0.5f,
				hue::c_color(20, 20, 23).modulate(alpha));

			draw::gradient_rect(x, alpha_y, w, k_picker.alpha_h, k_picker.alpha_h * 0.5f,
				current.alpha(0), current.alpha(static_cast<int>(255.f * alpha)));

			draw::border(x, alpha_y, w, k_picker.alpha_h, k_picker.alpha_h * 0.5f, 1.f,
				hue::c_color(30, 30, 34).modulate(alpha));
		}

		/* ---------------------------------------------------------------- */
		/* input dispatch                                                    */
		/* ---------------------------------------------------------------- */
		/* Only one region can be dragged at a time, otherwise the picker flips
		   between hue and SV when the cursor grazes the boundary. Once a drag
		   starts the region locks until the button is released. */
		if (!input.m_left_down) this->m_drag = drag_target::none;

		/* @a fresh press, never a held button.

		   This used to start a drag on m_left_down, and that is the bug that made
		   recolouring the client feel haunted: the editor opens UNDER the cursor,
		   on the frame after the click that opened it, with the button often still
		   held. A held-button test grabs that as a drag the instant the square
		   arrives beneath the pointer, and the colour snaps to whatever point of
		   the square happened to land there — frequently the top-right corner,
		   which is full-saturation red. The user never touched the square.

		   Requiring the press to START here is the same discipline the window drag
		   is written against, and it costs nothing: click-to-jump still works,
		   because a click is also a press. */
		if (input.m_left_clicked && this->m_drag == drag_target::none)
		{
			if (input.hovered(sv_x, sv_y, sv_w, sv_h))
				this->m_drag = drag_target::sv;
			else if (input.hovered(x, hue_y - 4.f, w, k_picker.hue_h + 8.f))
				this->m_drag = drag_target::hue;
			else if (input.hovered(x, alpha_y - 4.f, w, k_picker.alpha_h + 8.f))
				this->m_drag = drag_target::alpha;
		}

		auto clamp01 = [](float value) { return std::clamp(value, 0.f, 1.f); };

		/* the rails resolve against the ball's travel, not the rail's own edges,
		   so the ball lands under the cursor at both extremes */
		auto rail_percent = [&]() { return clamp01((input.m_cursor.x - rail_x) / rail_w); };

		if (this->m_drag == drag_target::hue)
		{
			this->m_hue = rail_percent();
			this->refresh_rgb_from_hsv();
		}
		else if (this->m_drag == drag_target::sv)
		{
			this->m_sat = clamp01((input.m_cursor.x - sv_x) / std::max(1.f, sv_w));
			this->m_val = 1.f - clamp01((input.m_cursor.y - sv_y) / std::max(1.f, sv_h));
			this->refresh_rgb_from_hsv();
		}
		else if (this->m_drag == drag_target::alpha)
		{
			this->m_a = rail_percent();
		}

		/* a drag anywhere in the picker means the hex field is no longer being
		   typed into, and its buffer must adopt whatever was just picked */
		if (this->m_drag != drag_target::none && s_hex_focus == this)
			clear_hex_focus();

		/* ---------------------------------------------------------------- */
		/* handles, drawn from the EASED values                              */
		/* ---------------------------------------------------------------- */
		{
			const float engaged = std::max(this->m_drag == drag_target::sv ? 1.f : 0.f, this->m_hover);

			const float dot_x = sv_x + sv_w * this->m_visual_sat;
			const float dot_y = sv_y + sv_h * (1.f - this->m_visual_val);

			const float ring = 6.2f + 0.8f * engaged;

			draw::glow(dot_x, dot_y, ring, 10.f, current.alpha(static_cast<int>(120.f * alpha)));

			draw::circle(dot_x, dot_y, ring + 1.4f, hue::c_color(0, 0, 0, static_cast<int>(90.f * alpha)));
			draw::circle(dot_x, dot_y, ring, theme::text_white.modulate(alpha));
			draw::circle(dot_x, dot_y, ring - 2.2f, current.modulate(alpha));
		}

		{
			const float engaged = std::max(this->m_drag == drag_target::hue ? 1.f : 0.f, this->m_hover);

			slider_knob(rail_x + rail_w * this->m_visual_hue, hue_y + k_picker.hue_h * 0.5f,
				k_rail_knob, engaged * 0.5f, alpha);
		}

		{
			const float engaged = std::max(this->m_drag == drag_target::alpha ? 1.f : 0.f, this->m_hover);

			slider_knob(rail_x + rail_w * this->m_visual_alpha, alpha_y + k_picker.alpha_h * 0.5f,
				k_rail_knob, engaged * 0.5f, alpha);
		}

		/* ---------------------------------------------------------------- */
		/* hex field                                                         */
		/* ---------------------------------------------------------------- */
		this->render_hex_field(x, y + k_picker.hex_y, w, k_picker.hex_h, input, alpha);
	}

	void c_color_setting::render_hex_field(float x, float y, float w, float h,
		const frame_input_t& input, float alpha)
	{
		const bool editing = (s_hex_focus == this);

		constexpr float copy_size = 26.f;

		const float copy_x = x + w - copy_size - 3.f;
		const float copy_y = y + (h - copy_size) * 0.5f;

		const bool copy_hovered = input.hovered(copy_x, copy_y, copy_size, copy_size);
		const bool field_hovered = input.hovered(x, y, w, h) && !copy_hovered;

		this->m_hex_hover = anim::lerp(this->m_hex_hover, field_hovered ? 1.f : 0.f, 16.f, input.m_delta);
		this->m_copy_hover = anim::lerp(this->m_copy_hover, copy_hovered ? 1.f : 0.f, 18.f, input.m_delta);
		this->m_hex_focus_anim = anim::lerp(this->m_hex_focus_anim, editing ? 1.f : 0.f, 15.f, input.m_delta);

		if (input.m_left_clicked)
		{
			if (copy_hovered)
			{
				this->copy_clipboard();
			}
			else if (field_hovered)
			{
				if (!editing)
					this->begin_hex_edit();
			}
			else if (editing)
			{
				// clicking anywhere else commits and gives the caret up
				clear_hex_focus();
			}
		}

		const float focus = anim::saturate(this->m_hex_focus_anim);

		/* --- the field ---------------------------------------------------- */
		if (focus > 0.02f)
		{
			draw::shadow(x, y, w, h, 8.f, 12.f * focus, 0.f, 1.f,
				theme::accent_violet.alpha(static_cast<int>(70.f * focus * alpha)));
		}

		draw::rounded_rect(x, y, w, h, 8.f,
			hue::c_color(17, 17, 19).lerp(hue::c_color(23, 23, 26), this->m_hex_hover).modulate(alpha));

		draw::border(x, y, w, h, 8.f, 1.f + 0.35f * focus,
			hue::c_color(32, 32, 36).lerp(theme::accent_violet, focus).modulate(alpha));

		const float center_y = y + h * 0.5f;

		/* --- the hash, then the digits ------------------------------------ */
		constexpr float font_size = 12.5f;

		const float hash_x = x + 11.f;

		draw_text("#", hash_x, center_y, font_face::bold, font_size,
			theme::text_muted.lerp(theme::accent_violet, focus).modulate(alpha),
			align_left | align_middle);

		const float value_x = hash_x + text_width("#", font_face::bold, font_size) + 4.f;

		/* while editing the buffer is the truth, so a half typed colour stays on
		   screen instead of being overwritten by the colour it has not made yet */
		const std::string shown = editing ? this->m_hex_buffer : this->hex();

		draw_text(shown, value_x, center_y, font_face::bold, font_size,
			theme::text_white.modulate(alpha * (editing ? 1.f : 0.86f)),
			align_left | align_middle);

		/* the caret glides to its target rather than jumping, and never blinks —
		   the same contract the search field is written against */
		const float caret_target = value_x + text_width(shown, font_face::bold, font_size) + 1.f;

		if (this->m_hex_caret_x < 0.f)
			this->m_hex_caret_x = caret_target;
		else
			this->m_hex_caret_x = anim::lerp(this->m_hex_caret_x, caret_target, 22.f, input.m_delta);

		if (focus > 0.02f)
		{
			draw::rounded_rect(this->m_hex_caret_x, center_y - 7.f, 1.5f, 14.f, 0.75f,
				theme::accent_violet.modulate(alpha * focus));
		}

		/* --- copy button --------------------------------------------------- */
		{
			const float done = anim::saturate(this->m_copied);

			if (this->m_copy_hover > 0.01f || done > 0.01f)
			{
				draw::rounded_rect(copy_x, copy_y, copy_size, copy_size, 7.f,
					hue::c_color(30, 30, 34)
						.lerp(theme::accent_violet_deep, done)
						.modulate((0.55f * this->m_copy_hover + 0.75f * done) * alpha));
			}

			if (const float pulse = this->m_copy_pulse.update(input.m_delta); pulse > 0.01f)
			{
				draw::shadow(copy_x, copy_y, copy_size, copy_size, 7.f, 6.f + 16.f * (1.f - pulse), 0.f, 0.f,
					theme::accent_violet.alpha(static_cast<int>(150.f * pulse * alpha)));
			}

			/* The two glyphs crossfade in place rather than swapping, so the
			   confirmation reads as this button acknowledging the click and not as
			   a different button appearing where the old one was. */
			constexpr float glyph = 12.f;

			const float glyph_x = copy_x + (copy_size - glyph) * 0.5f;
			const float glyph_y = copy_y + (copy_size - glyph) * 0.5f;

			const hue::c_color tint = theme::text_muted.lerp(theme::text_white,
				std::max(this->m_copy_hover, done));

			if (done < 0.99f)
				draw::copy_glyph(glyph_x, glyph_y, glyph, 1.35f, tint.modulate(alpha * (1.f - done)));

			if (done > 0.01f)
				draw::check_glyph(glyph_x, glyph_y, glyph, 1.7f, theme::text_white.modulate(alpha * done));
		}
	}

	void c_color_setting::propagate_if_changed(const std::string& module_name)
	{
		if (std::isnan(this->m_pushed_r) == false
			&& this->m_r == this->m_pushed_r
			&& this->m_g == this->m_pushed_g
			&& this->m_b == this->m_pushed_b
			&& this->m_a == this->m_pushed_a)
			return;

		this->m_pushed_r = this->m_r;
		this->m_pushed_g = this->m_g;
		this->m_pushed_b = this->m_b;
		this->m_pushed_a = this->m_a;
		dt::vgui::bridge::applyColor(module_name, this->m_name,
			this->m_r, this->m_g, this->m_b, this->m_a);
	}

	/* ------------------------------------------------------------------ */
	/* boolean                                                             */
	/* ------------------------------------------------------------------ */

	c_boolean_setting::c_boolean_setting(std::string name, bool value)
		: c_setting(std::move(name)), m_enabled(value)
	{
		this->m_toggle = value ? 1.f : 0.f;
	}

	void c_boolean_setting::update(float delta)
	{
		this->m_toggle = anim::lerp(this->m_toggle, this->m_enabled ? 1.f : 0.f, 16.f, delta);
	}

	void c_boolean_setting::render(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		constexpr float row_height = 28.f;
		const float center_y = y + row_height * 0.5f;

		const bool hovered = input.hovered(x, y, w, row_height);
		this->m_hover = anim::lerp(this->m_hover, hovered ? 1.f : 0.f, 16.f, input.m_delta);

		if (hovered && input.m_left_clicked)
		{
			this->m_enabled = !this->m_enabled;
			this->m_flip.kick();
		}

		/* the whole row is clickable, so the whole row lights up */
		if (this->m_hover > 0.01f)
		{
			draw::rounded_rect(x - 8.f, y, w + 16.f, row_height, 7.f,
				hue::c_color(255, 255, 255, static_cast<int>(9.f * this->m_hover * alpha)));
		}

		const hue::c_color label = theme::text_secondary
			.lerp(theme::text_white, std::max(this->m_toggle, this->m_hover * 0.5f));

		draw_text(this->label(), x, center_y, font_face::regular, 12.f,
			label.modulate(alpha), align_left | align_middle);

		/* --- the toggle ------------------------------------------------
		   an outlined box that fills from its own centre with the accent as it comes on, and the
		   outline fades out underneath. the tick is dark, because it sits on the accent. */
		constexpr float box_size = 18.f;
		constexpr float box_radius = 5.f;

		/* @in the same column as everything else on its right.

		   Inset by the same 11px the colour swatches use, so a panel that mixes
		   switches and swatches has ONE right-hand edge rather than two. A control
		   floating at some other x reads as unaligned long before anyone works out
		   what it is unaligned with. */
		const float box_x = x + w - box_size - 11.f;
		const float box_y = center_y - box_size * 0.5f;

		if (!this->m_knob_ready)
		{
			this->m_knob.snap(this->m_enabled ? 1.f : 0.f);
			this->m_knob_ready = true;
		}

		this->m_knob.update(this->m_enabled ? 1.f : 0.f, 320.f, 26.f, input.m_delta);

		const float grow = anim::saturate(this->m_knob.m_value);
		const float fill = box_size * (0.5f + 0.5f * grow);
		const float inset = (box_size - fill) * 0.5f;

		if (this->m_toggle > 0.05f)
		{
			draw::shadow(box_x, box_y, box_size, box_size, box_radius, 11.f * this->m_toggle, 0.f, 1.f,
				theme::accent_violet.alpha(static_cast<int>(95.f * this->m_toggle * alpha)));
		}

		/* @an unchecked box has to be findable, and this one was not.

		   It was drawn near-black (9,9,10) with an (8,8,9) outline -- ONE level
		   apart, on a card only a little lighter again. The reasoning was that an
		   unchecked box should recede into the surface and that the hover lift
		   would say it was pressable. But you cannot hover what you cannot find,
		   so what it produced was a column of labels with an invisible target
		   somewhere off to their right.

		   It reads as a control at rest now: a grey well, and a hairline
		   noticeably lighter again so the edge is an edge. Both still step up on
		   hover, so the affordance the old version leaned on is not lost -- it is
		   simply no longer the only thing there. */
		const hue::c_color outline = theme::border.lerp(theme::accent_violet, 0.35f)
			.lerp(theme::accent_violet, this->m_hover);

		/* the well the box sits in, so the edge has something to be an edge OF */
		draw::rounded_rect(box_x, box_y, box_size, box_size, box_radius,
			theme::bg_element
				.lerp(theme::bg_card, this->m_hover)
				.modulate(alpha * (1.f - this->m_toggle)));

		draw::border(box_x, box_y, box_size, box_size, box_radius, 1.f,
			outline.modulate(alpha * (1.f - this->m_toggle)));

		draw::rounded_rect(box_x + inset, box_y + inset, fill, fill,
			box_radius * (0.5f + 0.5f * grow),
			theme::accent_violet.modulate(alpha * this->m_toggle));

		if (const float flip = this->m_flip.update(input.m_delta); flip > 0.01f)
		{
			const float spread = 4.f + 18.f * (1.f - flip);

			draw::shadow(box_x, box_y, box_size, box_size, box_radius, spread, 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>(190.f * flip * alpha)));
		}

		if (this->m_toggle > 0.05f)
		{
			const float tick = 9.f;
			const float tick_inset = (box_size - tick) * 0.5f;

			g_render->check_mark(
				static_cast<int>(std::lround(box_x + tick_inset)),
				static_cast<int>(std::lround(box_y + tick_inset)),
				tick,
				hue::c_color(10, 8, 16).modulate(this->m_toggle * alpha)
			);
		}
	}

	void c_boolean_setting::propagate_if_changed(const std::string& module_name)
	{
		/* Sentinel -1 forces the first push after construction; afterwards
		   only actual flips propagate to the real dt::BoolSetting. */
		const int now = this->m_enabled ? 1 : 0;
		if (now == this->m_last_pushed) return;
		this->m_last_pushed = now;
		dt::vgui::bridge::applyBool(module_name, this->m_name, this->m_enabled);
	}

	/* ------------------------------------------------------------------ */
	/* slot map                                                            */
	/* ------------------------------------------------------------------ */

	c_slot_map_setting::c_slot_map_setting(std::string name, std::vector<std::string> options)
		: c_setting(std::move(name)), m_options(std::move(options))
	{
	}

	void c_slot_map_setting::update(float)
	{
		/* hover eases inside render, where the per-square rects are known */
	}

	/* ---- vector item art -------------------------------------------------
	   Drawn rather than blitted: Minecraft's item sprites live in a baked
	   model atlas owned by the game's own renderer, and reaching into it would
	   mean resolving the atlas texture and per-sprite UVs through several more
	   mappings for something the overlay can draw exactly. Proportions and
	   colours are taken from the real items, so a square reads as the item and
	   not as a label.
	   Everything is authored on a 16x16 grid and scaled, the same grid the
	   textures themselves use.                                              */

	namespace
	{
		const hue::c_color potion_glass{ 214, 228, 236 };
		const hue::c_color potion_cork { 170, 133,  92 };
		const hue::c_color totem_gold  { 232, 197,  90 };
		const hue::c_color totem_dark  { 176, 141,  58 };
		const hue::c_color totem_gem   {  69, 201,  58 };
	}

	void c_slot_map_setting::draw_potion(float cx, float cy, float s, hue::c_color liquid, float alpha)
	{
		const float u  = s / 16.f;               /* one texel of the source grid */
		const float ox = cx - s * 0.5f;
		const float oy = cy - s * 0.5f;
		auto gx = [&](float g) { return ox + g * u; };
		auto gy = [&](float g) { return oy + g * u; };

		/* cork, neck, round body, then the liquid sitting inside the glass */
		draw::rounded_rect(gx(6.0f), gy(1.4f), u * 4.0f, u * 2.2f, u * 0.8f, potion_cork.modulate(alpha));
		draw::rounded_rect(gx(6.5f), gy(3.0f), u * 3.0f, u * 4.6f, u * 1.0f,
			potion_glass.alpha(215).modulate(alpha));
		draw::circle(gx(8.0f), gy(10.4f), u * 4.3f, potion_glass.alpha(215).modulate(alpha));
		draw::circle(gx(8.0f), gy(10.7f), u * 3.3f, liquid.modulate(alpha));

		/* the glint that makes it read as glass rather than a filled circle */
		draw::circle(gx(6.3f), gy(8.9f), u * 0.85f, hue::c_color(255, 255, 255, 140).modulate(alpha));
	}

	void c_slot_map_setting::draw_totem(float cx, float cy, float s, float alpha)
	{
		const float u  = s / 16.f;
		const float ox = cx - s * 0.5f;
		const float oy = cy - s * 0.5f;
		auto gx = [&](float g) { return ox + g * u; };
		auto gy = [&](float g) { return oy + g * u; };

		/* arms first so the body and head overlap them */
		draw::rounded_rect(gx(2.6f),  gy(7.0f),  u * 2.8f, u * 3.2f, u * 1.0f, totem_dark.modulate(alpha));
		draw::rounded_rect(gx(10.6f), gy(7.0f),  u * 2.8f, u * 3.2f, u * 1.0f, totem_dark.modulate(alpha));
		draw::rounded_rect(gx(5.2f),  gy(5.0f),  u * 5.6f, u * 9.4f, u * 1.6f, totem_gold.modulate(alpha));
		draw::rounded_rect(gx(4.4f),  gy(2.4f),  u * 7.2f, u * 5.2f, u * 2.0f, totem_gold.modulate(alpha));
		draw::rounded_rect(gx(5.2f),  gy(12.2f), u * 5.6f, u * 2.2f, u * 0.8f, totem_dark.modulate(alpha));
		draw::circle       (gx(8.0f), gy(4.9f),  u * 1.5f, totem_gem.modulate(alpha));
	}

	/* Dispatched on the option's LABEL, not its index, so the art follows the
	   item if the module ever reorders its list. */
	void c_slot_map_setting::draw_item(int option, float cx, float cy, float s, float alpha) const
	{
		if (option < 0 || option >= static_cast<int>(this->m_options.size())) return;
		const std::string& label = this->m_options[static_cast<std::size_t>(option)];

		if (label.find("Totem") != std::string::npos) { draw_totem(cx, cy, s, alpha); return; }

		/* Vanilla's own potion colours, except strength: its real #932423 is a
		   dark red that is indistinguishable from healing at this size, so it
		   is pushed to orange for the sake of telling the squares apart. */
		hue::c_color liquid(190, 190, 190);
		if      (label.find("Heal")  != std::string::npos) liquid = hue::c_color(248,  36,  35);
		else if (label.find("Speed") != std::string::npos) liquid = hue::c_color(124, 175, 198);
		else if (label.find("Stren") != std::string::npos) liquid = hue::c_color(232, 128,  42);

		draw_potion(cx, cy, s, liquid, alpha);
	}

	void c_slot_map_setting::render(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		draw_text(this->label(), x, y + 8.f, font_face::regular, 12.f,
			theme::text_secondary.modulate(alpha), align_left | align_middle);

		/* Squares fill the row width with even gaps, capped so a wide panel
		   does not blow them up out of proportion; the strip then centres in
		   whatever width is left. */
		constexpr float gap = 4.f;
		const float size = std::min((w - gap * static_cast<float>(SLOTS - 1))
			/ static_cast<float>(SLOTS), 30.f);
		const float strip_w = size * static_cast<float>(SLOTS) + gap * static_cast<float>(SLOTS - 1);
		const float x0  = x + (w - strip_w) * 0.5f;
		const float top = y + 22.f;

		const std::uint32_t allowed = this->m_allowed ? this->m_allowed() : ~0u;

		for (int i = 0; i < SLOTS; ++i)
		{
			const float sx = x0 + static_cast<float>(i) * (size + gap);

			const bool hovered = input.hovered(sx, top, size, size);
			this->m_hover[i] = anim::lerp(this->m_hover[i], hovered ? 1.f : 0.f, 16.f, input.m_delta);

			if (hovered && input.m_left_clicked && allowed != 0u)
				this->m_pending_cycle = i;

			const int value = this->m_slot ? this->m_slot(i) : -1;
			const bool filled = value >= 0 && value < static_cast<int>(this->m_options.size());

			/* Fire the pop on the frame the value actually changed. The first
			   frame only seeds, so a drawer opening on saved slots shows them
			   already settled instead of firing nine animations at once. */
			if (!this->m_seeded)
			{
				this->m_last[i] = value;
				this->m_fill[i] = filled ? 1.f : 0.f;
			}
			else if (value != this->m_last[i])
			{
				this->m_last[i] = value;
				this->m_pop[i]  = 1.f;
			}

			/* Held separately from m_last: a slot going empty still has to draw
			   its old item for as long as it takes to shrink away. */
			if (filled) this->m_art[i] = value;

			this->m_pop[i]  = anim::lerp(this->m_pop[i],  0.f, 9.f,  input.m_delta);
			this->m_fill[i] = anim::lerp(this->m_fill[i], filled ? 1.f : 0.f, 14.f, input.m_delta);

			/* The square itself swells a little on assignment. */
			const float grow = this->m_pop[i] * 1.8f;
			const float bx = sx - grow * 0.5f, by = top - grow * 0.5f, bs = size + grow;

			const hue::c_color body = theme::accent_violet
				.alpha(static_cast<int>((52.f + 26.f * this->m_hover[i]) * this->m_fill[i]))
				.lerp(hue::c_color(255, 255, 255,
					static_cast<int>(7.f + 9.f * this->m_hover[i])), 1.f - this->m_fill[i]);

			draw::rounded_rect(bx, by, bs, bs, 5.f, body.modulate(alpha));

			const hue::c_color outline = hue::c_color(46, 46, 46)
				.lerp(hue::c_color(70, 70, 70), this->m_hover[i])
				.lerp(theme::accent_violet.alpha(static_cast<int>(150.f + 60.f * this->m_hover[i])),
					this->m_fill[i]);

			draw::border(bx, by, bs, bs, 5.f, 1.f, outline.modulate(alpha));

			/* A ring that expands out of the square and fades, so the eye is
			   told which of nine identical squares just took the click. */
			if (this->m_pop[i] > 0.02f)
			{
				const float spread = (1.f - this->m_pop[i]) * 5.f;
				draw::border(bx - spread, by - spread, bs + spread * 2.f, bs + spread * 2.f,
					5.f + spread, 1.2f,
					theme::accent_violet
						.alpha(static_cast<int>(180.f * this->m_pop[i] * this->m_pop[i]))
						.modulate(alpha));
			}

			if (this->m_fill[i] > 0.02f)
			{
				/* Scale carries both the entrance (m_fill) and the pop, so an
				   item grows in when assigned and shrinks away when cleared. */
				const float scale = this->m_fill[i] * (1.f + 0.45f * this->m_pop[i]);
				this->draw_item(this->m_art[i], sx + size * 0.5f, top + size * 0.5f,
					size * 0.70f * scale, alpha * this->m_fill[i]);
			}

			if (this->m_fill[i] < 0.98f)
			{
				/* The slot number, so an empty strip still reads as a hotbar. */
				draw_text(std::to_string(i + 1), sx + size * 0.5f, top + size * 0.5f,
					font_face::regular, 9.f,
					theme::text_secondary.modulate(alpha * 0.55f * (1.f - this->m_fill[i])),
					align_center | align_middle);
			}
		}

		this->m_seeded = true;
	}

	void c_slot_map_setting::propagate_if_changed(const std::string& module_name)
	{
		if (this->m_pending_cycle < 0) return;
		const int index = this->m_pending_cycle;
		this->m_pending_cycle = -1;
		dt::vgui::bridge::cycleSlot(module_name, this->m_name, index);
	}

	/* ------------------------------------------------------------------ */
	/* mode                                                                */
	/* ------------------------------------------------------------------ */

	c_mode_setting::c_mode_setting(std::string name, std::vector<std::string> modes, int index)
		: c_setting(std::move(name)), m_modes(std::move(modes)), m_index(index)
	{
		if (this->m_modes.empty())
			this->m_modes.push_back("None");

		this->m_index = std::clamp(this->m_index, 0, static_cast<int>(this->m_modes.size()) - 1);
		this->m_slide = static_cast<float>(this->m_index);
	}

	void c_mode_setting::update(float delta)
	{
		this->m_slide = anim::lerp(this->m_slide, static_cast<float>(this->m_index), 16.f, delta);
	}

	float c_mode_setting::height() const
	{
		if (!this->uses_dropdown())
			return 42.f;

		/* The open list is part of the row's height, so the drawer's own layout
		   makes room for it and everything below slides down with the spring
		   rather than being covered by it. */
		const float reveal = anim::saturate(this->m_expand.m_value);

		return 42.f + reveal * (static_cast<float>(this->m_modes.size()) * 26.f + 8.f);
	}

	void c_mode_setting::render(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		if (this->uses_dropdown())
			this->render_dropdown(x, y, w, input, alpha);
		else
			this->render_segmented(x, y, w, input, alpha);
	}

	void c_mode_setting::render_dropdown(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		constexpr float field_height = 25.f;
		constexpr float radius = 7.f;
		constexpr float row_height = 26.f;

		const float field_width = std::min(210.f, w * 0.62f);
		const float field_x = x + w - field_width;
		const float field_y = y + 4.f;
		const float center_y = field_y + field_height * 0.5f;

		draw_text(this->label(), x, center_y, font_face::regular, 12.f,
			theme::text_secondary.modulate(alpha), align_left | align_middle);

		const bool field_hovered = input.hovered(field_x, field_y, field_width, field_height);

		this->m_hover = anim::lerp(this->m_hover, field_hovered ? 1.f : 0.f, 16.f, input.m_delta);

		if (field_hovered && input.m_left_clicked)
		{
			this->m_open = !this->m_open;
			this->m_pick.kick();
		}

		this->m_expand.update(this->m_open ? 1.f : 0.f, 250.f, 26.f, input.m_delta);

		const float reveal = anim::saturate(this->m_expand.m_value);

		/* --- the field itself --------------------------------------------- */
		draw::rounded_rect(field_x, field_y, field_width, field_height, radius,
			theme::pane(theme::bg_element, 0.5f).modulate(alpha));

		draw::border(field_x, field_y, field_width, field_height, radius, 1.f,
			hue::c_color(26, 26, 26).modulate(alpha));

		if (this->m_hover > 0.01f)
		{
			draw::rounded_rect(field_x, field_y, field_width, field_height, radius,
				hue::c_color(255, 255, 255, static_cast<int>(10.f * this->m_hover * alpha)));
		}

		draw_text(this->selected(), field_x + 11.f, center_y, font_face::regular, 11.f,
			theme::text_white.modulate(alpha), align_left | align_middle);

		/* The same caret the profile panel uses, turned over by the same spring
		   that opens the list — one control, one motion. */
		draw::disclosure_arrow(field_x + field_width - 13.f, center_y, 8.f,
			1.f - 2.f * reveal, 1.f, theme::text_secondary.modulate(alpha));

		if (reveal <= 0.004f)
			return;

		if (this->m_option_hover.size() != this->m_modes.size())
			this->m_option_hover.assign(this->m_modes.size(), 0.f);

		/* --- the list ------------------------------------------------------ */
		const float list_height = static_cast<float>(this->m_modes.size()) * row_height + 8.f;
		const float list_y = field_y + field_height + 5.f;

		const float list_alpha = alpha * anim::saturate(reveal * 1.3f);

		draw::rounded_rect(field_x, list_y, field_width, list_height, 9.f,
			theme::pane(hue::c_color(17, 17, 20), 0.55f).modulate(list_alpha));

		draw::border(field_x, list_y, field_width, list_height, 9.f, 1.f,
			hue::c_color(32, 32, 38).modulate(list_alpha));

		/* Clipped to the height the row has actually grown to. Without this the
		   list is drawn at full size from the first frame and the rows below it
		   slide out from underneath a box that is already there — the animation
		   reads as the list pushing them, not as the list opening. */
		draw::push_clip(field_x - 2.f, list_y, field_width + 4.f,
			std::max(0.f, (y + this->height()) - list_y));

		for (std::size_t index = 0; index < this->m_modes.size(); ++index)
		{
			const float row_y = list_y + 4.f + static_cast<float>(index) * row_height;

			const bool selected = (this->m_index == static_cast<int>(index));
			/* @the hit test is clipped the same way the paint is.

			   push_clip only scissors pixels; it does not touch input. While the
			   list is still growing, the rows below the animated edge are drawn
			   nowhere and clickable everywhere — you would press a module row you
			   can see and select a font you cannot. */
			const bool in_view = (row_y + row_height) <= (y + this->height());

			const bool hovered = this->m_open && in_view
				&& input.hovered(field_x + 4.f, row_y, field_width - 8.f, row_height);

			this->m_option_hover[index] = anim::lerp(this->m_option_hover[index],
				hovered ? 1.f : 0.f, 18.f, input.m_delta);

			if (hovered && input.m_left_clicked)
			{
				this->m_index = static_cast<int>(index);
				this->m_open = false;
				this->m_pick.kick();
			}

			if (selected)
			{
				draw::rounded_rect(field_x + 4.f, row_y, field_width - 8.f, row_height, 7.f,
					theme::accent_violet_deep.alpha(static_cast<int>(190.f * list_alpha)));
			}
			else if (this->m_option_hover[index] > 0.01f)
			{
				draw::rounded_rect(field_x + 4.f, row_y, field_width - 8.f, row_height, 7.f,
					hue::c_color(30, 30, 36, static_cast<int>(255.f * this->m_option_hover[index] * list_alpha)));
			}

			draw_text(this->m_modes[index], field_x + 15.f, row_y + row_height * 0.5f,
				selected ? font_face::bold : font_face::regular, 11.f,
				(selected || this->m_option_hover[index] > 0.5f
					? theme::text_white : theme::text_secondary).modulate(list_alpha),
				align_left | align_middle);
		}

		draw::pop_clip();
	}

	bool c_mode_setting::uses_dropdown() const
	{
		if (this->m_dropdown >= 0)
			return this->m_dropdown == 1;

		if (this->m_modes.size() < 2)
			return false;

		/* Measured against the track at its WIDEST. The capsule is
		   min(210, w * 0.62), so anything that does not fit in 210/n cannot fit
		   in the real width either — and deciding on a constant keeps the answer
		   stable, which matters because height() is asked for this row's size
		   without being told how wide the drawer is. */
		const float option_width =
			k_track_width / static_cast<float>(this->m_modes.size());

		for (const std::string& mode : this->m_modes)
		{
			if (text_width(mode, font_face::regular, k_option_font)
				> option_width - k_option_padding)
			{
				this->m_dropdown = 1;
				return true;
			}
		}

		this->m_dropdown = 0;
		return false;
	}

	void c_mode_setting::render_segmented(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		/* segmented capsule pinned to the right edge, label centred on the same line */
		constexpr float segment_height = 25.f;
		constexpr float radius = 7.f;

		const float segment_width = std::min(k_track_width, w * 0.62f);
		const float segment_x = x + w - segment_width;
		const float segment_y = y + 4.f;
		const float center_y = segment_y + segment_height * 0.5f;

		draw_text(this->label(), x, center_y, font_face::regular, 12.f,
			theme::text_secondary.modulate(alpha), align_left | align_middle);

		draw::rounded_rect(segment_x, segment_y, segment_width, segment_height, radius, theme::bg_element.modulate(alpha));
		draw::border(segment_x, segment_y, segment_width, segment_height, radius, 1.f, hue::c_color(26, 26, 26).modulate(alpha));

		const float option_width = segment_width / static_cast<float>(this->m_modes.size());

		int hovered_option = -1;

		for (std::size_t index = 0; index < this->m_modes.size(); ++index)
		{
			const float option_x = segment_x + static_cast<float>(index) * option_width;

			if (!input.hovered(option_x, segment_y, option_width, segment_height))
				continue;

			hovered_option = static_cast<int>(index);

			if (input.m_left_clicked && this->m_index != static_cast<int>(index))
			{
				this->m_index = static_cast<int>(index);
				this->m_pick.kick();
			}
		}

		/* sliding pill highlight, with its own glow so the active mode reads at a glance */
		const float pill_x = segment_x + this->m_slide * option_width + 2.f;
		const float pill_y = segment_y + 2.f;
		const float pill_width = option_width - 4.f;
		const float pill_height = segment_height - 4.f;

		draw::shadow(pill_x, pill_y, pill_width, pill_height, radius - 2.f, 9.f, 0.f, 1.f,
			theme::accent_violet.alpha(static_cast<int>(80.f * alpha)));

		const float pick = this->m_pick.update(input.m_delta);

		draw::rounded_rect(pill_x, pill_y, pill_width, pill_height, radius - 2.f,
			theme::accent_violet_deep.lerp(theme::accent_violet, pick)
			.alpha(static_cast<int>(225.f * alpha)));

		if (pick > 0.01f)
		{
			draw::shadow(pill_x, pill_y, pill_width, pill_height, radius - 2.f,
				10.f + 14.f * (1.f - pick), 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>(150.f * pick * alpha)));
		}

		for (std::size_t index = 0; index < this->m_modes.size(); ++index)
		{
			const bool selected = (this->m_index == static_cast<int>(index));
			const bool hovered = (hovered_option == static_cast<int>(index));

			// unselected options used to sit at text_muted, which read as disabled rather than pickable
			const hue::c_color color = selected
				? theme::text_white
				: (hovered ? theme::text_white : theme::text_secondary);

			draw_text(this->m_modes[index],
				segment_x + static_cast<float>(index) * option_width + option_width * 0.5f, center_y,
				font_face::regular, k_option_font, color.modulate(alpha), align_center | align_middle);
		}
	}

	void c_mode_setting::propagate_if_changed(const std::string& module_name)
	{
		/* INT_MIN sentinel forces the first frame to sync the initial selection
		   to the real dt::EnumSetting. Subsequent frames only push on a
		   real change. Bridge takes the string, not the index, to sidestep
		   any reordering mismatch between the demo card and the real enum. */
		if (this->m_index == this->m_last_pushed) return;
		this->m_last_pushed = this->m_index;
		if (this->m_index >= 0
		 && this->m_index < static_cast<int>(this->m_modes.size()))
		{
			dt::vgui::bridge::applyMode(module_name, this->m_name,
				this->m_modes[static_cast<std::size_t>(this->m_index)]);
		}
	}

	/* ------------------------------------------------------------------ */
	/* module                                                              */
	/* ------------------------------------------------------------------ */

	c_module::c_module(std::string name, std::string description, std::string category)
		: m_name(std::move(name)), m_description(std::move(description)), m_category(std::move(category))
	{
	}

	c_module* c_module::add_setting(std::shared_ptr<c_setting> setting)
	{
		this->m_settings.push_back(std::move(setting));
		return this;
	}

	c_module* c_module::set_enabled(bool enabled)
	{
		this->m_enabled = enabled;
		this->m_toggle = enabled ? 1.f : 0.f;
		return this;
	}

	c_module* c_module::set_keybind(std::string keybind)
	{
		this->m_keybind = std::move(keybind);
		return this;
	}

	namespace
	{
		/* @what a virtual key looks like.

		   Windows already knows: MapVirtualKey turns the VK into a scan code and
		   GetKeyNameText turns that into whatever the user's own layout calls it,
		   which is the only name that will match the key they are looking at. The
		   engine's KeybindSetting names keys the same way; this is that function,
		   living where the gui can reach it. */
		std::string vk_label(int vk)
		{
			if (vk == VK_RBUTTON) return "MOUSE 2";
			if (vk == VK_XBUTTON1) return "M4";
			if (vk == VK_XBUTTON2) return "M5";
			if (vk <= 0 || vk >= 256)
				return "NONE";

			char buffer[64]{};
			const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);

			if (scan != 0 && GetKeyNameTextA(static_cast<LONG>(scan << 16), buffer, sizeof(buffer)) > 0)
			{
				std::string name{ buffer };

				std::transform(name.begin(), name.end(), name.begin(),
					[](unsigned char character) { return static_cast<char>(std::toupper(character)); });

				return name;
			}

			return "KEY " + std::to_string(vk);
		}

		/* @not every virtual key may be bound.

		   Side buttons are valid module binds. set_binding() snapshots every
		   already-held button when the pill is armed, so the left click used to
		   open capture cannot be mistaken for M4/M5 on the following frame. The
		   two client keys remain excluded because the hotkey poll claims them
		   before any module can. */
		bool vk_bindable(int vk)
		{
			if (vk == dt::vgui::bridge::guiToggleKey())
				return false;
			switch (vk)
			{
			case VK_LBUTTON: case VK_CANCEL:
			case VK_MBUTTON:
				return false;
			default:
				return vk > 0 && vk < 256;
			}
		}
	}

	void c_module::set_keybind_vk(int vk)
	{
		if (!vk_bindable(vk))
			vk = 0;

		this->m_keybind_vk = vk;
		this->m_keybind = vk_label(vk);
	}

	void c_module::set_binding(bool binding)
	{
        if (binding) s_auxiliary_binding = false;
		if (binding == this->m_binding)
			return;

		if (binding)
		{
			/* Whoever was waiting stops waiting. */
			if (s_binding != nullptr && s_binding != this)
				s_binding->m_binding = false;

			s_binding = this;

			/* Snapshot what is already held so a modifier the user was leaning on
			   when they clicked is not read as their answer. */
			for (int vk = 1; vk < 256; ++vk)
				s_bind_held[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
		}
		else if (s_binding == this)
		{
			s_binding = nullptr;
		}

		this->m_binding = binding;
	}

	void c_module::cancel_binding()
	{
        s_auxiliary_binding = false;
		if (s_binding == nullptr)
			return;

		s_binding->m_binding = false;
		s_binding = nullptr;
	}

	void c_module::poll_bind_capture()
	{
		if (!this->m_binding)
			return;

		/* @the keyboard itself, not ImGui's queue.

		   ImGui hands out ImGuiKey, which has no relationship to the Win32 VK the
		   engine compares against, and there is no translation table anywhere in
		   this client. Worse, its named-key range contains the mouse buttons and
		   the scroll wheel, so iterating it captured a click or a scroll tick as
		   though it were a key. Reading GetAsyncKeyState gives the VK the engine
		   wants, directly, with no table to keep in sync and no aliases to
		   exclude. It is the same primitive the hotkey poll has always used. */
		for (int vk = 1; vk < 256; ++vk)
		{
			const bool held = (GetAsyncKeyState(vk) & 0x8000) != 0;

			if (!held)
			{
				s_bind_held[vk] = false;
				continue;
			}

			/* Down since before the pill was armed: not an answer. */
			if (s_bind_held[vk])
				continue;

			if (vk == VK_ESCAPE || vk == VK_BACK)
			{
				this->set_keybind_vk(0);
				this->set_binding(false);

				dt::vgui::bridge::applyKeybind(this->m_name, 0);
				return;
			}

			if (!vk_bindable(vk))
				continue;

			this->set_keybind_vk(vk);
			this->set_binding(false);

			/* The half that was missing. Every other control on this card has
			   pushed its value through the bridge for a while; the bind pill
			   wrote a string into itself and stopped, so the real Module's VK
			   stayed 0 and no key ever matched it. */
			dt::vgui::bridge::applyKeybind(this->m_name, this->m_keybind_vk);
			return;
		}
	}

	bool c_module::enabled() const { return dt::vgui::bridge::isEnabled(this->m_name); }

	void c_module::update(float delta)
	{
		this->m_enabled = dt::vgui::bridge::isEnabled(this->m_name);
		this->m_description = dt::vgui::bridge::moduleDescription(this->m_name);
		this->m_toggle = anim::lerp(this->m_toggle, this->m_enabled ? 1.f : 0.f, 14.f, delta);

		for (auto& setting : this->m_settings)
			setting->update(delta);
	}

	void c_module::advance_bind_animation(float delta)
	{
		/* Once per frame, whoever draws the pill first. See m_bind_frame. */
		const int frame = ImGui::GetFrameCount();
		if (this->m_bind_frame == frame)
			return;

		this->m_bind_frame = frame;

		/* Here rather than in render_keybind_button because the pill is drawn
		   twice when the drawer is open beside its own card, and the keyboard
		   must be read once. This function is already the once-a-frame one. */
		this->poll_bind_capture();

		this->m_bind_mix = anim::lerp(this->m_bind_mix, this->m_binding ? 1.f : 0.f, 13.f, delta);
		this->m_bind_glow = this->m_bind_pulse.update(delta);

		/* the dots' clock only runs while there is something to animate, and is
		   rewound between binds so every wait starts on the same frame of the loop */
		if (this->m_binding)
			this->m_bind_phase += delta;
		else if (this->m_bind_mix < 0.01f)
			this->m_bind_phase = 0.f;
	}

	c_module::keybind_hit_t c_module::render_keybind_button(float right_edge, float center_y,
		const frame_input_t& input, float alpha, float minimum_width, float horizontal_padding)
	{
		this->advance_bind_animation(input.m_delta);
        if (!this->m_binding) this->set_keybind_vk(vgui::bridge::bindKey(this->m_name));

		/* @waiting:
			crossfaded, never switched.

			The pill has two states — it shows a key, or it is waiting for one — and
			they are different widths and different contents. Driving both off one
			0..1 mix means the label dissolves into the dots, the pill's width slides
			between them, and its violet lights up, all on the same curve. A boolean
			would have made each of those a jump cut. */
		const float waiting = this->m_bind_mix;

		/* --- geometry ---------------------------------------------------------- */
		constexpr float dot_radius = 2.1f;
		constexpr float dot_spacing = 7.4f;
		constexpr float dots_span = dot_spacing * 2.f + dot_radius * 2.f;

		const float label_width = text_width(this->m_keybind, font_face::regular, 11.f) + horizontal_padding * 2.f;
		const float waiting_width = dots_span + horizontal_padding * 2.f;

		/* @the pill does not resize any more, and that is the fix.

		   It used to interpolate its width between the label's and the dots', which
		   sounds smoother than snapping and is worse: the width is rounded up to a
		   whole even pixel so the contents can be centred crisply, so an animating
		   width does not slide — it JUMPS, two pixels at a time, and drags the dots
		   sideways with it. That stepping is what read as lag.

		   So the pill is sized to hold whichever of the two states is wider and then
		   left alone. Nothing moves; the label simply dissolves into the dots where
		   it stands.

		   An even width on an integer origin puts the pill's centre on a whole pixel;
		   a container with a fractional centre renders half a pixel lopsided, and
		   anything centred in it inherits that. */
		keybind_hit_t hit{};
		hit.m_width = std::ceil(std::max({ minimum_width, label_width, waiting_width }));

		if (static_cast<int>(hit.m_width) % 2 != 0)
			hit.m_width += 1.f;

		hit.m_x = std::round(right_edge - hit.m_width);

		constexpr float height = 21.f;
		const float y = std::round(center_y - height * 0.5f);

		const bool hovered = input.hovered(hit.m_x, y, hit.m_width, height);

		/* The same visibility gate the card's toggle has. Without it a pill on a
		   card that is still fading in — or on the drawer's copy while the drawer
		   closes — answers a click aimed at whatever is underneath it. */
		if (hovered && input.m_left_clicked && alpha > 0.35f)
		{
			this->set_binding(!this->m_binding);
			this->m_bind_pulse.kick();

			hit.m_clicked = true;
		}

        if(hovered && input.m_right_clicked && alpha>0.35f) {
            this->set_binding(false);
            bind_popup::open(this->m_name,hit.m_x,y+height+4.f);
            hit.m_clicked=true;
        }
		/* --- body -------------------------------------------------------------- */
		const hue::c_color resting = hovered ? theme::bg_element.lerp(theme::text_white, 0.05f) : theme::bg_element;
		const hue::c_color background = resting.lerp(theme::accent_violet_deep.alpha(140), waiting);

		draw::rounded_rect(hit.m_x, y, hit.m_width, height, 5.f, background.modulate(alpha));

		const hue::c_color resting_outline = hovered ? theme::border.lerp(theme::text_white, 0.15f) : theme::border;
		const hue::c_color outline = resting_outline.lerp(theme::accent_violet, waiting);

		draw::border(hit.m_x, y, hit.m_width, height, 5.f, 1.f,
			outline.alpha(static_cast<int>((255.f - 15.f * (1.f - waiting)) * alpha)));

		if (const float pulse = this->m_bind_glow; pulse > 0.01f)
		{
			draw::shadow(hit.m_x, y, hit.m_width, height, 5.f, 6.f + 14.f * (1.f - pulse), 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>(160.f * pulse * alpha)));
		}

		/* while waiting the pill breathes, so a bind left open is visibly still open */
		if (waiting > 0.02f)
		{
			const float breath = 0.5f + 0.5f * std::sin(this->m_bind_phase * 3.4f);

			draw::shadow(hit.m_x, y, hit.m_width, height, 5.f, 7.f + 5.f * breath, 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>((45.f + 35.f * breath) * waiting * alpha)));
		}

		const float pill_center_x = hit.m_x + hit.m_width * 0.5f;
		const float pill_center_y = y + height * 0.5f;

		/* --- the key it is bound to -------------------------------------------- */
		if (waiting < 0.99f)
		{
			const hue::c_color text = hovered ? theme::text_white : theme::text_secondary;

			// centred on the pill it sits in, not on the row, otherwise a rounded y offsets the glyph
			draw_text(this->m_keybind, pill_center_x, pill_center_y,
				font_face::regular, 11.f, text.modulate(alpha * (1.f - waiting)),
				align_center | align_middle);
		}

		/* --- waiting for a key: three dots walking ----------------------------- */
		if (waiting > 0.01f)
		{
			/* Grey, not white. The dots are a placeholder for a value that does not
			   exist yet, and white here would read as content — the same weight the
			   key itself will have once it is bound. */
			constexpr hue::c_color dot_colour{ 150, 150, 158, 255 };

			const float first_x = pill_center_x - dot_spacing;

			for (int index = 0; index < 3; ++index)
			{
				/* One wave rolling left to right, rather than three dots blinking:
				   the offset is what turns a pulse into movement.

				   The dots no longer hop. A 2px vertical bounce on a 2px dot is most
				   of the dot's own diameter, and at that scale it reads as jitter
				   rather than as travel — especially against a pill that was already
				   stepping. Brightness and size carry the wave instead, which is
				   smooth at any size because neither is quantised to the pixel grid. */
				const float phase = this->m_bind_phase * 3.1f - static_cast<float>(index) * 0.70f;
				const float wave = anim::ease_in_out_sine(0.5f + 0.5f * std::sin(phase));

				const float radius = dot_radius * (0.74f + 0.42f * wave);
				const int opacity = static_cast<int>((80.f + 155.f * wave) * waiting * alpha);

				draw::circle(first_x + dot_spacing * static_cast<float>(index),
					pill_center_y, radius, dot_colour.alpha(opacity));
			}
		}

		return hit;
	}

	bool c_module::render_card(float target_x, float target_y, float target_w, float h,
		bool selected_in_sidecar, const frame_input_t& input, float alpha)
	{
		this->update(input.m_delta);

		/* spring physics: cards glide into place instead of snapping */
		if (this->m_visual_y < 0.f)
			this->m_visual_y = target_y + 40.f * (1.f - alpha);

		this->m_visual_y = anim::lerp(this->m_visual_y, target_y, 14.f, input.m_delta);

		if (this->m_visual_w < 0.f)
			this->m_visual_w = target_w;
		else
			this->m_visual_w = anim::lerp(this->m_visual_w, target_w, 16.f, input.m_delta);

		const float x = target_x;
		const float y = this->m_visual_y;
		const float w = this->m_visual_w;

		const bool hovered = input.hovered(x, y, w, h);
		this->m_hover = anim::lerp(this->m_hover, hovered ? 1.f : 0.f, 16.f, input.m_delta);

		/* depth */
		draw::shadow(x, y, w, h, 10.f, 14.f * alpha, 0.f, 2.f, hue::c_color(0, 0, 0, static_cast<int>(110.f * alpha)));

		/* body, warms towards theme::card_enabled as the module comes on */
		const hue::c_color background = theme::bg_card
			.lerp(theme::card_enabled, this->m_toggle)
			.modulate(alpha);
		draw::rounded_rect(x, y, w, h, 10.f, background);

		if (selected_in_sidecar || this->m_toggle > 0.05f)
		{
			const float factor = std::max(this->m_toggle, selected_in_sidecar ? 1.f : 0.f);
			draw::border(x, y, w, h, 10.f, 1.2f,
				theme::accent_violet.alpha(static_cast<int>(255.f * theme::accent_ring_opacity * factor * alpha)));
		}
		else if (this->m_hover > 0.05f)
		{
			draw::border(x, y, w, h, 10.f, 1.f,
				theme::border.lerp(theme::accent_violet, 0.35f).alpha(static_cast<int>(255.f * this->m_hover * alpha)));
		}
		else
		{
			draw::border(x, y, w, h, 10.f, 1.f, theme::border.modulate(alpha));
		}

		if (const float toggled = this->m_toggled.update(input.m_delta); toggled > 0.01f)
		{
			// a ring leaves the card, so a toggle registers even out of the corner of the eye
			draw::shadow(x, y, w, h, 10.f, 8.f + 26.f * (1.f - toggled), 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>(150.f * toggled * alpha)));
		}

		const float center_y = y + h * 0.5f;

		/* right controls: the keybind pill */
		constexpr float right_padding = 14.f;
		const keybind_hit_t bind = this->render_keybind_button(x + w - right_padding, center_y, input, alpha, 26.f, 7.f);

		this->m_bind_hover = anim::lerp(this->m_bind_hover,
			(input.hovered(bind.m_x, center_y - 10.f, bind.m_width, 20.f) || this->m_binding) ? 1.f : 0.f, 16.f, input.m_delta);

		/* title and truncated description, neither is allowed to run under the keybind */
		const hue::c_color title(
			static_cast<int>(160.f + (255.f - 160.f) * this->m_toggle),
			static_cast<int>(160.f + (255.f - 160.f) * this->m_toggle),
			static_cast<int>(165.f + (255.f - 165.f) * this->m_toggle),
			static_cast<int>(255.f * alpha)
		);
		draw_text(this->m_name, x + 18.f, center_y - 7.f, font_face::bold, 14.f, title, align_left | align_middle);

		const float description_width = std::max(20.f, (bind.m_x - 14.f) - (x + 18.f));
		draw_text(truncate_text(this->m_description, description_width, font_face::regular, 10.5f),
			x + 18.f, center_y + 8.f, font_face::regular, 10.5f, theme::text_muted.modulate(alpha), align_left | align_middle);

		/* card behaviour: left toggles the module, right opens the settings drawer */
		bool request_drawer = false;

		if (hovered && alpha > 0.35f)
		{
			if (input.m_left_clicked && !bind.m_clicked)
			{
				this->m_enabled = !this->m_enabled;
				this->m_toggled.kick();

				/* Plumb the card toggle through to the real dt::Module in the
				   ModuleManager registry. Without this the card animates but no
				   engine-side code ever sees the switch flip — Module::setEnabled
				   is never called, onEnable/onDisable never fire, and modules
				   like TriggerBot look "activated" in the GUI while doing
				   nothing at all. (The rest of the setting apply* wiring lives
				   in c_slider_setting / c_boolean_setting / c_mode_setting, and
				   still needs the same treatment; the toggle is the one that
				   blocked triggerbot from ever running.) */
				dt::vgui::bridge::applyToggle(this->m_name, this->m_enabled);
			}

			if (input.m_right_clicked && !bind.m_clicked)
				request_drawer = true;
		}

		return request_drawer;
	}

	float c_module::settings_height() const
	{
		/* Hidden rows contribute nothing — otherwise the drawer would reserve
		   scroll space for settings that are not drawn. */
		float total = 0.f;
		for (const auto& s : this->m_settings)
			if (s && s->visible()) total += s->height();
		return total;
	}

	/* @setting tooltip.

	   One tooltip for the whole drawer, so the state is file-static: which row
	   the cursor has been resting on, for how long, and how far the panel has
	   faded in. The panel only starts after a short dwell -- a cursor crossing
	   the drawer on its way somewhere else should not flash every description
	   it passes -- and fades rather than pops so it reads as a hint, not a
	   dialog. Drawn AFTER every row, on the same draw list with a full-screen
	   clip push (the way bind_popup does), so it sits on top of the drawer
	   instead of being cut by its clip rect. */
	namespace
	{
		constexpr float k_tip_delay_s   = 0.15f;
		constexpr float k_tip_max_width = 220.f;
		constexpr float k_tip_pad_x     = 9.f;
		constexpr float k_tip_pad_y     = 7.f;

		const c_setting* s_tip_setting{ nullptr };
		float s_tip_dwell{ 0.f };
		float s_tip_alpha{ 0.f };

		void render_setting_tooltip(const c_setting* hovered, const frame_input_t& input, float alpha)
		{
			if (hovered != s_tip_setting)
			{
				s_tip_setting = hovered;
				s_tip_dwell = 0.f;
				s_tip_alpha = 0.f;
			}
			if (!hovered) return;

			s_tip_dwell += input.m_delta;
			const float target = s_tip_dwell >= k_tip_delay_s ? 1.f : 0.f;
			s_tip_alpha = anim::lerp(s_tip_alpha, target, 18.f, input.m_delta);
			if (s_tip_alpha < 0.02f) return;

			const std::string& text = hovered->description();
			const rich_text_t layout = layout_rich_text(text, font_face::regular, 10.5f, k_tip_max_width);
			const float w = layout.m_width + k_tip_pad_x * 2.f;
			const float h = layout.m_height + k_tip_pad_y * 2.f;

			/* beside the cursor, and kept inside the canvas: a tooltip that hangs
			   off the right edge for the last column would be half unreadable */
			float x = input.m_cursor.x + 14.f;
			float y = input.m_cursor.y + 18.f;
			x = std::clamp(x, 4.f, static_cast<float>(c_app::k_window_width) - w - 4.f);
			y = std::clamp(y, 4.f, static_cast<float>(c_app::k_window_height) - h - 4.f);

			const float a = s_tip_alpha * alpha;

			auto* dl = g_render->draw_list();
			dl->PushClipRectFullScreen();
			draw::shadow(x, y, w, h, 6.f, 14.f, 0.f, 2.f, hue::c_color(0, 0, 0, static_cast<int>(120.f * a)));
			draw::rounded_rect(x, y, w, h, 6.f, hue::c_color(23, 24, 27, 255).modulate(a));
			draw::border(x, y, w, h, 6.f, 1.f, hue::c_color(145, 145, 160, 55).modulate(a));
			draw_rich_text(layout, x + k_tip_pad_x, y + k_tip_pad_y, font_face::regular, 10.5f,
				theme::text_secondary.lerp(theme::text_white, 0.35f).modulate(a));
			dl->PopClipRect();
		}
	}

	void c_module::render_settings(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		float cursor_y = y;
		const c_setting* tip_target = nullptr;

		/* `index` counts only VISIBLE rows so the staggered entrance stays
		   contiguous — otherwise hiding a row in the middle would leave a gap
		   in the animation sequence. */
		std::size_t index = 0;
		for (auto& setting : this->m_settings)
		{
			if (!setting || !setting->visible()) continue;

			/* Snappy staggered entrance, each row trails the one above it.
			   Scale alpha by (1 + index*0.06) so that at alpha == 1 every row
			   fully settles at progress == 1 (offset_x == 0). Without the
			   scale, rows past index ~5 stayed stuck at a fractional progress
			   forever, visibly indenting their label + checkbox. */
			const float k = static_cast<float>(index) * 0.06f;
			const float progress = anim::saturate((alpha * (1.f + k) - k) / 0.70f);
			const float offset_x = (1.f - progress) * 36.f;
			const float row_alpha = std::min(1.f, progress * 1.6f) * alpha;

			setting->render(x + offset_x, cursor_y, w, input, row_alpha);

			/* the row's own hover rect is the row's slot in the drawer; a row
			   without a description never becomes the tooltip's subject */
			if (!setting->description().empty()
				&& input.hovered(x + offset_x, cursor_y, w, setting->height()))
			{
				tip_target = setting.get();
			}

			/* After the widget has consumed input for this frame, push its
			   current value through the bridge into the real dt::Setting
			   whenever it changed (and once at startup so the initial sync
			   fires). Without this a slider drag / bool click / enum pick
			   only moves the widget's own state and never reaches the
			   ModuleManager — exactly the same class of bug that stopped
			   TriggerBot from ever running. */
			setting->propagate_if_changed(this->m_name);

			cursor_y += setting->height();
			++index;
		}

		render_setting_tooltip(tip_target, input, alpha);
	}

	/* ------------------------------------------------------------------ */
	/* block picker                                                        */
	/* ------------------------------------------------------------------ */

	namespace
	{
		constexpr float k_bp_header_h  = 30.f;
		constexpr float k_bp_gap       = 4.f;
		constexpr float k_bp_search_h  = 26.f;
		constexpr float k_bp_chip_h    = 22.f;
		constexpr float k_bp_row_h     = 22.f;
		constexpr float k_bp_list_max  = 200.f;

		/* @prefix match on the short id (part after 'minecraft:').

		   Case-insensitive. User asked for "type 'c' → show every block starting
		   with 'c'" rather than a substring hit that also matched 'acacia',
		   'oak_button', 'cracked_stone_bricks-in-the-middle', etc. Substring
		   scoring made the list unusably long for a single-char query. */
		bool bp_prefix(const std::string& hay_short, const std::string& needle)
		{
			if (needle.empty()) return true;
			if (needle.size() > hay_short.size()) return false;
			auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
			for (std::size_t j = 0; j < needle.size(); ++j)
			{
				if (lower(hay_short[j]) != lower(needle[j])) return false;
			}
			return true;
		}

		std::string bp_short_id(const std::string& id)
		{
			auto pos = id.find(':');
			return pos == std::string::npos ? id : id.substr(pos + 1);
		}
	}

	std::size_t c_block_picker_setting::hash_ids(const std::vector<std::string>& ids)
	{
		std::size_t h = 1469598103934665603ull;
		for (const auto& s : ids)
		{
			for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
			h ^= 0x9E3779B97F4A7C15ull;
		}
		return h;
	}

	c_block_picker_setting::c_block_picker_setting(std::string name, std::vector<std::string> initial)
		: c_setting(std::move(name)), m_selected(std::move(initial))
	{
	}

	void c_block_picker_setting::update(float delta)
	{
		this->m_expand.update(this->m_open ? 1.f : 0.f, 250.f, 26.f, delta);
		this->m_scroll = anim::lerp(this->m_scroll, this->m_scroll_target, 22.f, delta);
	}

	float c_block_picker_setting::height() const
	{
		const float reveal = anim::saturate(this->m_expand.m_value);
		if (reveal < 0.001f) return k_bp_header_h + 4.f;
		const float body = k_bp_search_h + k_bp_gap
		                 + (this->m_selected.empty() ? 0.f : k_bp_chip_h + k_bp_gap)
		                 + k_bp_list_max + k_bp_gap;
		return k_bp_header_h + 4.f + reveal * body;
	}

	void c_block_picker_setting::render(float x, float y, float w, const frame_input_t& input, float alpha)
	{
		this->render_collapsed(x, y, w, input, alpha);
		const float reveal = anim::saturate(this->m_expand.m_value);
		if (reveal <= 0.004f) return;
		this->render_expanded(x, y + k_bp_header_h + 4.f, w, input, alpha * anim::saturate(reveal * 1.3f));
	}

	void c_block_picker_setting::render_collapsed(float x, float y, float w,
	                                              const frame_input_t& input, float alpha)
	{
		constexpr float radius = 7.f;
		const float pill_h = 24.f;
		const float pill_w = std::min(200.f, w * 0.55f);
		const float pill_x = x + w - pill_w;
		const float pill_y = y + (k_bp_header_h - pill_h) * 0.5f;
		const float center_y = pill_y + pill_h * 0.5f;

		draw_text(this->m_name, x, center_y, font_face::regular, 12.f,
			theme::text_secondary.modulate(alpha), align_left | align_middle);

		const bool hovered = input.hovered(pill_x, pill_y, pill_w, pill_h);
		this->m_hover_header = anim::lerp(this->m_hover_header, hovered ? 1.f : 0.f, 16.f, input.m_delta);

		if (hovered && input.m_left_clicked)
		{
			this->m_open = !this->m_open;
			if (!this->m_open && s_search_focus == this) s_search_focus = nullptr;
		}

		draw::rounded_rect(pill_x, pill_y, pill_w, pill_h, radius,
			theme::pane(theme::bg_element, 0.5f).modulate(alpha));
		draw::border(pill_x, pill_y, pill_w, pill_h, radius, 1.f,
			hue::c_color(26, 26, 26).modulate(alpha));
		if (this->m_hover_header > 0.01f)
		{
			draw::rounded_rect(pill_x, pill_y, pill_w, pill_h, radius,
				hue::c_color(255, 255, 255, static_cast<int>(10.f * this->m_hover_header * alpha)));
		}

		char label[48];
		if (this->m_selected.empty())
			std::snprintf(label, sizeof(label), "Search blocks...");
		else if (this->m_selected.size() == 1)
			std::snprintf(label, sizeof(label), "%s", bp_short_id(this->m_selected[0]).c_str());
		else
			std::snprintf(label, sizeof(label), "%zu blocks selected", this->m_selected.size());

		draw_text(label, pill_x + 11.f, center_y, font_face::regular, 11.f,
			(this->m_selected.empty() ? theme::text_secondary : theme::text_white).modulate(alpha),
			align_left | align_middle);

		const float reveal = anim::saturate(this->m_expand.m_value);
		draw::disclosure_arrow(pill_x + pill_w - 13.f, center_y, 8.f,
			1.f - 2.f * reveal, 1.f, theme::text_secondary.modulate(alpha));
	}

	void c_block_picker_setting::render_expanded(float x, float y, float w,
	                                             const frame_input_t& input, float alpha)
	{
		constexpr float radius = 7.f;
		const float body_x = x + 4.f;
		const float body_w = w - 8.f;

		float cursor_y = y;

		/* --- search field ------------------------------------------------- */
		const bool search_focused = (s_search_focus == this);
		const bool sf_hovered = input.hovered(body_x, cursor_y, body_w, k_bp_search_h);

		if (input.m_left_clicked)
		{
			s_search_focus = sf_hovered ? this : (search_focused ? nullptr : s_search_focus);
		}

		draw::rounded_rect(body_x, cursor_y, body_w, k_bp_search_h, radius,
			theme::pane(hue::c_color(17, 17, 20), 0.55f).modulate(alpha));
		draw::border(body_x, cursor_y, body_w, k_bp_search_h, radius, 1.f,
			(search_focused ? theme::accent_violet_deep : hue::c_color(32, 32, 38)).modulate(alpha));

		const std::string display = this->m_search.empty()
			? std::string("Type to search (e.g. crafting, ore, chest)")
			: this->m_search + (search_focused ? "_" : "");
		draw_text(display, body_x + 10.f, cursor_y + k_bp_search_h * 0.5f,
			font_face::regular, 11.f,
			(this->m_search.empty() ? theme::text_secondary : theme::text_white).modulate(alpha),
			align_left | align_middle);

		cursor_y += k_bp_search_h + k_bp_gap;

		/* --- selected chip strip ------------------------------------------ */
		if (!this->m_selected.empty())
		{
			const float chip_pad = 6.f;
			float chip_x = body_x;
			bool row_full = false;
			for (auto it = this->m_selected.begin(); it != this->m_selected.end() && !row_full; )
			{
				const std::string short_name = bp_short_id(*it);
				const float tw = text_width(short_name, font_face::regular, 10.f);
				const float chip_w = tw + 22.f;
				if (chip_x + chip_w > body_x + body_w) { row_full = true; break; }

				const bool chip_hover = input.hovered(chip_x, cursor_y, chip_w, k_bp_chip_h);
				draw::rounded_rect(chip_x, cursor_y, chip_w, k_bp_chip_h, 5.f,
					theme::accent_violet_deep.alpha(static_cast<int>(180.f * alpha)));

				draw_text(short_name, chip_x + 7.f, cursor_y + k_bp_chip_h * 0.5f,
					font_face::regular, 10.f, theme::text_white.modulate(alpha),
					align_left | align_middle);
				draw_text("x", chip_x + chip_w - 10.f, cursor_y + k_bp_chip_h * 0.5f,
					font_face::regular, 10.f, theme::text_secondary.modulate(alpha),
					align_center | align_middle);

				if (chip_hover && input.m_left_clicked)
				{
					it = this->m_selected.erase(it);
					continue;
				}

				chip_x += chip_w + chip_pad;
				++it;
			}
			cursor_y += k_bp_chip_h + k_bp_gap;
		}

		/* --- results list ------------------------------------------------- */
		const auto* rows = dt::vgui::bridge::blockPickerRows();
		std::vector<const dt::vgui::bridge::BlockPickerRow*> filtered;
		filtered.reserve(rows ? rows->size() : 0);
		if (rows)
		{
			for (const auto& r : *rows)
			{
				if (bp_prefix(bp_short_id(r.id), this->m_search))
					filtered.push_back(&r);
				if (filtered.size() > 4096) break;
			}
		}

		const float list_h = k_bp_list_max;
		const float list_y = cursor_y;

		draw::rounded_rect(body_x, list_y, body_w, list_h, 9.f,
			theme::pane(hue::c_color(17, 17, 20), 0.4f).modulate(alpha));
		draw::border(body_x, list_y, body_w, list_h, 9.f, 1.f,
			hue::c_color(28, 28, 34).modulate(alpha));

		if (filtered.empty())
		{
			const bool no_registry = !rows || rows->empty();
			const bool has_query   = !this->m_search.empty();
			std::string msg;
			if (no_registry)      msg = "Loading block registry... open a world if you're at the menu.";
			else if (has_query)   msg = "No blocks match \"" + this->m_search + "\".";
			else                  msg = "Type a name to search (crafting, ore, chest, spawner)";

			draw_text(msg, body_x + 10.f, list_y + 20.f, font_face::regular, 11.f,
				theme::text_secondary.modulate(alpha), align_left | align_middle);
			return;
		}

		const float content_h = static_cast<float>(filtered.size()) * k_bp_row_h + 6.f;
		const float max_scroll = std::max(0.f, content_h - list_h);

		if (input.hovered(body_x, list_y, body_w, list_h) && input.m_wheel != 0.f)
		{
			this->m_scroll_target = std::clamp(
				this->m_scroll_target - input.m_wheel * 40.f, 0.f, max_scroll);
		}

		draw::push_clip(body_x, list_y, body_w, list_h);

		this->m_hover_row = -1;
		for (std::size_t index = 0; index < filtered.size(); ++index)
		{
			const float row_y = list_y + 3.f + static_cast<float>(index) * k_bp_row_h - this->m_scroll;
			if (row_y + k_bp_row_h < list_y) continue;
			if (row_y > list_y + list_h) break;

			const auto* row = filtered[index];
			const bool is_selected =
				std::find(this->m_selected.begin(), this->m_selected.end(), row->id)
				!= this->m_selected.end();

			const bool hovered = input.hovered(body_x + 3.f, row_y, body_w - 6.f, k_bp_row_h);
			if (hovered) this->m_hover_row = static_cast<int>(index);

			if (is_selected)
			{
				draw::rounded_rect(body_x + 3.f, row_y, body_w - 6.f, k_bp_row_h, 5.f,
					theme::accent_violet_deep.alpha(static_cast<int>(150.f * alpha)));
			}
			else if (hovered)
			{
				draw::rounded_rect(body_x + 3.f, row_y, body_w - 6.f, k_bp_row_h, 5.f,
					hue::c_color(30, 30, 36, static_cast<int>(200.f * alpha)));
			}

			/* Names only, no visual swatch — the user asked for a search list
			   without block-icon rendering. The engine still uses the block's
			   map colour for the ESP paint, that is not tied to this row. */
			draw_text(row->label.empty() ? bp_short_id(row->id) : row->label,
				body_x + 10.f, row_y + k_bp_row_h * 0.5f,
				font_face::regular, 11.f,
				(is_selected ? theme::text_white : theme::text_secondary).modulate(alpha),
				align_left | align_middle);

			if (hovered && input.m_left_clicked)
			{
				auto it = std::find(this->m_selected.begin(), this->m_selected.end(), row->id);
				if (it == this->m_selected.end()) this->m_selected.push_back(row->id);
				else                              this->m_selected.erase(it);
			}
		}

		draw::pop_clip();
	}

	void c_block_picker_setting::propagate_if_changed(const std::string& module_name)
	{
		const std::size_t h = hash_ids(this->m_selected);
		if (h == this->m_last_pushed_hash && this->m_selected.size() == this->m_last_pushed_size)
			return;
		this->m_last_pushed_hash = h;
		this->m_last_pushed_size = this->m_selected.size();
		dt::vgui::bridge::applyBlockSet(module_name, this->m_name, this->m_selected);
	}

	void c_block_picker_setting::on_character(char character)
	{
		if (s_search_focus != this) return;
		if (character < 32 || character == 127) return;
		if (this->m_search.size() >= 60) return;
		this->m_search.push_back(character);
		this->m_scroll_target = 0.f;
	}

	void c_block_picker_setting::on_backspace()
	{
		if (s_search_focus != this || this->m_search.empty()) return;
		this->m_search.pop_back();
		this->m_scroll_target = 0.f;
	}
}
