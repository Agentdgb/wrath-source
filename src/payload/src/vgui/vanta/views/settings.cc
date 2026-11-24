#include "../../includes.hh"
#include "core/config/ConfigManager.h"

namespace vanta
{
	namespace
	{
		constexpr float k_padding_x = 26.f;
		constexpr float k_section_gap = 22.f;
		constexpr size_t k_max_name_len = 28;

		std::string sanitize_profile_name(const std::string& raw)
		{
			std::string out;
			out.reserve(raw.size());
			for (char c : raw)
			{
				const bool ok = (c >= 'a' && c <= 'z')
					|| (c >= 'A' && c <= 'Z')
					|| (c >= '0' && c <= '9')
					|| c == '-' || c == '_' || c == ' ';
				if (ok)
					out.push_back(c);
			}
			const auto first = out.find_first_not_of(' ');
			if (first == std::string::npos)
				return {};
			const auto last = out.find_last_not_of(' ');
			return out.substr(first, last - first + 1);
		}
	}

	c_settings::c_settings()
	{
		this->refresh_profiles();
	}

	void c_settings::refresh_profiles()
	{
		this->m_profiles = ConfigManager::get().listProfiles();
		if (std::find(this->m_profiles.begin(), this->m_profiles.end(), "default") == this->m_profiles.end())
		{
			this->m_profiles.insert(this->m_profiles.begin(), "default");
		}
		if (this->m_row_anims.size() != this->m_profiles.size())
		{
			this->m_row_anims.resize(this->m_profiles.size());
		}
		this->m_last_refresh_time = ImGui::GetTime();
	}

	void c_settings::on_scroll(float wheel)
	{
		this->m_target_scroll += wheel * 42.f;
		if (this->m_target_scroll > 0.f)
			this->m_target_scroll = 0.f;
		if (this->m_target_scroll < -this->m_max_scroll)
			this->m_target_scroll = -this->m_max_scroll;
	}

	void c_settings::on_character(char character)
	{
		if (!this->m_input_focused)
			return;
		if (character < 32 || character == 127 || (!this->m_select_all && this->m_new_name.size() >= k_max_name_len))
			return;

		const bool ok = (character >= 'a' && character <= 'z')
			|| (character >= 'A' && character <= 'Z')
			|| (character >= '0' && character <= '9')
			|| character == '-' || character == '_' || character == ' ';
		if (ok)
		{
			if (this->m_select_all) this->m_new_name.clear();
			this->m_select_all = false;
			this->m_new_name.push_back(character);
		}
	}

	void c_settings::on_backspace()
	{
		if (!this->m_input_focused || this->m_new_name.empty())
			return;
		if (this->m_select_all) { this->m_new_name.clear(); this->m_select_all = false; return; }
		this->m_new_name.pop_back();
	}

	void c_settings::paste_clipboard()
	{
		const char* clipboard = ImGui::GetClipboardText();
		if (!clipboard) return;
		const auto clean = sanitize_profile_name(std::string(clipboard).substr(0, 128));
		if (this->m_select_all) this->m_new_name.clear();
		this->m_select_all = false;
		const auto remaining = k_max_name_len - std::min(k_max_name_len, this->m_new_name.size());
		this->m_new_name.append(clean.substr(0, remaining));
	}

