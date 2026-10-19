#include "../../includes.hh"
#include "core/config/ConfigManager.h"
#include "../../bridge.h"

namespace vanta
{
	void c_header::render(const std::string& current_category, int active_modules,
		float x, float y, float w, float h, const frame_input_t& input)
	{
		(void)current_category;

		const float start_x = x + 24.f;
		const float right_x = x + w - 24.f;
		const float center_y = y + h * 0.5f;

		/* 1. separator hairline, drawn first so everything else sits on top of it */
		draw::gradient_line(start_x, y + h, right_x - start_x, 1.2f, hue::c_color(140, 140, 155, 110));

		/* 2. unified segmented control capsule [ A-Z | Active ] */
		constexpr float card_width = 126.f;
		constexpr float card_height = 28.f;
		constexpr float corner_radius = 9.f;

		const float card_x = start_x;
		const float card_y = center_y - card_height * 0.5f;

		draw::rounded_rect(card_x, card_y, card_width, card_height, corner_radius, theme::bg_element);
		draw::border(card_x, card_y, card_width, card_height, corner_radius, 1.f, theme::border);

		constexpr float padding = 2.5f;
		constexpr float tab_width = (card_width - padding * 2.f) * 0.5f;
		constexpr float tab_height = card_height - padding * 2.f;
		constexpr float inner_radius = corner_radius - padding;

		const float tab_y = card_y + padding;

		if (input.m_left_clicked)
		{
			const filter_mode previous = this->m_filter;

			if (input.hovered(card_x + padding, tab_y, tab_width, tab_height))
				this->m_filter = filter_mode::alphabetical;
			else if (input.hovered(card_x + padding + tab_width, tab_y, tab_width, tab_height))
				this->m_filter = filter_mode::active;

			if (this->m_filter != previous)
				this->m_filter_pulse.kick();
		}

		// the pill travels a real distance, so it gets a spring and settles with a touch of overshoot
		this->m_slide.update(this->m_filter == filter_mode::active ? 1.f : 0.f, 300.f, 26.f, input.m_delta);

		/* sliding active pill */
		const float slide = std::clamp(this->m_slide.m_value, -0.12f, 1.12f);
		const float pill_x = card_x + padding + slide * tab_width;

		draw::shadow(pill_x, tab_y, tab_width, tab_height, inner_radius, 10.f, 0.f, 0.5f, theme::accent_violet.alpha(85));
		draw::rounded_rect(pill_x, tab_y, tab_width, tab_height, inner_radius, theme::accent_violet_deep.alpha(210));
		draw::border(pill_x, tab_y, tab_width, tab_height, inner_radius, 1.f, theme::accent_violet.alpha(230));

		if (const float switched = this->m_filter_pulse.update(input.m_delta); switched > 0.01f)
		{
			draw::shadow(pill_x, tab_y, tab_width, tab_height, inner_radius,
				8.f + 20.f * (1.f - switched), 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>(180.f * switched)));
		}

		const auto tab_color = [](float weight)
			{
				const int channel = static_cast<int>(140.f + (255.f - 140.f) * weight);
				return hue::c_color(channel, channel, channel);
			};

		draw_text("A-Z", card_x + padding + tab_width * 0.5f, center_y,
			font_face::regular, 12.f, tab_color(anim::saturate(1.f - slide)), align_center | align_middle);

		draw_text("Active", card_x + padding + tab_width + tab_width * 0.5f, center_y,
			font_face::regular, 12.f, tab_color(anim::saturate(slide)), align_center | align_middle);

		/* 3. bookmark drawer, sits right next to the segmented control */
		constexpr float button_size = 28.f;

		// width and height both travel, both get a spring; the drawer is the animation the user sees most
		this->m_config_expand.update(this->m_config_expanded ? 1.f : 0.f, 240.f, 26.f, input.m_delta);
		this->m_dropdown.update((this->m_config_expanded && this->m_dropdown_open) ? 1.f : 0.f, 230.f, 25.f, input.m_delta);

