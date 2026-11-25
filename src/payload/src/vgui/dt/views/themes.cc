#include "../../includes.hh"
#include "themes.hh"
#include "core/config/ConfigManager.h"

namespace dt
{
	namespace
	{
		constexpr float k_padding_x = 26.f;
	}

	c_themes_view::c_themes_view()
	{
		this->m_glass = std::make_shared<c_boolean_setting>("Glass Backdrop", theme::glass_enabled);
	}

	void c_themes_view::on_scroll(float wheel)
	{
		this->m_target_scroll += wheel * 42.f;
		if (this->m_target_scroll > 0.f)
			this->m_target_scroll = 0.f;
		if (this->m_target_scroll < -this->m_max_scroll)
			this->m_target_scroll = -this->m_max_scroll;
	}

	void c_themes_view::render_section(const char* title, float x, float& cursor_y, float w, float alpha)
	{
		draw_text(title, x, cursor_y, font_face::bold, 11.f,
			theme::text_muted.modulate(alpha), align_left | align_middle);

		cursor_y += 10.f;
		draw::gradient_line(x, cursor_y, w, 1.f,
			hue::c_color(140, 140, 155, static_cast<int>(45.f * alpha)));

		cursor_y += 16.f;
	}

	void c_themes_view::render_theme_card(float cx, float cy, float cw, float ch,
		const theme::theme_preset_t& preset, bool selected, float hover,
		const frame_input_t& input, float alpha)
	{
		const float box_h = ch - 26.f;

		// Card shadow
		draw::shadow(cx, cy, cw, box_h, 11.f, 16.f, 0.f, 4.f,
			hue::c_color(0, 0, 0, static_cast<int>(140.f * alpha)));

		if (selected)
		{
			draw::glow(cx + cw * 0.5f, cy + box_h * 0.5f, box_h * 0.45f, 22.f,
				preset.accent.alpha(static_cast<int>(90.f * alpha)));
		}

		// Preview box body
		draw::rounded_rect(cx, cy, cw, box_h, 11.f, preset.bg_window.modulate(alpha));

		// Border
		if (selected)
		{
			draw::border(cx, cy, cw, box_h, 11.f, 1.5f, preset.accent.modulate(alpha));
		}
		else if (hover > 0.01f)
		{
			draw::border(cx, cy, cw, box_h, 11.f, 1.f,
				preset.accent.alpha(static_cast<int>(130.f * hover * alpha)));
		}
		else
		{
			draw::border(cx, cy, cw, box_h, 11.f, 1.f,
				hue::c_color(36, 36, 44, static_cast<int>(180.f * alpha)));
		}

		// Miniature UI Elements inside the preview box
		constexpr float pad = 8.f;
		const float ix = cx + pad;
		const float iy = cy + pad;
		const float iw = cw - pad * 2.f;
		const float ih = box_h - pad * 2.f;

		// 1. Mini Sidebar
		const float sb_w = 26.f;
		const float sb_h = ih;
		draw::rounded_rect(ix, iy, sb_w, sb_h, 6.f, preset.bg_sidebar.modulate(alpha));

		const float icon_w = 16.f;
		const float icon_h = 13.f;
		const float icon_x = ix + 5.f;

		// Slot 0 (Active!)
		draw::rounded_rect(icon_x, iy + 6.f, icon_w, icon_h, 3.5f,
			preset.secondary_accent.modulate(alpha));

		// Slots 1, 2, 3 (Inactive)
		const hue::c_color slot_dark(32, 32, 40, static_cast<int>(160.f * alpha));
		draw::rounded_rect(icon_x, iy + 6.f + 18.f, icon_w, icon_h, 3.5f, slot_dark);
		draw::rounded_rect(icon_x, iy + 6.f + 36.f, icon_w, icon_h, 3.5f, slot_dark);
		draw::rounded_rect(icon_x, iy + 6.f + 54.f, icon_w, icon_h, 3.5f, slot_dark);

		// 2. Mini Main Panel
		const float main_x = ix + sb_w + 8.f;
		const float main_w = iw - sb_w - 8.f;

		// Top mini tabs bar
		draw::rounded_rect(main_x, iy + 3.f, 36.f, 9.f, 3.f,
			preset.secondary_accent.modulate(alpha));
		draw::rounded_rect(main_x + 40.f, iy + 3.f, 20.f, 9.f, 3.f, slot_dark);
		draw::rounded_rect(main_x + 64.f, iy + 3.f, 20.f, 9.f, 3.f, slot_dark);

		// 3 Rows
		const float row_start_y = iy + 18.f;
		constexpr float row_h = 22.f;
		constexpr float row_gap = 5.f;

		// Row 1: Active Toggle
		const float r1_y = row_start_y;
		draw::rounded_rect(main_x, r1_y, main_w, row_h, 5.f, preset.bg_element.modulate(alpha));
		draw::rounded_rect(main_x + 6.f, r1_y + 9.f, 44.f, 4.f, 2.f,
			hue::c_color(64, 64, 76, static_cast<int>(180.f * alpha)));

		constexpr float tog_w = 22.f;
		constexpr float tog_h = 11.f;
		const float tog_x = main_x + main_w - tog_w - 6.f;
		const float tog_y = r1_y + 5.5f;
		draw::rounded_rect(tog_x, tog_y, tog_w, tog_h, 5.5f, preset.accent.modulate(alpha));
		draw::circle(tog_x + tog_w - 5.5f, tog_y + 5.5f, 4.2f, theme::text_white.modulate(alpha));

		// Row 2: Inactive Toggle
		const float r2_y = r1_y + row_h + row_gap;
		draw::rounded_rect(main_x, r2_y, main_w, row_h, 5.f, preset.bg_element.modulate(alpha));
		draw::rounded_rect(main_x + 6.f, r2_y + 9.f, 34.f, 4.f, 2.f,
			hue::c_color(64, 64, 76, static_cast<int>(180.f * alpha)));

		const float tog2_y = r2_y + 5.5f;
		draw::rounded_rect(tog_x, tog2_y, tog_w, tog_h, 5.5f,
			hue::c_color(36, 36, 44, static_cast<int>(200.f * alpha)));
		draw::circle(tog_x + 5.5f, tog2_y + 5.5f, 4.2f,
			hue::c_color(90, 90, 100, static_cast<int>(alpha * 200.f)));

		// Row 3: Slider
		const float r3_y = r2_y + row_h + row_gap;
		draw::rounded_rect(main_x, r3_y, main_w, row_h, 5.f, preset.bg_element.modulate(alpha));
		draw::rounded_rect(main_x + 6.f, r3_y + 4.5f, 36.f, 3.5f, 1.5f,
			hue::c_color(64, 64, 76, static_cast<int>(180.f * alpha)));
		draw::rounded_rect(main_x + main_w - 22.f, r3_y + 4.5f, 16.f, 3.5f, 1.5f,
			hue::c_color(64, 64, 76, static_cast<int>(180.f * alpha)));

		const float track_x = main_x + 6.f;
		const float track_y = r3_y + 13.5f;
		const float track_w = main_w - 12.f;
		constexpr float track_h = 4.f;
		draw::rounded_rect(track_x, track_y, track_w, track_h, 2.f,
			hue::c_color(0, 0, 0, static_cast<int>(120.f * alpha)));
		draw::rounded_rect(track_x, track_y, track_w * 0.62f, track_h, 2.f,
			preset.accent.modulate(alpha));

		// Text label underneath
		const hue::c_color label_color = selected
			? theme::text_white.modulate(alpha)
			: theme::text_secondary.modulate(alpha * (0.8f + 0.2f * hover));

		draw_text(preset.name, cx + cw * 0.5f, cy + box_h + 13.f, font_face::bold, 13.f,
			label_color, align_center | align_middle);
	}