	void c_settings::on_submit()
	{
		std::string clean = sanitize_profile_name(this->m_new_name);
		if (!this->m_rename_from.empty())
		{
			const bool renamed = ConfigManager::get().renameProfile(this->m_rename_from, clean);
			this->m_status_text = renamed ? "Renamed to '" + clean + "'" : "Name unavailable or config could not be renamed";
			this->m_status_until = ImGui::GetTime() + 4.0;
			if (renamed) {
				this->m_rename_from.clear(); this->m_new_name.clear();
				this->m_input_focused = false; this->m_select_all = false;
				this->refresh_profiles();
			}
			return;
		}
		if (clean.empty())
		{
			/* If the user clicked "Create / Save" without typing a name first,
			   automatically generate the next available config name (config-1, config-2, ...)
			   so 1-click creation always works immediately. */
			const auto existing = ConfigManager::get().listProfiles();
			for (int idx = 1; idx < 1000; ++idx)
			{
				std::string candidate = "config-" + std::to_string(idx);
				if (std::find(existing.begin(), existing.end(), candidate) == existing.end())
				{
					clean = std::move(candidate);
					break;
				}
			}
			if (clean.empty())
				clean = "config-1";
		}

		if (!ConfigManager::validName(clean)) {
			this->m_status_text = "Choose a valid config name";
			this->m_status_until = ImGui::GetTime() + 3.0;
			return;
		}
		if (!ConfigManager::get().saveProfile(clean)) {
			this->m_status_text = "Save failed. Check storage access and try again.";
			this->m_status_until = ImGui::GetTime() + 5.0;
			return;
		}
		vanta::vgui::bridge::switchProfile(clean);
		this->m_new_name.clear();
		this->m_input_focused = false;
		this->m_create_pulse.kick();
		const double now = ImGui::GetTime();
		this->m_create_done_until = now + 1.8;
		this->m_status_text = "Saved & loaded '" + clean + "'";
		this->m_status_until = now + 3.0;
		this->refresh_profiles();
	}

	void c_settings::render_section(const char* title, float x, float& cursor_y, float w, float alpha)
	{
		draw_text(title, x, cursor_y, font_face::bold, 11.f,
			theme::text_muted.modulate(alpha), align_left | align_middle);

		cursor_y += 10.f;
		draw::gradient_line(x, cursor_y, w, 1.f,
			hue::c_color(140, 140, 155, static_cast<int>(45.f * alpha)));

		cursor_y += 16.f;
	}