		const float config_expand = anim::saturate(this->m_config_expand.m_value);
		const float dropdown = std::max(0.f, this->m_dropdown.m_value);

		constexpr float full_config_width = 158.f;
		constexpr float compact_config_width = 28.f;

		const float config_width = compact_config_width + (full_config_width - compact_config_width) * config_expand;
		const float config_x = card_x + card_width + 10.f;
		const float config_y = center_y - button_size * 0.5f;

		constexpr float item_height = 28.f;
		const float list_height = static_cast<float>(this->m_configs.size()) * item_height + 10.f;
		const float config_height = button_size + list_height * dropdown;

		const bool top_hovered = input.hovered(config_x, config_y, config_width, button_size);
		const bool card_hovered = input.hovered(config_x, config_y, config_width, config_height);

		this->m_config_hover = anim::lerp(this->m_config_hover, (card_hovered || this->m_config_expanded) ? 1.f : 0.f, 16.f, input.m_delta);

		/* collapse arrow area on the right edge */
		constexpr float arrow_size = 20.f;
		const float arrow_x = config_x + config_width - arrow_size - 4.f;
		const bool arrow_hovered = this->m_config_expanded && input.hovered(arrow_x, config_y, arrow_size, button_size);

		this->m_collapse_arrow_hover = anim::lerp(this->m_collapse_arrow_hover, arrow_hovered ? 1.f : 0.f, 16.f, input.m_delta);

		const std::string& curProfile = vgui::bridge::currentProfile();
		if (!curProfile.empty() && this->m_selected_config != curProfile && !this->m_dropdown_open)
		{
			this->m_selected_config = curProfile;
		}

		if (top_hovered && input.m_left_clicked)
		{
			this->m_drawer_pulse.kick();

			if (!this->m_config_expanded)
			{
				this->m_config_expanded = true;
			}
			else if (input.m_cursor.x <= config_x + 26.f || arrow_hovered)
			{
				// the bookmark icon and the arrow both collapse the drawer
				this->m_config_expanded = false;
				this->m_dropdown_open = false;
			}
			else
			{
				this->m_dropdown_open = !this->m_dropdown_open;
				if (this->m_dropdown_open)
				{
					auto disk_profiles = ConfigManager::get().listProfiles();
					std::vector<std::string> profiles = { "[Save current]", "default", "legit", "hypixel", "blatant" };
					for (const auto& p : disk_profiles)
					{
						bool exists = false;
						for (const auto& existing : profiles)
						{
							if (existing == p) { exists = true; break; }
						}
						if (!exists)
							profiles.push_back(p);
					}
					this->m_configs = std::move(profiles);
					this->m_selected_config = vgui::bridge::currentProfile();
				}
			}
		}

		/* one shadow around the whole expanding card */
		if (dropdown > 0.01f)
		{
			draw::shadow(config_x, config_y, config_width, config_height, 7.f, 18.f * dropdown, 0.f, 3.f,
				hue::c_color(0, 0, 0, static_cast<int>(160.f * dropdown)));
		}

		if (this->m_config_hover > 0.01f)
		{
			draw::shadow(config_x, config_y, config_width, config_height, 7.f, 14.f * this->m_config_hover, 0.f, 1.f,
				theme::accent_violet.alpha(static_cast<int>(35.f * this->m_config_hover)));
		}

		/* The drawer is a surface like any other, so in glass mode it is one too —
		   otherwise the one opaque slab left on screen is the one the user opened
		   deliberately, which is exactly backwards. */
		if (theme::glass_enabled && vanta::g_app->visible())
			panel_glass::blur_under(config_x, config_y, config_width, config_height, 7.f, 1.f, 0.92f, 0.16f);

		draw::rounded_rect(config_x, config_y, config_width, config_height, 7.f,
			theme::pane(hue::c_color(14, 14, 14), 0.30f));

		draw::border(config_x, config_y, config_width, config_height, 7.f, 1.f, theme::border);