	void c_themes_view::render(float x, float y, float w, float h, const frame_input_t& input)
	{
		this->m_target_scroll = std::clamp(this->m_target_scroll, -this->m_max_scroll, 0.f);
		this->m_scroll = anim::lerp(this->m_scroll, this->m_target_scroll, 14.f, input.m_delta);
		this->m_entrance = anim::lerp(this->m_entrance, 1.f, 8.5f, input.m_delta);

		const float alpha = anim::ease_out_cubic(anim::saturate(this->m_entrance));

		draw::push_clip(x, y, w, h);

		float cursor_y = y + 26.f + this->m_scroll;
		const float start_y = cursor_y;
		const float content_x = x + k_padding_x;
		const float content_w = w - k_padding_x * 2.f;

		// Page heading
		draw_text("Themes", content_x, cursor_y, font_face::bold, 19.f,
			theme::text_white.modulate(alpha), align_left | align_middle);

		cursor_y += 16.f;
		draw_text("Select a curated visual palette — applies harmonized accents and surfaces instantly",
			content_x, cursor_y, font_face::regular, 11.5f,
			theme::text_muted.modulate(alpha), align_left | align_middle);
		cursor_y += 28.f;

		// --- Section: PRESET PALETTES ---
		this->render_section("PRESET PALETTES", content_x, cursor_y, content_w, alpha);

		// Grid of theme cards
		constexpr float card_w = 210.f;
		constexpr float card_h = 148.f;
		constexpr float gap_x = 16.f;
		constexpr float gap_y = 16.f;

		const int cols = std::max(1, static_cast<int>((content_w + gap_x) / (card_w + gap_x)));

		for (std::size_t i = 0; i < theme::k_preset_count; ++i)
		{
			const auto& preset = theme::k_presets[i];
			const int col = static_cast<int>(i % cols);
			const int row = static_cast<int>(i / cols);

			const float cx = content_x + static_cast<float>(col) * (card_w + gap_x);
			const float cy = cursor_y + static_cast<float>(row) * (card_h + gap_y);

			const bool selected = (theme::current_preset == preset.id);
			const bool hovered = input.hovered(cx, cy, card_w, card_h);

			if (i < this->m_cards.size())
			{
				this->m_cards[i].m_hover = anim::lerp(this->m_cards[i].m_hover,
					hovered ? 1.f : 0.f, 16.f, input.m_delta);
			}

			const float hov = (i < this->m_cards.size()) ? this->m_cards[i].m_hover : 0.f;

			if (hovered && input.m_left_clicked && !selected)
			{
				theme::apply_preset(preset);
				ConfigManager::get().markDirty();
				if (i < this->m_cards.size())
					this->m_cards[i].m_pulse.kick();
			}

			this->render_theme_card(cx, cy, card_w, card_h, preset, selected, hov, input, alpha);
		}

		const int total_rows = static_cast<int>((theme::k_preset_count + cols - 1) / cols);
		cursor_y += static_cast<float>(total_rows) * (card_h + gap_y) + 14.f;

		// --- Section: APPEARANCE OPTIONS ---
		this->render_section("SURFACE & BACKDROP", content_x, cursor_y, content_w, alpha);

		if (this->m_glass)
		{
			this->m_glass->set_enabled(theme::glass_enabled);
			this->m_glass->update(input.m_delta);
			this->m_glass->render(content_x, cursor_y, 280.f, input, alpha);

			if (theme::glass_enabled != this->m_glass->enabled())
			{
				theme::glass_enabled = this->m_glass->enabled();
				ConfigManager::get().markDirty();
			}

			cursor_y += 42.f;
		}

		// Reset to default button
		{
			constexpr float btn_w = 140.f;
			constexpr float btn_h = 32.f;

			const bool r_hovered = input.hovered(content_x, cursor_y, btn_w, btn_h);
			this->m_reset_hover = anim::lerp(this->m_reset_hover, r_hovered ? 1.f : 0.f, 16.f, input.m_delta);

			const double now = ImGui::GetTime();
			const bool reset_done = (now < this->m_reset_done_until);

			if (r_hovered && input.m_left_clicked)
			{
				theme::reset_customisation();
				ConfigManager::get().markDirty();
				this->m_reset_pulse.kick();
				this->m_reset_done_until = now + 1.5;
			}

			draw::rounded_rect(content_x, cursor_y, btn_w, btn_h, 8.f,
				(reset_done
					? theme::online_green.alpha(80)
					: hue::c_color(22, 22, 26).lerp(hue::c_color(34, 32, 42), this->m_reset_hover)).modulate(alpha));

			draw::border(content_x, cursor_y, btn_w, btn_h, 8.f, 1.f,
				(reset_done ? theme::online_green : theme::border.lerp(theme::accent_violet, this->m_reset_hover * 0.7f)).modulate(alpha));

			draw_text(reset_done ? "Restored" : "Reset Palette",
				content_x + btn_w * 0.5f, cursor_y + btn_h * 0.5f, font_face::bold, 11.5f,
				(reset_done ? theme::text_white : theme::text_secondary.lerp(theme::text_white, this->m_reset_hover)).modulate(alpha),
				align_center | align_middle);

			cursor_y += btn_h + 20.f;
		}

		const float total_content_h = (cursor_y - start_y) + 40.f;
		this->m_max_scroll = std::max(0.f, total_content_h - h);

		draw::pop_clip();
	}
}