	void c_settings::render(float x, float y, float w, float h, const frame_input_t& input)
	{
		const double now = ImGui::GetTime();
		if (now - this->m_last_refresh_time > 1.5 || this->m_profiles.empty())
		{
			this->refresh_profiles();
		}

		this->m_target_scroll = std::clamp(this->m_target_scroll, -this->m_max_scroll, 0.f);
		this->m_scroll = anim::lerp(this->m_scroll, this->m_target_scroll, 14.f, input.m_delta);
		this->m_entrance = anim::lerp(this->m_entrance, 1.f, 8.5f, input.m_delta);

		const float alpha = anim::ease_out_cubic(anim::saturate(this->m_entrance));

		draw::push_clip(x, y, w, h);

		float cursor_y = y + 26.f + this->m_scroll;
		const float start_y = cursor_y;
		const float content_x = x + k_padding_x;
		const float content_w = w - k_padding_x * 2.f;

		/* page heading */
		draw_text("Configs", content_x, cursor_y, font_face::bold, 19.f,
			theme::text_white.modulate(alpha), align_left | align_middle);

		if (now < this->m_status_until && !this->m_status_text.empty())
		{
			const float rem = static_cast<float>(std::min(1.0, (this->m_status_until - now) * 2.5));
			const float pill_w = text_width(this->m_status_text, font_face::bold, 11.f) + 22.f;
			constexpr float pill_h = 24.f;
			const float pill_x = content_x + content_w - pill_w;
			const float pill_y = cursor_y - pill_h * 0.5f + 4.f;

			draw::rounded_rect(pill_x, pill_y, pill_w, pill_h, 7.f,
				theme::accent_violet.alpha(static_cast<int>(55.f * alpha * rem)));
			draw::border(pill_x, pill_y, pill_w, pill_h, 7.f, 1.f,
				theme::accent_violet.alpha(static_cast<int>(185.f * alpha * rem)));
			draw_text(this->m_status_text, pill_x + pill_w * 0.5f, pill_y + pill_h * 0.5f,
				font_face::bold, 11.f, theme::text_white.modulate(alpha * rem), align_center | align_middle);
		}

		cursor_y += 24.f;

		/* --- create / save bar --------------------------------------------- */
		{
			constexpr float bar_h = 34.f;
			constexpr float btn_create_w = 90.f;
			constexpr float btn_save_w = 75.f;
			constexpr float btn_import_w = 80.f;
			constexpr float gap = 8.f;
			const float field_w = content_w - btn_create_w - btn_save_w - btn_import_w - gap * 3.f;

			const bool field_hovered = input.hovered(content_x, cursor_y, field_w, bar_h);
			if (input.m_left_clicked)
			{
				this->m_input_focused = field_hovered;
				if (field_hovered)
				{
					c_color_setting::clear_hex_focus();
					c_block_picker_setting::clear_search_focus();
					c_slider_setting::clear_value_focus();
				}
			}

			this->m_input_hover = anim::lerp(this->m_input_hover, field_hovered ? 1.f : 0.f, 16.f, input.m_delta);
			this->m_input_focus_anim.update(this->m_input_focused ? 1.f : 0.f, 240.f, 24.f, input.m_delta);
			const float focus_t = anim::saturate(this->m_input_focus_anim.m_value);

			draw::rounded_rect(content_x, cursor_y, field_w, bar_h, 9.f,
				theme::bg_element.lerp(hue::c_color(24, 22, 30), std::max(focus_t, this->m_input_hover * 0.5f)).modulate(alpha));
			draw::border(content_x, cursor_y, field_w, bar_h, 9.f, 1.f,
				theme::border.lerp(theme::accent_violet, std::max(focus_t, this->m_input_hover * 0.45f)).modulate(alpha));

			const float text_x = content_x + 12.f;
			const float text_y = cursor_y + bar_h * 0.5f;

			if (!this->m_new_name.empty())
			{
				draw_text(this->m_new_name, text_x, text_y, font_face::regular, 12.f,
					theme::text_white.modulate(alpha), align_left | align_middle);
			}

			if (focus_t > 0.05f)
			{
				const float tw = text_width(this->m_new_name, font_face::regular, 12.f);
				const float blink = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 6.5f);
				draw::rounded_rect(text_x + tw + 2.f, text_y - 7.f, 1.5f, 14.f, 0.75f,
					theme::accent_violet.modulate(alpha * focus_t * blink));
			}

			/* Create button */
			const float create_x = content_x + field_w + gap;
			const bool create_hovered = input.hovered(create_x, cursor_y, btn_create_w, bar_h);
			this->m_create_hover = anim::lerp(this->m_create_hover, create_hovered ? 1.f : 0.f, 16.f, input.m_delta);

			if (create_hovered && input.m_left_clicked)
			{
				this->on_submit();
			}

			const bool create_just_done = now < this->m_create_done_until;
			draw::rounded_rect(create_x, cursor_y, btn_create_w, bar_h, 8.f,
				theme::accent_violet.alpha(static_cast<int>((180.f + 55.f * this->m_create_hover) * alpha)));
			draw::border(create_x, cursor_y, btn_create_w, bar_h, 8.f, 1.f,
				theme::accent_violet.lerp(theme::text_white, this->m_create_hover * 0.35f).modulate(alpha));

			if (const float pulse = this->m_create_pulse.update(input.m_delta); pulse > 0.01f)
			{
				draw::shadow(create_x, cursor_y, btn_create_w, bar_h, 8.f, 10.f + 16.f * (1.f - pulse), 0.f, 0.f,
					theme::accent_violet.alpha(static_cast<int>(160.f * pulse * alpha)));
			}

			draw_text(!this->m_rename_from.empty() ? "Save" : (create_just_done ? "Created" : "Create"), create_x + btn_create_w * 0.5f, text_y,
				font_face::bold, 11.5f, theme::text_white.modulate(alpha), align_center | align_middle);

			/* Save Active button */
			const float save_x = create_x + btn_create_w + gap;
			const bool save_hovered = input.hovered(save_x, cursor_y, btn_save_w, bar_h);
			this->m_save_current_hover = anim::lerp(this->m_save_current_hover, save_hovered ? 1.f : 0.f, 16.f, input.m_delta);

			if (save_hovered && input.m_left_clicked)
			{
				if (!this->m_rename_from.empty()) {
					this->m_rename_from.clear(); this->m_new_name.clear();
					this->m_input_focused = false; this->m_select_all = false;
				} else {
				std::string active = ConfigManager::get().currentProfile();
				if (active.empty())
					active = "default";
				if (ConfigManager::get().saveProfile(active)) {
					this->m_save_current_pulse.kick();
					this->m_save_current_done_until = now + 1.8;
					this->m_status_text = "Saved active profile '" + active + "'";
					this->m_status_until = now + 3.0;
				} else {
					this->m_status_text = "Save failed. Check storage access and try again.";
					this->m_status_until = now + 5.0;
				}
				this->refresh_profiles();
				}
			}

			const bool save_cur_done = now < this->m_save_current_done_until;
			draw::rounded_rect(save_x, cursor_y, btn_save_w, bar_h, 8.f,
				(save_cur_done
					? theme::accent_violet.alpha(110)
					: hue::c_color(20, 20, 25).lerp(hue::c_color(30, 28, 38), this->m_save_current_hover)).modulate(alpha));
			draw::border(save_x, cursor_y, btn_save_w, bar_h, 8.f, 1.f,
				(save_cur_done
					? theme::accent_violet
					: theme::border.lerp(theme::accent_violet, this->m_save_current_hover * 0.8f)).modulate(alpha));

			if (const float pulse = this->m_save_current_pulse.update(input.m_delta); pulse > 0.01f)
			{
				draw::shadow(save_x, cursor_y, btn_save_w, bar_h, 8.f, 10.f + 16.f * (1.f - pulse), 0.f, 0.f,
					theme::accent_violet.alpha(static_cast<int>(150.f * pulse * alpha)));
			}

			draw_text(!this->m_rename_from.empty() ? "Cancel" : (save_cur_done ? "Saved" : "Save"), save_x + btn_save_w * 0.5f, text_y,
				save_cur_done ? font_face::bold : font_face::regular, 11.5f,
				(save_cur_done ? theme::text_white : theme::text_secondary.lerp(theme::text_white, this->m_save_current_hover)).modulate(alpha),
				align_center | align_middle);

			const float import_x = save_x + btn_save_w + gap;
			const bool import_hovered = input.hovered(import_x, cursor_y, btn_import_w, bar_h);
			draw::rounded_rect(import_x, cursor_y, btn_import_w, bar_h, 8.f,
				theme::bg_element.lerp(theme::accent_violet.alpha(140), import_hovered ? 1.f : 0.f).modulate(alpha));
			draw::border(import_x, cursor_y, btn_import_w, bar_h, 8.f, 1.f, theme::border.modulate(alpha));
			draw_text("Import", import_x + btn_import_w * 0.5f, text_y, font_face::bold, 11.f,
				theme::text_white.modulate(alpha), align_center | align_middle);
			if (import_hovered && input.m_left_clicked) {
				const char* clipboard = ImGui::GetClipboardText();
				if (!clipboard || !*clipboard) {
					this->m_status_text = "Clipboard is empty";
					this->m_status_until = now + 3.0;
				} else {
					try {
						const auto imported = ConfigManager::get().importProfile(clipboard);
						if (!imported.empty()) {
							vanta::vgui::bridge::switchProfile(imported);
							this->m_status_text = "Imported '" + imported + "'";
							this->m_rename_from.clear(); this->m_new_name.clear();
							this->m_input_focused = false;
							this->refresh_profiles();
						} else {
							this->m_status_text = "Invalid code";
						}
					} catch (...) {
						this->m_status_text = "Invalid code";
					}
					this->m_status_until = now + 4.0;
				}
			}
			cursor_y += bar_h + 16.f;
		}