		if (const float used = this->m_drawer_pulse.update(input.m_delta); used > 0.01f)
		{
			draw::shadow(config_x, config_y, config_width, button_size, 7.f,
				6.f + 18.f * (1.f - used), 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>(150.f * used)));
		}

		/* header icon */
		constexpr float config_icon_size = 14.5f;
		const hue::c_color mark_color = (this->m_config_hover > 0.1f || this->m_config_expanded) ? theme::text_white : theme::text_secondary;

		draw::icon("mark", config_x + 7.f, config_y + (button_size - config_icon_size) * 0.5f, config_icon_size, mark_color);

		if (config_expand > 0.05f)
		{
			const float text_x = config_x + 26.f;
			const float reserved = arrow_size + 6.f;
			const float available = std::max(10.f, config_width - 26.f - reserved);

			draw_text(truncate_text(this->m_selected_config, available, font_face::regular, 12.f),
				text_x, center_y, font_face::regular, 12.f, theme::text_white.modulate(config_expand), align_left | align_middle);

			const hue::c_color arrow_color = arrow_hovered ? theme::text_white : theme::text_muted.modulate(config_expand);
			draw::chevron(arrow_x + arrow_size * 0.5f, center_y, 7.f, -1, 1.4f, arrow_color);
		}

		/* seamless dropdown content inside the very same card */
		if (dropdown > 0.01f)
		{
			const float divider_y = config_y + button_size;
			draw::gradient_line(config_x + 4.f, divider_y, config_width - 8.f, 1.f,
				hue::c_color(140, 140, 155, static_cast<int>(70.f * dropdown)));

			const float list_start = divider_y + 4.f;

			for (std::size_t index = 0; index < this->m_configs.size(); ++index)
			{
				const float item_y = list_start + static_cast<float>(index) * item_height;

				if (item_y + item_height > config_y + config_height)
					continue;

				/* each row trails the one above it, so the list unrolls instead of popping in */
				const float progress = anim::saturate((dropdown - static_cast<float>(index) * 0.07f) / 0.6f);
				const float row_alpha = anim::ease_out_cubic(progress);
				const float row_offset = (1.f - row_alpha) * 10.f;

				if (row_alpha <= 0.01f)
					continue;

				const bool selected = (this->m_selected_config == this->m_configs[index]);
				const bool item_hovered = input.hovered(config_x + 4.f, item_y, config_width - 8.f, item_height);

				if (item_hovered)
				{
					draw::rounded_rect(config_x + 4.f, item_y, config_width - 8.f, item_height, 6.f,
						hue::c_color(30, 30, 36, static_cast<int>(255.f * row_alpha)));

					if (input.m_left_clicked)
					{
						const std::string chosen = this->m_configs[index];
						if (chosen == "[Save current]" || chosen == this->m_selected_config)
						{
							ConfigManager::get().saveProfile(this->m_selected_config.empty() ? "default" : this->m_selected_config);
						}
						else
						{
							if (!this->m_selected_config.empty())
								ConfigManager::get().saveProfile(this->m_selected_config);
							this->m_selected_config = chosen;
							vgui::bridge::switchProfile(chosen);
							ConfigManager::get().saveProfile(chosen);
						}
						this->m_dropdown_open = false;

						this->m_config_pulse.kick();
					}
				}

				/* the active config carries an accent bar, so the list says which one is loaded
				   without relying on weight alone */
				if (selected)
				{
					draw::rounded_rect(config_x + 6.f, item_y + 7.f, 2.5f, item_height - 14.f, 1.25f,
						theme::accent_violet.modulate(row_alpha));
				}

				draw_text(this->m_configs[index], config_x + 15.f + row_offset, item_y + item_height * 0.5f,
					selected ? font_face::bold : font_face::regular, 12.f,
					(selected ? theme::text_white : (item_hovered ? theme::text_white : theme::text_secondary)).modulate(row_alpha),
					align_left | align_middle);
			}
		}

		/* 4. eraser button on the far right corner */
		const float eraser_x = right_x - button_size;

		/* @what is left of the bar once the controls have taken their share.
		
		   Measured against the drawer's FULL width rather than its current one, so
		   the handle does not breathe in and out as the drawer expands — a grab
		   point that moves while you are reaching for it is worse than a smaller
		   one that stays put. */
		{
			const float band_start = config_x + full_config_width + 10.f;
			const float band_end = eraser_x - button_size - 8.f - 10.f;

			this->m_drag_x = band_start;
			this->m_drag_y = y;
			this->m_drag_w = std::max(0.f, band_end - band_start);
			this->m_drag_h = h;
		}

		/* The FPS / memory / active-count readout used to fill the gap here. It was
		   diagnostics wearing chrome: three numbers nobody acts on, in the one strip
		   of the header the eye passes over on the way to the module list. */
		(void)active_modules;
		const float eraser_y = center_y - button_size * 0.5f;

		const bool eraser_hovered = input.hovered(eraser_x, eraser_y, button_size, button_size);
		this->m_eraser_hover = anim::lerp(this->m_eraser_hover, eraser_hovered ? 1.f : 0.f, 16.f, input.m_delta);

		if (eraser_hovered && input.m_left_clicked)
		{
			this->m_eraser_pulse.kick();

			if (this->m_wipe)
				this->m_wipe();
		}

		if (this->m_eraser_hover > 0.01f)
		{
			draw::shadow(eraser_x, eraser_y, button_size, button_size, 6.f, 12.f * this->m_eraser_hover, 0.f, 1.f,
				theme::danger_red.alpha(static_cast<int>(35.f * this->m_eraser_hover)));
		}

		draw::rounded_rect(eraser_x, eraser_y, button_size, button_size, 6.f, theme::bg_element);
		draw::border(eraser_x, eraser_y, button_size, button_size, 6.f, 1.f, theme::border);

		if (const float wiped = this->m_eraser_pulse.update(input.m_delta); wiped > 0.01f)
		{
			// red, because this is the one control in the bar that destroys something
			draw::shadow(eraser_x, eraser_y, button_size, button_size, 6.f,
				6.f + 20.f * (1.f - wiped), 0.f, 0.f,
				theme::danger_red.alpha(static_cast<int>(190.f * wiped)));
		}

		/* A reset mark, not a rubber. The button stopped being "erase something" when
		   it started turning modules off as well as clearing their keys — what it
		   does now is put the category back to how it shipped, and that is a
		   different verb. */
		constexpr float eraser_icon_size = 16.f;
		draw::restart_glyph(
			eraser_x + (button_size - eraser_icon_size) * 0.5f,
			eraser_y + (button_size - eraser_icon_size) * 0.5f,
			eraser_icon_size, 1.7f,
			eraser_hovered ? theme::text_white : theme::text_secondary);

		/* 5. the player-list switch, immediately left of the reset button.

		   @it is a STATE, so it is drawn as one.

		   The reset button beside it fires and is done, so it only ever has a hover look. This
		   one is a latch — the thing it controls stays where it was put — and a latch that
		   looks identical whether it is on or off is a light switch with no position. So the
		   accent fills it while the list is up and drains out of it when it is not, and both
		   the fill and the glyph's colour are driven off one eased value rather than the
		   boolean. Nothing in this bar snaps.

		   The list's own entrance is not here: this button owns the state and the shell owns
		   the animation, because the card is drawn outside the panel entirely and the header
		   has no idea where it is. */
		const float people_x = eraser_x - button_size - 8.f;
		const float people_y = eraser_y;

		const bool people_hovered = input.hovered(people_x, people_y, button_size, button_size);
		this->m_people_hover = anim::lerp(this->m_people_hover, people_hovered ? 1.f : 0.f,
			16.f, input.m_delta);

		if (people_hovered && input.m_left_clicked)
		{
			this->m_players_on = !this->m_players_on;
			this->m_people_pulse.kick();
		}

		this->m_people_glow = anim::lerp(this->m_people_glow, this->m_players_on ? 1.f : 0.f,
			15.f, input.m_delta);

		const float glow = this->m_people_glow;

		if (this->m_people_hover > 0.01f || glow > 0.01f)
			draw::shadow(people_x, people_y, button_size, button_size, 6.f,
				12.f * std::max(this->m_people_hover, glow), 0.f, 1.f,
				theme::accent_violet.alpha(static_cast<int>(
					35.f * std::max(this->m_people_hover, glow))));

		/* The fill is the element colour lifted towards the accent, not a second colour swapped
		   in: a restyled theme has to carry this button with it. */
		draw::rounded_rect(people_x, people_y, button_size, button_size, 6.f,
			theme::bg_element.lerp(hue::c_color{ theme::accent_violet.r / 4 + 22,
				theme::accent_violet.g / 4 + 22, theme::accent_violet.b / 4 + 22, 255 }, glow));
		draw::border(people_x, people_y, button_size, button_size, 6.f, 1.f,
			theme::border.lerp(theme::accent_violet, glow * 0.85f));

		if (const float kicked = this->m_people_pulse.update(input.m_delta); kicked > 0.01f)
			draw::shadow(people_x, people_y, button_size, button_size, 6.f,
				6.f + 20.f * (1.f - kicked), 0.f, 0.f,
				theme::accent_violet.alpha(static_cast<int>(190.f * kicked)));

		constexpr float people_icon_size = 15.f;
		draw::icon("user",
			people_x + (button_size - people_icon_size) * 0.5f,
			people_y + (button_size - people_icon_size) * 0.5f,
			people_icon_size,
			theme::text_secondary
				.lerp(theme::text_white, this->m_people_hover)
				.lerp(theme::accent_violet, glow * 0.75f));
	}

	void c_header::render_metrics(float right_edge, float center_y, int active_modules, const frame_input_t& input)
	{
		const ImGuiIO& io = ImGui::GetIO();

		/* --- sampling ------------------------------------------------- */
		this->m_fps.update(io.Framerate, 90.f, 18.f, input.m_delta);
		this->m_active.update(static_cast<float>(active_modules), 200.f, 24.f, input.m_delta);

		this->m_sample_timer += input.m_delta;
		if (this->m_sample_timer >= 0.05f)
		{
			this->m_sample_timer = 0.f;

			this->m_fps_history[static_cast<std::size_t>(this->m_fps_cursor)] = io.Framerate;
			this->m_fps_cursor = (this->m_fps_cursor + 1) % k_history;

			if (this->m_fps_cursor == 0)
				this->m_history_ready = true;
		}

		this->m_memory_timer += input.m_delta;
		if (this->m_memory_timer >= 0.5f || this->m_memory <= 0.f)
		{
			this->m_memory_timer = 0.f;

			PROCESS_MEMORY_COUNTERS counters{};
			if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
				this->m_memory = static_cast<float>(counters.WorkingSetSize) / (1024.f * 1024.f);
		}

		/* --- layout ---------------------------------------------------- */
		struct cell_t
		{
			const char* m_label;
			std::string m_value;
			float m_width;
		};

		std::array<cell_t, 3> cells{
			cell_t{ "FPS", std::format("{}", static_cast<int>(std::lround(this->m_fps.m_value))), 0.f },
			cell_t{ "MEM", std::format("{} MB", static_cast<int>(std::lround(this->m_memory))), 0.f },
			cell_t{ "ACTIVE", std::format("{}", static_cast<int>(std::lround(this->m_active.m_value))), 0.f }
		};

		constexpr float label_size = 9.5f;
		constexpr float value_size = 12.f;
		constexpr float cell_padding = 13.f;
		constexpr float label_gap = 6.f;

		float total = 0.f;
		for (auto& cell : cells)
		{
			cell.m_width = cell_padding * 2.f + label_gap
				+ text_width(cell.m_label, font_face::regular, label_size)
				+ text_width(cell.m_value, font_face::bold, value_size);

			total += cell.m_width;
		}

		constexpr float strip_height = 28.f;
		constexpr float radius = 9.f;

		const float strip_width = total;
		const float strip_x = right_edge - strip_width;
		const float strip_y = center_y - strip_height * 0.5f;

		if (strip_width <= 0.f)
			return;

		draw::rounded_rect(strip_x, strip_y, strip_width, strip_height, radius, theme::bg_element);
		draw::border(strip_x, strip_y, strip_width, strip_height, radius, 1.f, theme::border);

		/* --- frame rate graph, sitting behind the numbers --------------- */
		{
			float peak = 1.f;
			const int samples = this->m_history_ready ? k_history : this->m_fps_cursor;

			for (int index = 0; index < samples; ++index)
				peak = std::max(peak, this->m_fps_history[static_cast<std::size_t>(index)]);

			if (samples > 1)
			{
				draw::push_clip(strip_x + 1.f, strip_y + 1.f, strip_width - 2.f, strip_height - 2.f);

				ImDrawList* draw_list = g_render->draw_list();
				const ImVec2 uv = draw_list->_Data->TexUvWhitePixel;

				const float bottom = strip_y + strip_height - 1.f;
				const float span = strip_height - 8.f;

				const ImU32 top_color = theme::accent_violet.alpha(46).transform();
				const ImU32 bottom_color = theme::accent_violet.alpha(0).transform();

				// the ring buffer is walked oldest first so the graph scrolls left
				const auto sample_at = [&](int index) {
					const int slot = this->m_history_ready
						? (this->m_fps_cursor + index) % k_history
						: index;

					return this->m_fps_history[static_cast<std::size_t>(slot)];
					};

				const int quads = samples - 1;
				draw_list->PrimReserve(quads * 6, quads * 4);

				for (int index = 0; index < quads; ++index)
				{
					const float x0 = strip_x + strip_width * (static_cast<float>(index) / static_cast<float>(samples - 1));
					const float x1 = strip_x + strip_width * (static_cast<float>(index + 1) / static_cast<float>(samples - 1));

					const float y0 = bottom - span * std::clamp(sample_at(index) / peak, 0.f, 1.f);
					const float y1 = bottom - span * std::clamp(sample_at(index + 1) / peak, 0.f, 1.f);

					const ImDrawIdx base = static_cast<ImDrawIdx>(draw_list->_VtxCurrentIdx);

					draw_list->PrimWriteVtx(ImVec2(x0, y0), uv, top_color);
					draw_list->PrimWriteVtx(ImVec2(x1, y1), uv, top_color);
					draw_list->PrimWriteVtx(ImVec2(x1, bottom), uv, bottom_color);
					draw_list->PrimWriteVtx(ImVec2(x0, bottom), uv, bottom_color);

					draw_list->PrimWriteIdx(base);
					draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
					draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
					draw_list->PrimWriteIdx(base);
					draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
					draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
				}

				draw::pop_clip();
			}
		}

		/* --- cells ------------------------------------------------------ */
		float cursor = strip_x;

		for (std::size_t index = 0; index < cells.size(); ++index)
		{
			const cell_t& cell = cells[index];

			if (index > 0)
			{
				draw::rounded_rect(cursor, strip_y + 7.f, 1.f, strip_height - 14.f, 0.5f,
					hue::c_color(255, 255, 255, 16));
			}

			const float label_x = cursor + cell_padding;

			draw_text(cell.m_label, label_x, center_y, font_face::regular, label_size,
				theme::text_muted, align_left | align_middle);

			draw_text(cell.m_value,
				label_x + text_width(cell.m_label, font_face::regular, label_size) + label_gap, center_y,
				font_face::bold, value_size, theme::text_white, align_left | align_middle);

			cursor += cell.m_width;
		}
	}
}
