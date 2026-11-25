#include "../../includes.hh"

#include "color_popup.hh"

namespace dt::colour_popup
{
	namespace
	{
		/* the editor's own body, plus the frame around it */
		constexpr float k_editor_w = 236.f;
		constexpr float k_pad = 13.f;
		constexpr float k_title_h = 30.f;

		constexpr float k_width = k_editor_w + k_pad * 2.f;
		constexpr float k_radius = 13.f;

		c_color_setting* g_target = nullptr;
		std::string g_title{};

		/* @where it opens.

		   Static, and deliberately outliving every editing session: the whole point
		   of a window you can move is that moving it means something next time. It
		   is seeded on the first open from the middle of the canvas and is only
		   ever written by a drag after that. */
		float g_x = 0.f;
		float g_y = 0.f;
		bool g_placed = false;

		anim::spring_t g_reveal{};

		bool g_dragging = false;
		float g_grab_x = 0.f;
		float g_grab_y = 0.f;

		float g_close_hover = 0.f;
		anim::pulse_t g_open_pulse{};

		/* published for the shell's input shield */
		float g_rect_x = 0.f, g_rect_y = 0.f, g_rect_w = 0.f, g_rect_h = 0.f;
		bool g_rect_live = false;

		float body_height()
		{
			return k_title_h + c_color_setting::editor_height + k_pad;
		}

		/* @clamp_into_canvas:
			the window is drawn in canvas space and anything outside the canvas is
			scissored away, not scrolled to — so a window dragged past the edge does
			not hang off, it gets sliced. Clamping the position (rather than only
			the drawing) is what stops a drag against the edge from banking up
			travel that has to be undone before the window moves back.
		*/
		void clamp_into_canvas()
		{
			const float limit_x = static_cast<float>(c_app::k_window_width) - k_width - 8.f;
			const float limit_y = static_cast<float>(c_app::k_window_height) - body_height() - 8.f;

			g_x = std::clamp(g_x, 8.f, std::max(8.f, limit_x));
			g_y = std::clamp(g_y, 8.f, std::max(8.f, limit_y));
		}
	}

	void open(c_color_setting* target, std::string title)
	{
		if (target == nullptr)
			return;

		/* opening it on what it already shows is the caller asking to put it away */
		if (g_target == target)
		{
			close();
			return;
		}

		g_target = target;
		g_title = std::move(title);

		if (!g_placed)
		{
			g_placed = true;

			/* first ever open: the middle of the canvas, nudged up, which is clear
			   of both the sidebar and the module grid's busiest region */
			g_x = (static_cast<float>(c_app::k_window_width) - k_width) * 0.5f;
			g_y = (static_cast<float>(c_app::k_window_height) - body_height()) * 0.5f - 30.f;
		}

		clamp_into_canvas();

		g_reveal.snap(0.f);
		g_open_pulse.kick();
	}

	void close()
	{
		g_target = nullptr;
		g_dragging = false;

		/* the hex caret belongs to the field inside this window; leaving it focused
		   would send the next keystroke to something nobody can see */
		if (c_color_setting::hex_focus() != nullptr)
			c_color_setting::clear_hex_focus();
	}

	bool open_for(const c_color_setting* setting)
	{
		return setting != nullptr && g_target == setting;
	}

	void forget(const c_color_setting* setting)
	{
		if (g_target == setting)
		{
			g_target = nullptr;
			g_dragging = false;
		}
	}

	bool rect(float& x, float& y, float& w, float& h)
	{
		if (!g_rect_live)
			return false;

		x = g_rect_x; y = g_rect_y; w = g_rect_w; h = g_rect_h;
		return true;
	}