		/* --- saved profiles zone (square panel container & card grid) ----- */
		const std::string active_profile = ConfigManager::get().currentProfile();
		const int n_profiles = static_cast<int>(this->m_profiles.size());
		const int total_slots = n_profiles + (n_profiles % 2 != 0 ? 1 : 0);
		const int rows = std::max(1, (total_slots + 1) / 2);

		constexpr float zone_pad = 12.f;
		constexpr float zone_header_h = 36.f;
		constexpr float col_gap = 10.f;
		constexpr float row_gap = 10.f;
		constexpr float card_h = 94.f;

		const float zone_x = content_x;
		const float zone_y = cursor_y;
		const float zone_w = content_w;
		const float card_w = (zone_w - zone_pad * 2.f - col_gap) * 0.5f;
		const float zone_cards_y = zone_y + zone_header_h + 10.f;
		const float zone_h = (zone_cards_y - zone_y) + static_cast<float>(rows) * card_h + static_cast<float>(rows - 1) * row_gap + zone_pad;

		/* Ambient shadow and square container panel */
		draw::shadow(zone_x, zone_y, zone_w, zone_h, 12.f, 20.f, 0.f, 4.f,
			hue::c_color(0, 0, 0, static_cast<int>(130.f * alpha)));
		draw::rounded_rect(zone_x, zone_y, zone_w, zone_h, 12.f,
			hue::c_color(13, 13, 17, static_cast<int>(215.f * alpha)));
		draw::border(zone_x, zone_y, zone_w, zone_h, 12.f, 1.f,
			theme::border.modulate(alpha));

