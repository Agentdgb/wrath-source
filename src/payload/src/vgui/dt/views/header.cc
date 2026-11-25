#include "../../includes.hh"

namespace dt
{
	void c_header::render(const std::string& current_category, int active_modules,
		float x, float y, float w, float h, const frame_input_t& input)
	{
		(void)current_category;

		const float start_x = x + 24.f;
		const float right_x = x + w - 24.f;

		/* 1. separator hairline, drawn first so everything else sits on top of it */
		draw::gradient_line(start_x, y + h, right_x - start_x, 1.2f, theme::border.lerp(theme::accent_violet, 0.45f).alpha(110));

		/* The space across the entire header can drag the window. */
		{
			this->m_drag_x = start_x;
			this->m_drag_y = y;
			this->m_drag_w = std::max(0.f, right_x - start_x);
			this->m_drag_h = h;
		}
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
