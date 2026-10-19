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
		ConfigManager::get().saveProfile(clean);
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

		cursor_y += 16.f;
		draw_text("Create, rename and share profiles — portable codes work without a server", content_x, cursor_y,
			font_face::regular, 11.5f, theme::text_muted.modulate(alpha), align_left | align_middle);
		cursor_y += 28.f;

		/* --- create / save bar --------------------------------------------- */
		this->render_section("CREATE OR SAVE PROFILE", content_x, cursor_y, content_w, alpha);

		{
			constexpr float bar_h = 36.f;
			constexpr float btn_create_w = 122.f;
			constexpr float btn_save_w = 122.f;
			constexpr float btn_import_w = 100.f;
			constexpr float gap = 10.f;
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

			if (this->m_new_name.empty() && !this->m_input_focused)
			{
				draw_text("Config name (or click Create for auto)...", text_x, text_y, font_face::regular, 12.f,
					theme::text_muted.modulate(alpha * 0.8f), align_left | align_middle);
			}
			else
			{
				draw_text(this->m_new_name, text_x, text_y, font_face::regular, 12.f,
					theme::text_white.modulate(alpha), align_left | align_middle);

				if (focus_t > 0.05f)
				{
					const float tw = text_width(this->m_new_name, font_face::regular, 12.f);
					const float blink = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 6.5f);
					draw::rounded_rect(text_x + tw + 2.f, text_y - 7.f, 1.5f, 14.f, 0.75f,
						theme::accent_violet.modulate(alpha * focus_t * blink));
				}
			}

			/* Create & Save button */
			const float create_x = content_x + field_w + gap;
			const bool create_hovered = input.hovered(create_x, cursor_y, btn_create_w, bar_h);
			this->m_create_hover = anim::lerp(this->m_create_hover, create_hovered ? 1.f : 0.f, 16.f, input.m_delta);

			if (create_hovered && input.m_left_clicked)
			{
				this->on_submit();
			}

			const bool create_just_done = now < this->m_create_done_until;
			draw::rounded_rect(create_x, cursor_y, btn_create_w, bar_h, 9.f,
				theme::accent_violet.alpha(static_cast<int>((180.f + 55.f * this->m_create_hover) * alpha)));
			draw::border(create_x, cursor_y, btn_create_w, bar_h, 9.f, 1.f,
				theme::accent_violet.lerp(theme::text_white, this->m_create_hover * 0.35f).modulate(alpha));

			if (const float pulse = this->m_create_pulse.update(input.m_delta); pulse > 0.01f)
			{
				draw::shadow(create_x, cursor_y, btn_create_w, bar_h, 9.f, 10.f + 16.f * (1.f - pulse), 0.f, 0.f,
					theme::accent_violet.alpha(static_cast<int>(160.f * pulse * alpha)));
			}

			draw_text(!this->m_rename_from.empty() ? "Save name" : (create_just_done ? "Created!" : "+ Create / Save"), create_x + btn_create_w * 0.5f, text_y,
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
				ConfigManager::get().saveProfile(active);
				this->m_save_current_pulse.kick();
				this->m_save_current_done_until = now + 1.8;
				this->m_status_text = "Saved active profile '" + active + "'";
				this->m_status_until = now + 3.0;
				this->refresh_profiles();
				}
			}

			const bool save_cur_done = now < this->m_save_current_done_until;
			draw::rounded_rect(save_x, cursor_y, btn_save_w, bar_h, 9.f,
				(save_cur_done
					? theme::accent_violet.alpha(110)
					: hue::c_color(20, 20, 25).lerp(hue::c_color(30, 28, 38), this->m_save_current_hover)).modulate(alpha));
			draw::border(save_x, cursor_y, btn_save_w, bar_h, 9.f, 1.f,
				(save_cur_done
					? theme::accent_violet
					: theme::border.lerp(theme::accent_violet, this->m_save_current_hover * 0.8f)).modulate(alpha));

			if (const float pulse = this->m_save_current_pulse.update(input.m_delta); pulse > 0.01f)
			{
				draw::shadow(save_x, cursor_y, btn_save_w, bar_h, 9.f, 10.f + 16.f * (1.f - pulse), 0.f, 0.f,
					theme::accent_violet.alpha(static_cast<int>(150.f * pulse * alpha)));
			}

			draw_text(!this->m_rename_from.empty() ? "Cancel" : (save_cur_done ? "Saved!" : "Save current"), save_x + btn_save_w * 0.5f, text_y,
				save_cur_done ? font_face::bold : font_face::regular, 11.5f,
				(save_cur_done ? theme::text_white : theme::text_secondary.lerp(theme::text_white, this->m_save_current_hover)).modulate(alpha),
				align_center | align_middle);

			const float import_x = save_x + btn_save_w + gap;
			const bool import_hovered = input.hovered(import_x, cursor_y, btn_import_w, bar_h);
			draw::rounded_rect(import_x, cursor_y, btn_import_w, bar_h, 9.f,
				theme::bg_element.lerp(theme::accent_violet.alpha(140), import_hovered ? 1.f : 0.f).modulate(alpha));
			draw::border(import_x, cursor_y, btn_import_w, bar_h, 9.f, 1.f, theme::border.modulate(alpha));
			draw_text("Import code", import_x + btn_import_w * 0.5f, text_y, font_face::bold, 11.f,
				theme::text_white.modulate(alpha), align_center | align_middle);
			if (import_hovered && input.m_left_clicked) {
				try {
					const char* clipboard = ImGui::GetClipboardText();
					const auto imported = ConfigManager::get().importProfile(clipboard ? clipboard : "");
					vanta::vgui::bridge::switchProfile(imported);
					this->m_status_text = "Imported '" + imported + "'";
					this->m_rename_from.clear(); this->m_new_name.clear();
					this->m_input_focused = false;
					this->refresh_profiles();
				} catch (const std::exception&) { this->m_status_text = "Invalid code or config could not be imported"; }
				this->m_status_until = now + 4.0;
			}
			cursor_y += bar_h + 12.f;

			/* Quick 1-click preset slots so user can save/switch without typing */
			static constexpr const char* k_presets[4] = { "legit", "blatant", "pvp", "hypixel" };
			constexpr float chip_h = 28.f;
			constexpr float chip_gap = 8.f;
			const float chip_w = (content_w - chip_gap * 3.f) * 0.25f;

			for (int p = 0; p < 4; ++p)
			{
				const std::string pname = k_presets[p];
				const float cx = content_x + static_cast<float>(p) * (chip_w + chip_gap);
				const bool chov = input.hovered(cx, cursor_y, chip_w, chip_h);
				this->m_preset_hover[p] = anim::lerp(this->m_preset_hover[p], chov ? 1.f : 0.f, 16.f, input.m_delta);

				const bool exists = std::find(this->m_profiles.begin(), this->m_profiles.end(), pname) != this->m_profiles.end();
				const bool is_cur = (ConfigManager::get().currentProfile() == pname);

				draw::rounded_rect(cx, cursor_y, chip_w, chip_h, 7.f,
					(is_cur
						? theme::accent_violet.alpha(65)
						: hue::c_color(18, 18, 23).lerp(hue::c_color(28, 26, 36), this->m_preset_hover[p])).modulate(alpha));
				draw::border(cx, cursor_y, chip_w, chip_h, 7.f, 1.f,
					(is_cur
						? theme::accent_violet.alpha(180)
						: theme::border.lerp(theme::accent_violet, this->m_preset_hover[p] * 0.7f)).modulate(alpha));

				const std::string clabel = exists ? ("Save / Use: " + pname) : ("+ Create '" + pname + "'");
				draw_text(clabel, cx + chip_w * 0.5f, cursor_y + chip_h * 0.5f,
					is_cur ? font_face::bold : font_face::regular, 11.f,
					(is_cur ? theme::text_white : theme::text_secondary.lerp(theme::text_white, this->m_preset_hover[p])).modulate(alpha),
					align_center | align_middle);

				if (chov && input.m_left_clicked)
				{
					ConfigManager::get().saveProfile(pname);
					vanta::vgui::bridge::switchProfile(pname);
					this->m_status_text = "Saved & active: '" + pname + "'";
					this->m_status_until = now + 3.0;
					this->refresh_profiles();
				}
			}

			cursor_y += chip_h + k_section_gap;
		}

		/* --- saved profiles list ------------------------------------------- */
		this->render_section("SAVED PROFILES", content_x, cursor_y, content_w, alpha);

		const std::string active_profile = ConfigManager::get().currentProfile();
		constexpr float row_h = 44.f;
		constexpr float row_gap = 8.f;

		for (size_t i = 0; i < this->m_profiles.size(); ++i)
		{
			const std::string prof = this->m_profiles[i];
			const bool is_active = (prof == active_profile);
			const bool is_default = (prof == "default");
			auto& anim = this->m_row_anims[i];

			const bool row_hovered = input.hovered(content_x, cursor_y, content_w, row_h);
			anim.m_row_hover = anim::lerp(anim.m_row_hover, row_hovered ? 1.f : 0.f, 16.f, input.m_delta);

			draw::rounded_rect(content_x, cursor_y, content_w, row_h, 10.f,
				(is_active
					? hue::c_color(24, 19, 34).lerp(hue::c_color(30, 24, 42), anim.m_row_hover)
					: theme::bg_element.lerp(hue::c_color(22, 22, 27), anim.m_row_hover)).modulate(alpha));

			draw::border(content_x, cursor_y, content_w, row_h, 10.f, 1.f,
				(is_active
					? theme::accent_violet.alpha(185)
					: theme::border.lerp(hue::c_color(48, 46, 58), anim.m_row_hover)).modulate(alpha));

			if (const float pulse = anim.m_pulse.update(input.m_delta); pulse > 0.01f)
			{
				draw::shadow(content_x, cursor_y, content_w, row_h, 10.f, 8.f + 16.f * (1.f - pulse), 0.f, 0.f,
					theme::accent_violet.alpha(static_cast<int>(135.f * pulse * alpha)));
			}

			const float mid_y = cursor_y + row_h * 0.5f;
			draw::push_clip(content_x + 10.f, cursor_y, std::max(1.f, content_w - 325.f), row_h);
			draw_text(prof, content_x + 14.f, mid_y, font_face::bold, 13.f,
				theme::text_white.modulate(alpha), align_left | align_middle);

			if (is_active)
			{
				const float name_w = text_width(prof, font_face::bold, 13.f);
				const float badge_x = content_x + 14.f + name_w + 10.f;
				constexpr float badge_w = 52.f;
				constexpr float badge_h = 18.f;

				draw::rounded_rect(badge_x, mid_y - badge_h * 0.5f, badge_w, badge_h, 5.f,
					theme::accent_violet.alpha(static_cast<int>(55.f * alpha)));
				draw::border(badge_x, mid_y - badge_h * 0.5f, badge_w, badge_h, 5.f, 1.f,
					theme::accent_violet.alpha(static_cast<int>(150.f * alpha)));
				draw_text("ACTIVE", badge_x + badge_w * 0.5f, mid_y, font_face::bold, 9.5f,
					theme::accent_violet.modulate(alpha), align_center | align_middle);
			}

			draw::pop_clip();
			/* Action buttons on the right: Share, Rename, Load, Save, Delete */
			constexpr float btn_h = 28.f;
			constexpr float btn_w = 56.f;
			constexpr float btn_gap = 6.f;
			const float btn_y = mid_y - btn_h * 0.5f;

			float right_edge = content_x + content_w - 10.f;
			auto extra_button = [&](const char* label) {
				const float bx = right_edge - btn_w;
				right_edge = bx - btn_gap;
				const bool hovered = input.hovered(bx, btn_y, btn_w, btn_h);
				draw::rounded_rect(bx, btn_y, btn_w, btn_h, 7.f,
					theme::bg_element.lerp(theme::accent_violet.alpha(120), hovered ? 1.f : 0.f).modulate(alpha));
				draw::border(bx, btn_y, btn_w, btn_h, 7.f, 1.f, theme::border.modulate(alpha));
				draw_text(label, bx + btn_w * 0.5f, mid_y, font_face::regular, 10.5f,
					theme::text_white.modulate(alpha), align_center | align_middle);
				return hovered && input.m_left_clicked;
			};
			if (extra_button("Share")) {
				try {
					const auto code = ConfigManager::get().shareProfile(prof);
					ImGui::SetClipboardText(code.c_str());
					this->m_status_text = "Portable code copied for '" + prof + "'";
				} catch (const std::exception&) { this->m_status_text = "Config could not be shared"; }
				this->m_status_until = now + 4.0;
			}
			if (extra_button("Rename")) {
				this->m_rename_from = prof; this->m_new_name = prof;
				this->m_input_focused = true; this->m_select_all = true;
				this->m_target_scroll = 0.f;
			}

			if (!is_default)
			{
				const float del_x = right_edge - btn_w;
				right_edge = del_x - btn_gap;

				const bool del_hovered = input.hovered(del_x, btn_y, btn_w, btn_h);
				anim.m_del_hover = anim::lerp(anim.m_del_hover, del_hovered ? 1.f : 0.f, 16.f, input.m_delta);

				draw::rounded_rect(del_x, btn_y, btn_w, btn_h, 7.f,
					hue::c_color(28, 15, 18).lerp(hue::c_color(64, 22, 26), anim.m_del_hover).modulate(alpha));
				draw::border(del_x, btn_y, btn_w, btn_h, 7.f, 1.f,
					hue::c_color(58, 26, 30).lerp(theme::danger_red, anim.m_del_hover * 0.85f).modulate(alpha));
				draw_text("Delete", del_x + btn_w * 0.5f, mid_y, font_face::regular, 11.f,
					theme::danger_red.lerp(theme::text_white, anim.m_del_hover * 0.5f).modulate(alpha),
					align_center | align_middle);

				if (del_hovered && input.m_left_clicked)
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

			/* Save (overwrite this profile) */
			{
				const float sv_x = right_edge - btn_w;
				right_edge = sv_x - btn_gap;

				const bool sv_hovered = input.hovered(sv_x, btn_y, btn_w, btn_h);
				anim.m_save_hover = anim::lerp(anim.m_save_hover, sv_hovered ? 1.f : 0.f, 16.f, input.m_delta);
				const bool row_saved = now < anim.m_saved_until;

				draw::rounded_rect(sv_x, btn_y, btn_w, btn_h, 7.f,
					(row_saved
						? theme::accent_violet.alpha(115)
						: hue::c_color(20, 20, 25).lerp(hue::c_color(32, 30, 40), anim.m_save_hover)).modulate(alpha));
				draw::border(sv_x, btn_y, btn_w, btn_h, 7.f, 1.f,
					(row_saved
						? theme::accent_violet
						: theme::border.lerp(theme::accent_violet, anim.m_save_hover * 0.75f)).modulate(alpha));
				draw_text(row_saved ? "Saved!" : "Save", sv_x + btn_w * 0.5f, mid_y,
					row_saved ? font_face::bold : font_face::regular, 11.f,
					(row_saved ? theme::text_white : theme::text_secondary.lerp(theme::text_white, anim.m_save_hover)).modulate(alpha),
					align_center | align_middle);

				if (sv_hovered && input.m_left_clicked)
				{
					ConfigManager::get().saveProfile(prof);
					anim.m_pulse.kick();
					anim.m_saved_until = now + 1.8;
					this->m_status_text = "Saved profile '" + prof + "'";
					this->m_status_until = now + 3.0;
				}
			}

			/* Load profile */
			{
				const float ld_x = right_edge - btn_w;
				const bool ld_hovered = input.hovered(ld_x, btn_y, btn_w, btn_h);
				anim.m_load_hover = anim::lerp(anim.m_load_hover, ld_hovered ? 1.f : 0.f, 16.f, input.m_delta);

				draw::rounded_rect(ld_x, btn_y, btn_w, btn_h, 7.f,
					(is_active
						? theme::accent_violet.alpha(70)
						: hue::c_color(20, 20, 25).lerp(theme::accent_violet.alpha(140), anim.m_load_hover)).modulate(alpha));
				draw::border(ld_x, btn_y, btn_w, btn_h, 7.f, 1.f,
					(is_active
						? theme::accent_violet
						: theme::border.lerp(theme::accent_violet, anim.m_load_hover * 0.85f)).modulate(alpha));
				draw_text(is_active ? "Loaded" : "Load", ld_x + btn_w * 0.5f, mid_y,
					is_active ? font_face::bold : font_face::regular, 11.f,
					theme::text_white.modulate(alpha), align_center | align_middle);

				if (ld_hovered && input.m_left_clicked)
				{
					vanta::vgui::bridge::switchProfile(prof);
					anim.m_pulse.kick();
					this->m_status_text = "Loaded profile '" + prof + "'";
					this->m_status_until = now + 3.0;
				}
			}

			cursor_y += row_h + row_gap;
		}

		const float total_content_h = (cursor_y - start_y) + 36.f;
		this->m_max_scroll = std::max(0.f, total_content_h - h);

		draw::pop_clip();
	}
}