		/* Zone Header */
		const float hdr_center_y = zone_y + zone_header_h * 0.5f;
		draw::icon("configs", zone_x + zone_pad + 2.f, hdr_center_y - 7.f, 15.f,
			theme::accent_violet.modulate(alpha));

		draw_text("Profiles", zone_x + zone_pad + 22.f, hdr_center_y, font_face::bold, 12.f,
			theme::text_white.modulate(alpha), align_left | align_middle);

		const std::string count_str = std::to_string(n_profiles);
		const float count_w = text_width(count_str, font_face::bold, 10.f) + 14.f;
		constexpr float count_h = 18.f;
		const float count_x = zone_x + zone_pad + 22.f + text_width("Profiles", font_face::bold, 12.f) + 8.f;

		draw::rounded_rect(count_x, hdr_center_y - count_h * 0.5f, count_w, count_h, 5.f,
			theme::accent_violet.alpha(static_cast<int>(35.f * alpha)));
		draw::border(count_x, hdr_center_y - count_h * 0.5f, count_w, count_h, 5.f, 1.f,
			theme::accent_violet.alpha(static_cast<int>(110.f * alpha)));
		draw_text(count_str, count_x + count_w * 0.5f, hdr_center_y, font_face::bold, 10.f,
			theme::accent_violet.modulate(alpha), align_center | align_middle);

		/* Active profile indicator on header right */
		const std::string act_lbl = active_profile.empty() ? "default" : active_profile;
		const float act_w = text_width(act_lbl, font_face::bold, 11.f);
		const float act_right = zone_x + zone_w - zone_pad - 4.f;

		draw::circle(act_right - act_w - 8.f, hdr_center_y, 3.5f, theme::online_green.modulate(alpha));
		draw_text(act_lbl, act_right, hdr_center_y, font_face::bold, 11.f,
			theme::text_secondary.modulate(alpha), align_right | align_middle);

		/* Header bottom divider */
		draw::gradient_line(zone_x + zone_pad, zone_y + zone_header_h, zone_w - zone_pad * 2.f, 1.f,
			hue::c_color(140, 140, 155, static_cast<int>(35.f * alpha)));