	void render(const frame_input_t& input)
	{
		/* Ticked on every frame including the ones where it is shut, because a
		   spring that is only integrated while its target is 1 never comes to rest
		   at 0 — it stops wherever the closing frame left it. */
		g_reveal.update(g_target != nullptr ? 1.f : 0.f, 250.f, 27.f, input.m_delta);

		const float reveal = anim::saturate(g_reveal.m_value);

		g_rect_live = reveal > 0.004f && g_target != nullptr;

		if (!g_rect_live)
		{
			g_close_hover = 0.f;
			return;
		}

		const float height = body_height();

		/* --- dragging ------------------------------------------------------ */
		/* Resolved in CANVAS space, unlike the main window's own drag. That is safe
		   here for a reason that does not hold there: moving this window does not
		   change the canvas -> framebuffer mapping, so the drag cannot feed its own
		   result back into its input. */
		const bool on_title = input.hovered(g_x, g_y, k_width, k_title_h);

		if (!input.m_left_down)
			g_dragging = false;
		else if (input.m_left_clicked && on_title)
		{
			g_dragging = true;
			g_grab_x = input.m_cursor.x - g_x;
			g_grab_y = input.m_cursor.y - g_y;
		}

		if (g_dragging)
		{
			g_x = input.m_cursor.x - g_grab_x;
			g_y = input.m_cursor.y - g_grab_y;

			clamp_into_canvas();
		}

		g_rect_x = g_x; g_rect_y = g_y; g_rect_w = k_width; g_rect_h = height;

		/* --- the window ---------------------------------------------------- */
		/* it lifts the last few pixels into place, so it reads as arriving rather
		   than as having always been there */
		const float lift = (1.f - anim::ease_out_cubic(reveal)) * 12.f;

		const float x = g_x;
		const float y = g_y + lift;

		const float alpha = anim::saturate(reveal * 1.2f);

		draw::shadow(x, y, k_width, height, k_radius, 30.f, 0.f, 8.f,
			hue::c_color(0, 0, 0, static_cast<int>(200.f * alpha)));

		/* @it LIFTS. Everything else in glass mode pulls down; this one must not.

		   The darkening compounds, and that is what was wrong here. The window pane
		   underneath has already taken the game down once — brightness 0.62 with a
		   third of a stop of black over it. Blurring that result again and applying
		   another 0.38 of black on top multiplies the two: about half the light,
		   twice the tint, which lands on very nearly black. The blur was running
		   correctly the whole time and there was nothing left in it to see.

		   So this pane brightens instead. What separates a floating window from the
		   surface under it is a difference, and over an already-dimmed backdrop the
		   difference that reads is LIGHTER, not darker — the same reason a sheet of
		   frosted glass over a dark table looks pale rather than black. Its own fill
		   then stays almost out of the way so the blur is what the eye gets. */
		if (theme::glass_enabled && dt::g_app->visible())
		{
			/* Nine rounds, where the window gets six. This pane is a few hundred
			   pixels across, so the extra rounds cost almost nothing — and the pane
			   needs them more than the window does: at this size a merely soft
			   background still shows recognisable shapes through it, and a pane you
			   can read the client through is a hole rather than a surface. */
			panel_glass::blur_under(x, y, k_width, height, k_radius, alpha,
				/*brightness=*/1.45f, /*darken=*/0.05f, /*passes=*/9);
		}

		draw::rounded_rect(x, y, k_width, height, k_radius,
			theme::pane(hue::c_color(14, 14, 17), 0.10f).modulate(alpha));

		draw::border(x, y, k_width, height, k_radius, 1.f,
			hue::c_color(30, 30, 36).modulate(alpha));

		if (const float pulse = g_open_pulse.update(input.m_delta); pulse > 0.01f)
		{
			draw::shadow(x, y, k_width, height, k_radius, 10.f + 22.f * (1.f - pulse), 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>(120.f * pulse * alpha)));
		}

		/* --- title bar ----------------------------------------------------- */
		{
			const float centre_y = y + k_title_h * 0.5f;

			/* @the grip.

			   Three short bars rather than a label saying "drag me". The title
			   already names the colour; what the strip needs to say is that it can
			   be picked up, and a grip is the only thing on a window that means
			   that without words. It brightens while the window is actually being
			   carried. */
			{
				const float grip_x = x + 12.f;
				const float lit = g_dragging ? 1.f : (on_title ? 0.55f : 0.f);

				for (int bar = 0; bar < 3; ++bar)
				{
					draw::rounded_rect(grip_x, centre_y - 4.f + static_cast<float>(bar) * 3.5f, 9.f, 1.5f, 0.75f,
						hue::c_color(96, 96, 108).lerp(theme::accent_violet, lit)
							.modulate(alpha * (0.75f + 0.25f * lit)));
				}
			}

			draw_text(g_title, x + 28.f, centre_y, font_face::bold, 11.5f,
				theme::text_white.modulate(alpha * 0.92f), align_left | align_middle);

			/* --- close ------------------------------------------------------ */
			constexpr float close_size = 20.f;

			const float close_x = x + k_width - close_size - 8.f;
			const float close_y = centre_y - close_size * 0.5f;

			const bool close_hovered = input.hovered(close_x, close_y, close_size, close_size);
			g_close_hover = anim::lerp(g_close_hover, close_hovered ? 1.f : 0.f, 18.f, input.m_delta);

			if (close_hovered && input.m_left_clicked)
			{
				close();
				return;
			}

			if (g_close_hover > 0.01f)
			{
				draw::rounded_rect(close_x, close_y, close_size, close_size, 6.f,
					hue::c_color(52, 24, 30).modulate(g_close_hover * alpha));
			}

			const hue::c_color cross = theme::text_muted.lerp(theme::danger_red, g_close_hover);

			const float cx = close_x + close_size * 0.5f;
			const float cy = close_y + close_size * 0.5f;
			constexpr float reach = 3.6f;

			ImDrawList* draw_list = g_render->draw_list();

			for (int stroke = 0; stroke < 2; ++stroke)
			{
				const float sign = (stroke == 0) ? 1.f : -1.f;

				draw_list->PathClear();
				draw_list->PathLineTo(ImVec2(cx - reach, cy - reach * sign));
				draw_list->PathLineTo(ImVec2(cx + reach, cy + reach * sign));
				draw_list->PathStroke(cross.modulate(alpha).transform(), ImDrawFlags_None, 1.5f);
			}
		}

		/* --- the editor ---------------------------------------------------- */
		/* Clipped, because on the frames the window is still arriving it is drawn
		   short of its resting place and the editor would otherwise poke out of the
		   bottom of a card that has not finished growing into it. */
		draw::push_clip(x, y, k_width, height);

		g_target->update(input.m_delta);
		g_target->render_editor(x + k_pad, y + k_title_h, k_editor_w, input, alpha);

		draw::pop_clip();
	}
}