		/* Grid of square profile cards */
		for (size_t i = 0; i < this->m_profiles.size(); ++i)
		{
			const std::string prof = this->m_profiles[i];
			const bool is_active = (prof == active_profile);
			const bool is_default = (prof == "default");
			auto& anim = this->m_row_anims[i];

			const int col = static_cast<int>(i % 2);
			const int row = static_cast<int>(i / 2);
			const float card_x = zone_x + zone_pad + static_cast<float>(col) * (card_w + col_gap);
			const float card_y = zone_cards_y + static_cast<float>(row) * (card_h + row_gap);

			const bool card_hovered = input.hovered(card_x, card_y, card_w, card_h);
			anim.m_row_hover = anim::lerp(anim.m_row_hover, card_hovered ? 1.f : 0.f, 16.f, input.m_delta);

			/* Ambient card depth */
			if (is_active)
			{
				draw::shadow(card_x, card_y, card_w, card_h, 10.f, 14.f, 0.f, 2.f,
					theme::accent_violet.alpha(static_cast<int>(40.f * alpha)));
			}
			else
			{
				draw::shadow(card_x, card_y, card_w, card_h, 10.f, 8.f, 0.f, 2.f,
					hue::c_color(0, 0, 0, static_cast<int>(90.f * alpha)));
			}

			/* Card background */
			draw::rounded_rect(card_x, card_y, card_w, card_h, 10.f,
				(is_active
					? hue::c_color(24, 19, 34).lerp(hue::c_color(30, 24, 44), anim.m_row_hover)
					: theme::bg_element.lerp(hue::c_color(22, 22, 28), anim.m_row_hover)).modulate(alpha));

			/* Card border */
			draw::border(card_x, card_y, card_w, card_h, 10.f, 1.f,
				(is_active
					? theme::accent_violet.alpha(static_cast<int>(190.f * alpha))
					: theme::border.lerp(hue::c_color(52, 50, 64), anim.m_row_hover)).modulate(alpha));

			if (const float pulse = anim.m_pulse.update(input.m_delta); pulse > 0.01f)
			{
				draw::shadow(card_x, card_y, card_w, card_h, 10.f, 10.f + 16.f * (1.f - pulse), 0.f, 0.f,
					theme::accent_violet.alpha(static_cast<int>(145.f * pulse * alpha)));
			}

			/* Card top row: badge, title, status tag */
			constexpr float badge_sz = 26.f;
			const float badge_x = card_x + 10.f;
			const float badge_y = card_y + 10.f;

			draw::rounded_rect(badge_x, badge_y, badge_sz, badge_sz, 6.f,
				(is_active
					? theme::accent_violet.alpha(static_cast<int>(55.f * alpha))
					: hue::c_color(22, 22, 28, static_cast<int>(240.f * alpha))));
			draw::border(badge_x, badge_y, badge_sz, badge_sz, 6.f, 1.f,
				(is_active
					? theme::accent_violet.alpha(static_cast<int>(180.f * alpha))
					: theme::border.modulate(alpha)));
			draw::icon("configs", badge_x + 5.f, badge_y + 5.f, 16.f,
				(is_active ? theme::accent_violet : theme::text_muted).modulate(alpha));

			const float name_x = badge_x + badge_sz + 10.f;
			const float name_y = badge_y + badge_sz * 0.5f;

			draw::push_clip(name_x, card_y, std::max(1.f, card_w - (is_active ? 130.f : 60.f)), 38.f);
			draw_text(prof, name_x, name_y, font_face::bold, 13.f,
				theme::text_white.modulate(alpha), align_left | align_middle);
			draw::pop_clip();

			/* Only show badge on ACTIVE profile (minimal, no redundant "SAVED" badges) */
			if (is_active)
			{
				constexpr float tag_h = 20.f;
				constexpr float tag_w = 60.f;
				const float tag_x = card_x + card_w - 10.f - tag_w;
				const float tag_y = badge_y + (badge_sz - tag_h) * 0.5f;

				draw::rounded_rect(tag_x, tag_y, tag_w, tag_h, 5.f,
					theme::accent_violet.alpha(static_cast<int>(50.f * alpha)));
				draw::border(tag_x, tag_y, tag_w, tag_h, 5.f, 1.f,
					theme::accent_violet.alpha(static_cast<int>(180.f * alpha)));
				draw::circle(tag_x + 9.f, tag_y + tag_h * 0.5f, 2.5f, theme::online_green.modulate(alpha));
				draw_text("Active", tag_x + 15.f + (tag_w - 15.f) * 0.5f, tag_y + tag_h * 0.5f,
					font_face::bold, 9.5f, theme::accent_violet.modulate(alpha), align_center | align_middle);
			}

			/* Card divider */
			const float card_div_y = card_y + 44.f;
			draw::gradient_line(card_x + 10.f, card_div_y, card_w - 20.f, 1.f,
				hue::c_color(140, 140, 155, static_cast<int>(25.f * alpha)));

			/* Card action buttons */
			constexpr float btn_h = 26.f;
			const float btn_y = card_y + card_h - btn_h - 10.f;
			const float btn_center_y = btn_y + btn_h * 0.5f;

			/* 1. Load / Active Button */
			const float load_btn_w = 80.f;
			const float load_x = card_x + 10.f;
			const bool ld_hov = input.hovered(load_x, btn_y, load_btn_w, btn_h);
			anim.m_load_hover = anim::lerp(anim.m_load_hover, ld_hov ? 1.f : 0.f, 16.f, input.m_delta);

			draw::rounded_rect(load_x, btn_y, load_btn_w, btn_h, 7.f,
				(is_active
					? theme::accent_violet.alpha(static_cast<int>(75.f * alpha))
					: theme::bg_element.lerp(theme::accent_violet.alpha(160), anim.m_load_hover)).modulate(alpha));
			draw::border(load_x, btn_y, load_btn_w, btn_h, 7.f, 1.f,
				(is_active
					? theme::accent_violet.alpha(static_cast<int>(190.f * alpha))
					: theme::border.lerp(theme::accent_violet, anim.m_load_hover * 0.85f)).modulate(alpha));
			draw_text(is_active ? "Loaded" : "Load", load_x + load_btn_w * 0.5f, btn_center_y,
				font_face::bold, 11.f, theme::text_white.modulate(alpha), align_center | align_middle);

			if (ld_hov && input.m_left_clicked)
			{
				vanta::vgui::bridge::switchProfile(prof);
				anim.m_pulse.kick();
				this->m_status_text = "Loaded profile '" + prof + "'";
				this->m_status_until = now + 3.0;
			}

			/* 2. Save Button */
			const float save_btn_w = 54.f;
			const float sv_x = load_x + load_btn_w + 6.f;
			const bool sv_hov = input.hovered(sv_x, btn_y, save_btn_w, btn_h);
			anim.m_save_hover = anim::lerp(anim.m_save_hover, sv_hov ? 1.f : 0.f, 16.f, input.m_delta);
			const bool row_saved = now < anim.m_saved_until;

			draw::rounded_rect(sv_x, btn_y, save_btn_w, btn_h, 7.f,
				(row_saved
					? theme::accent_violet.alpha(115)
					: hue::c_color(20, 20, 25).lerp(hue::c_color(32, 30, 42), anim.m_save_hover)).modulate(alpha));
			draw::border(sv_x, btn_y, save_btn_w, btn_h, 7.f, 1.f,
				(row_saved
					? theme::accent_violet
					: theme::border.lerp(theme::accent_violet, anim.m_save_hover * 0.75f)).modulate(alpha));
			draw_text(row_saved ? "Saved!" : "Save", sv_x + save_btn_w * 0.5f, btn_center_y,
				row_saved ? font_face::bold : font_face::regular, 11.f,
				(row_saved ? theme::text_white : theme::text_secondary.lerp(theme::text_white, anim.m_save_hover)).modulate(alpha),
				align_center | align_middle);

			if (sv_hov && input.m_left_clicked)
			{
				if (ConfigManager::get().saveProfile(prof))
				{
					anim.m_pulse.kick();
					anim.m_saved_until = now + 1.8;
					this->m_status_text = "Saved profile '" + prof + "'";
					this->m_status_until = now + 3.0;
				}
				else
				{
					this->m_status_text = "Save failed. Check storage access and try again.";
					this->m_status_until = now + 5.0;
				}
			}

			/* 3. Share Button */
			const float share_btn_w = 54.f;
			const float sh_x = sv_x + save_btn_w + 6.f;
			const bool sh_hov = input.hovered(sh_x, btn_y, share_btn_w, btn_h);
			anim.m_share_hover = anim::lerp(anim.m_share_hover, sh_hov ? 1.f : 0.f, 16.f, input.m_delta);

			draw::rounded_rect(sh_x, btn_y, share_btn_w, btn_h, 7.f,
				theme::bg_element.lerp(theme::accent_violet.alpha(120), anim.m_share_hover).modulate(alpha));
			draw::border(sh_x, btn_y, share_btn_w, btn_h, 7.f, 1.f,
				theme::border.lerp(theme::accent_violet, anim.m_share_hover * 0.75f).modulate(alpha));
			draw_text("Share", sh_x + share_btn_w * 0.5f, btn_center_y, font_face::regular, 10.5f,
				theme::text_white.modulate(alpha), align_center | align_middle);

			if (sh_hov && input.m_left_clicked)
			{
				try
				{
					const auto code = ConfigManager::get().shareProfile(prof);
					ImGui::SetClipboardText(code.c_str());
					this->m_status_text = "Portable code copied for '" + prof + "'";
				}
				catch (const std::exception&)
				{
					this->m_status_text = "Config could not be shared";
				}
				this->m_status_until = now + 4.0;
			}

			/* 4. Rename Button */
			const float ren_btn_w = 58.f;
			const float rn_x = sh_x + share_btn_w + 6.f;
			const bool rn_hov = input.hovered(rn_x, btn_y, ren_btn_w, btn_h);
			anim.m_rename_hover = anim::lerp(anim.m_rename_hover, rn_hov ? 1.f : 0.f, 16.f, input.m_delta);

			draw::rounded_rect(rn_x, btn_y, ren_btn_w, btn_h, 7.f,
				theme::bg_element.lerp(theme::accent_violet.alpha(120), anim.m_rename_hover).modulate(alpha));
			draw::border(rn_x, btn_y, ren_btn_w, btn_h, 7.f, 1.f,
				theme::border.lerp(theme::accent_violet, anim.m_rename_hover * 0.75f).modulate(alpha));
			draw_text("Rename", rn_x + ren_btn_w * 0.5f, btn_center_y, font_face::regular, 10.5f,
				theme::text_white.modulate(alpha), align_center | align_middle);

			if (rn_hov && input.m_left_clicked)
			{
				this->m_rename_from = prof;
				this->m_new_name = prof;
				this->m_input_focused = true;
				this->m_select_all = true;
				this->m_target_scroll = 0.f;
			}

			/* 5. Delete Button (non-default only) */
			if (!is_default)
			{
				const float del_x = rn_x + ren_btn_w + 6.f;
				const float del_btn_w = (card_x + card_w - 12.f) - del_x;
				const bool del_hov = input.hovered(del_x, btn_y, del_btn_w, btn_h);
				anim.m_del_hover = anim::lerp(anim.m_del_hover, del_hov ? 1.f : 0.f, 16.f, input.m_delta);

				draw::rounded_rect(del_x, btn_y, del_btn_w, btn_h, 7.f,
					hue::c_color(28, 15, 18).lerp(hue::c_color(64, 22, 26), anim.m_del_hover).modulate(alpha));
				draw::border(del_x, btn_y, del_btn_w, btn_h, 7.f, 1.f,
					hue::c_color(58, 26, 30).lerp(theme::danger_red, anim.m_del_hover * 0.85f).modulate(alpha));
				draw_text("Delete", del_x + del_btn_w * 0.5f, btn_center_y, font_face::regular, 11.f,
					theme::danger_red.lerp(theme::text_white, anim.m_del_hover * 0.5f).modulate(alpha),
					align_center | align_middle);

				if (del_hov && input.m_left_clicked)
				{
					const bool was_active = is_active;
					ConfigManager::get().deleteProfile(prof);
					if (was_active)
					{
						vanta::vgui::bridge::switchProfile("default");
					}
					this->m_status_text = "Deleted profile '" + prof + "'";
					this->m_status_until = now + 3.0;
					this->refresh_profiles();
					break;
				}
			}
		}

		/* Optional empty slot card when profiles count is odd, to complete the square grid */
		if (n_profiles % 2 != 0)
		{
			const int col = 1;
			const int row = n_profiles / 2;
			const float slot_x = zone_x + zone_pad + static_cast<float>(col) * (card_w + col_gap);
			const float slot_y = zone_cards_y + static_cast<float>(row) * (card_h + row_gap);

			const bool slot_hov = input.hovered(slot_x, slot_y, card_w, card_h);

			draw::rounded_rect(slot_x, slot_y, card_w, card_h, 10.f,
				(slot_hov ? hue::c_color(18, 16, 24, static_cast<int>(160.f * alpha)) : hue::c_color(14, 14, 18, static_cast<int>(100.f * alpha))));
			draw::border(slot_x, slot_y, card_w, card_h, 10.f, 1.f,
				(slot_hov ? theme::accent_violet.alpha(static_cast<int>(120.f * alpha)) : theme::border.modulate(alpha * 0.8f)));

			const float center_x = slot_x + card_w * 0.5f;
			const float center_y = slot_y + card_h * 0.5f;

			draw_text("+", center_x, center_y - 8.f, font_face::bold, 18.f,
				(slot_hov ? theme::accent_violet : theme::text_muted).modulate(alpha), align_center | align_middle);
			draw_text("New profile", center_x, center_y + 10.f, font_face::bold, 11.f,
				(slot_hov ? theme::text_white : theme::text_secondary).modulate(alpha), align_center | align_middle);

			if (slot_hov && input.m_left_clicked)
			{
				this->m_input_focused = true;
				this->m_target_scroll = 0.f;
			}
		}

		cursor_y = zone_y + zone_h + 24.f;

		const float total_content_h = (cursor_y - start_y) + 36.f;
		this->m_max_scroll = std::max(0.f, total_content_h - h);

		draw::pop_clip();
	}
}
