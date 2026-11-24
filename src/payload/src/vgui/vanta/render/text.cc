#include "../../includes.hh"

/* @alpha_text:
	the vendored draw list multiplies every glyph by this global. the client keeps it fully opaque
	and drives per element opacity through the text color instead.
*/
float alpha_text = 1.f;

namespace vanta
{
	namespace
	{
		/* every size the gui draws at. the atlas is immutable once built, so this list is the contract */
		constexpr float k_sizes[]{ 9.5f, 10.f, 10.5f, 11.f, 11.5f, 12.f, 12.5f, 13.f, 13.5f, 14.f, 14.5f, 15.f, 16.f };

		/* last resort slant, only used when the machine has no handwriting face at all.
		   ~12 degrees, which is what a real oblique cut uses. */
		constexpr float k_oblique_slant = 0.213f;

		/* @find_script_face:
			joined handwriting faces, best first. segoe script is the one that actually looks like
			a written hand at ui sizes; gabriola and ink free are there so odd installs still get
			something rather than falling back to a slanted roman.
		*/
		std::optional<std::string> find_script_face()
		{
			constexpr const char* candidates[]{
				"C:/Windows/Fonts/segoescb.ttf", // segoe script bold
				"C:/Windows/Fonts/segoesc.ttf",  // segoe script
				"C:/Windows/Fonts/Gabriola.ttf",
				"C:/Windows/Fonts/Inkfree.ttf"
			};

			for (const char* candidate : candidates)
			{
				if (std::ifstream(candidate).good())
					return std::string(candidate);
			}

			return std::nullopt;
		}

		/* @find_italic_face:
			true italics, best first — the test that matters is a single-storey 'a'.
			Segoe UI and Arial only slope their roman, so they are last resorts.
		*/
		std::optional<std::string> find_italic_face()
		{
			constexpr const char* candidates[]{
				"C:/Windows/Fonts/corbelz.ttf",   // Corbel Bold Italic
				"C:/Windows/Fonts/calibriz.ttf",  // Calibri Bold Italic
				"C:/Windows/Fonts/candaraz.ttf",  // Candara Bold Italic
				"C:/Windows/Fonts/segoeuiz.ttf"   // Segoe UI Bold Italic (sloped roman)
			};

			for (const char* candidate : candidates)
			{
				if (std::ifstream(candidate).good())
					return std::string(candidate);
			}

			return std::nullopt;
		}

		std::string resolve_face_path(font_face face)
		{
			return core::asset_path(face == font_face::bold ? "fonts/proximabd.ttf" : "fonts/proxima.ttf");
		}

		/* @canvas_scale:
			glyph metrics are in atlas pixels (font->FontSize). the layout code speaks CANVAS
			pixels (the nominal size the caller asked for). converting one to the other is
			always `nominal / font->FontSize`, so keep it in a helper and don't hand-inline it.
		*/
		float canvas_scale(ImFont* font, float nominal)
		{
			if (font == nullptr || font->FontSize <= 0.f)
				return 1.f;
			return nominal / font->FontSize;
		}

		/* @ink_centre:
			offset from the start of a run to the middle of the ink it puts on screen. returned in
			CANVAS pixels — glyph metrics are in atlas units so we scale by nominal/FontSize.
		*/
		float ink_centre(ImFont* font, const std::string& text, float nominal)
		{
			float pen = 0.f;
			float minimum = FLT_MAX;
			float maximum = -FLT_MAX;

			const char* cursor = text.c_str();
			const char* end = cursor + text.size();

			while (cursor < end)
			{
				unsigned int codepoint = 0;
				const int consumed = ImTextCharFromUtf8(&codepoint, cursor, end);

				if (consumed == 0)
					break;

				cursor += consumed;

				const ImFontGlyph* glyph = font->FindGlyph(static_cast<ImWchar>(codepoint));
				if (glyph == nullptr)
					continue;

				minimum = std::min(minimum, pen + glyph->X0);
				maximum = std::max(maximum, pen + glyph->X1);

				pen += glyph->AdvanceX;
			}

			const float s = canvas_scale(font, nominal);

			// nothing with ink in it, fall back to the advance box
			if (maximum < minimum)
				return pen * 0.5f * s;

			return (minimum + maximum) * 0.5f * s;
		}

		/* @measure_at:
			text advance width in CANVAS pixels for a font baked at atlas resolution, sized
			back down to `nominal` at render time.
		*/
		float measure_at(ImFont* font, float nominal, const std::string& text)
		{
			if (font == nullptr || text.empty())
				return 0.f;

			return font->CalcTextSizeA(nominal, FLT_MAX, -1.f, text.c_str()).x;
		}
	}

	/* @cap_centre_of:
		how far below the top of a line the optical centre of the glyphs actually sits, in
		ATLAS pixels. multiply by canvas_scale for the equivalent in canvas pixels.

		guessing this from the point size is what leaves text riding high inside a bubble, so it
		is measured off the font: the cap band of an 'H' is exactly the mass the eye centres on.
		cached per handle, FindGlyph is a lookup we do not want inside a draw call. the cache
		lives on the registry so we can drop it when the atlas is rebuilt (which invalidates every
		ImFont* pointer).
	*/
	float c_font_registry::cap_centre_of(ImFont* font)
	{
		if (font == nullptr)
			return 0.f;

		if (const auto hit = this->m_cap_cache.find(font); hit != this->m_cap_cache.end())
			return hit->second;

		float offset = font->FontSize * 0.5f;

		if (const ImFontGlyph* glyph = font->FindGlyph(static_cast<ImWchar>('H')); glyph != nullptr)
			offset = (glyph->Y0 + glyph->Y1) * 0.5f;

		this->m_cap_cache[font] = offset;
		return offset;
	}

	void c_font_registry::clear_cap_cache()
	{
		this->m_cap_cache.clear();
	}

	void c_font_registry::bake_all(float atlas_scale)
	{
		ImFontConfig configuration{};
		configuration.OversampleH = 2;
		configuration.OversampleV = 1;
		configuration.PixelSnapH = false;
		configuration.RasterizerMultiply = 1.05f; // a touch more presence at 10px, the ui is dark

		// latin plus the punctuation the gui actually types, keeps the atlas small
		static constexpr ImWchar ranges[]{
			0x0020, 0x00FF,
			0x2018, 0x201F,
			0x2039, 0x203A,
			0x2022, 0x2022,
			0
		};

		const auto script_path = find_script_face();
		this->m_script_synthesized = !script_path.has_value();

		for (auto& face_bakes : this->m_faces)
			face_bakes.clear();

		this->m_cap_cache.clear();
		this->m_atlas_scale = atlas_scale;

		const auto bake_face = [&](font_face face, const std::string& path)
			{
				auto& baked = this->m_faces[static_cast<std::size_t>(face)];
				baked.reserve(std::size(k_sizes));

				const auto embedded = core::find_embedded_asset(path);

				for (const float size : k_sizes)
				{
					baked_t entry{};
					entry.m_size = size;

					const float atlas_pixels = std::max(1.f, std::round(size * atlas_scale));

					bool ok = false;
					if (embedded.valid())
					{
						ImFontConfig mem_cfg = configuration;
						mem_cfg.FontDataOwnedByAtlas = false;
						ok = entry.m_font.create(
							const_cast<std::uint8_t*>(embedded.data),
							static_cast<int>(embedded.size),
							atlas_pixels, &mem_cfg, ranges, path, false);
					}
					else
					{
						ok = entry.m_font.create(path, atlas_pixels, &configuration, ranges);
					}

					if (!ok)
						slog::log::error("fonts: failed to bake {} at {} ({}px atlas)", path, size, atlas_pixels);

					baked.push_back(std::move(entry));
				}
			};

		bake_face(font_face::regular, resolve_face_path(font_face::regular));
		bake_face(font_face::bold, resolve_face_path(font_face::bold));

		if (script_path.has_value())
			bake_face(font_face::script, *script_path);

		const auto italic_path = find_italic_face();
		this->m_italic_synthesized = !italic_path.has_value();

		bake_face(font_face::italic,
			italic_path.value_or(resolve_face_path(font_face::bold)));

		g_font->build_fonts(ImGui::GetIO().Fonts);
	}

	void c_font_registry::initialize()
	{
		this->bake_all(1.f);

		if (this->m_script_synthesized)
			slog::log::warning("fonts: no handwriting face found, the wordmark falls back to a sheared bold");

		slog::log::success("typography baked, {} sizes per face @ 1.0x", static_cast<int>(std::size(k_sizes)));
	}

	bool c_font_registry::apply_pending_atlas_scale()
	{
		/* Every glyph that is drawn on a 2K/4K display is stretched by the panel-scale that
		   gl_hook applies at present time. If the atlas glyphs were baked for 1.0x and the
		   panel scales by 1.6x, the GPU upsamples 12px glyph textures into 20px screen quads
		   and the whole gui looks fuzzy. Rebaking the atlas at the panel scale lets the GPU
		   do a near-1:1 sample instead.

		   This MUST run outside the NewFrame → Render window. NewFrame sets Fonts->Locked
		   and points g.Font at the default atlas font; touching Clear() past that point tears
		   the pointer out from under the frame we are already midway through. gl_hook wires
		   this to fire right before NewFrame. */
		const float desired = this->m_pending_scale;
		if (desired <= 0.f)
			return false;

		if (ImGui::GetIO().Fonts == nullptr || ImGui::GetIO().Fonts->Locked)
			return false;

		const float clamped = std::clamp(desired, 0.75f, 3.5f);

		// Snap to eighths — a 1.6 scale doesn't need a rebake for 1.61.
		const float snapped = std::round(clamped * 8.f) / 8.f;

		if (std::fabs(snapped - this->m_atlas_scale) < 0.001f)
			return false;

		/* Clear the atlas before rebuilding. AddFont* into a non-empty atlas keeps the old
		   glyphs alive AND appends the new ones, which drifts the memory usage upwards on
		   every scale change and (once large enough) makes the backend upload fail silently. */
		ImGui::GetIO().Fonts->Clear();

		this->bake_all(snapped);

		/* Force the GL backend to drop its cached atlas texture; without this the freshly
		   built atlas would keep sampling from the previous frame's smaller texture until
		   the backend happened to re-upload on its own. */
		ImGui_ImplOpenGL3_DestroyDeviceObjects();

		slog::log::info("typography rebaked at {:.2f}x atlas scale", snapped);
		return true;
	}

	c_font* c_font_registry::get(font_face face, float size)
	{
		// with no handwriting face installed the script slot is empty, borrow the bold atlas
		if (face == font_face::script && this->m_script_synthesized)
			face = font_face::bold;

		if (face == font_face::italic && this->m_faces[static_cast<std::size_t>(face)].empty())
			face = font_face::bold;

		auto& baked = this->m_faces[static_cast<std::size_t>(face)];
		if (baked.empty())
			return nullptr;

		baked_t* closest = &baked.front();
		float best = std::fabs(closest->m_size - size);

		for (auto& entry : baked)
		{
			const float distance = std::fabs(entry.m_size - size);
			if (distance < best)
			{
				best = distance;
				closest = &entry;
			}
		}

		return &closest->m_font;
	}

	float text_width(const std::string& text, font_face face, float size)
	{
		if (text.empty())
			return 0.f;

		c_font* font = g_fonts->get(face, size);
		if (font == nullptr || font->get_font() == nullptr)
			return static_cast<float>(text.size()) * size * 0.52f;

		return measure_at(font->get_font(), size, text);
	}

	float text_baseline(font_face face, float size, float center_y)
	{
		c_font* font = g_fonts->get(face, size);
		if (font == nullptr || font->get_font() == nullptr)
			return center_y;

		ImFont* handle = font->get_font();
		const float s = canvas_scale(handle, size);

		return center_y - g_fonts->cap_centre_of(handle) * s + handle->Ascent * s;
	}

	void draw_text(const std::string& text, float x, float y, font_face face, float size, hue::c_color color, int align)
	{
		if (text.empty() || color.a <= 0)
			return;

		c_font* font = g_fonts->get(face, size);
		if (font == nullptr || font->get_font() == nullptr)
			return;

		ImFont* handle = font->get_font();
		const float s = canvas_scale(handle, size);

		float position_x = x;

		if ((align & align_center) != 0)
		{
			// anchored on the ink, not on the advance box, so a lone glyph sits dead centre
			position_x -= ink_centre(handle, text, size);
		}
		else if ((align & align_right) != 0)
		{
			position_x -= measure_at(handle, size, text);
		}

		float position_y = y;

		if ((align & align_baseline) != 0)
		{
			position_y = y - handle->Ascent * s;
		}
		else if ((align & align_middle) != 0)
		{
			// y is the optical centre of the line, measured from the font rather than assumed
			position_y = y - g_fonts->cap_centre_of(handle) * s;
		}

		/* both axes are rounded here, and that is not cosmetic.

		   ImFont::RenderText truncates the origin it is handed (IM_TRUNC), so a fractional x is
		   not honoured: it is floored, which drags the whole run up to a pixel to the left. Left to
		   itself that turns a perfectly computed centre into a visible offset. Rounding first keeps
		   the error symmetric and bounded at half a pixel. */
		position_x = std::round(position_x);

		const float snapped_y = std::round(position_y);

		ImDrawList* draw_list = g_render->draw_list();
		const int first_vertex = draw_list->VtxBuffer.Size;

		/* Explicit size on AddText — this is the fixed lever that lets the atlas live at a
		   different scale from the requested render size. RenderText scales every glyph quad
		   by (size / font->FontSize) and samples UVs from the atlas at whatever detail was
		   baked. Baking bigger and rendering at nominal is what stops the text going fuzzy
		   on 2K/4K displays. */
		ImGui::PushFont(handle);
		draw_list->AddText(handle, size, ImVec2(position_x, snapped_y), color.transform(), text.c_str());
		ImGui::PopFont();

		const bool shear_script = (face == font_face::script) && g_fonts->script_is_synthesized();
		const bool shear_italic = (face == font_face::italic) && g_fonts->italic_is_synthesized();

		if (!shear_script && !shear_italic)
			return;

		/* no handwriting face on this machine: shear the bold glyphs around their baseline so the
		   feet stay put and the shoulders lean forward, the way a drawn oblique does */
		const float baseline = snapped_y + handle->Ascent * s;

		for (int index = first_vertex; index < draw_list->VtxBuffer.Size; ++index)
		{
			ImDrawVert& vertex = draw_list->VtxBuffer[index];
			vertex.pos.x += (baseline - vertex.pos.y) * k_oblique_slant;
		}
	}

	void draw_slanted_text(const std::string& text, float x, float y, font_face face, float size,
		hue::c_color color, int align, float slant)
	{
		if (text.empty() || color.a <= 0)
			return;

		c_font* font = g_fonts->get(face, size);
		if (font == nullptr || font->get_font() == nullptr)
			return;

		ImDrawList* draw_list = g_render->draw_list();

		const int first = draw_list->VtxBuffer.Size;

		draw_text(text, x, y, face, size, color, align);

		if (slant == 0.f)
			return;

		/* the baseline the run was actually set on, which is what the shear has to
		   pivot around */
		ImFont* handle = font->get_font();
		const float scale = size / handle->FontSize;

		const float baseline = ((align & align_baseline) != 0)
			? y
			: (((align & align_middle) != 0)
				? y - g_fonts->cap_centre_of(handle) * scale + handle->Ascent * scale
				: y + handle->Ascent * scale);

		for (int index = first; index < draw_list->VtxBuffer.Size; ++index)
		{
			ImDrawVert& vertex = draw_list->VtxBuffer[index];
			vertex.pos.x += (baseline - vertex.pos.y) * slant;
		}
	}

	std::string truncate_text(const std::string& text, float max_width, font_face face, float size)
	{
		if (text.empty())
			return {};

		if (text_width(text, face, size) <= max_width)
			return text;

		const std::string ellipsis = "...";
		const float available = max_width - text_width(ellipsis, face, size);

		if (available <= 0.f)
			return ellipsis;

		for (std::size_t length = text.size(); length > 0; --length)
		{
			const std::string candidate = text.substr(0, length - 1);
			if (text_width(candidate, face, size) <= available)
				return candidate + ellipsis;
		}

		return ellipsis;
	}

	/* ------------------------------------------------------------------ */
	/* rich text                                                           */
	/* ------------------------------------------------------------------ */

	namespace
	{
		/* emoji read small when set at the surrounding text size, so they are given a little more */
		constexpr float k_emoji_scale = 1.2f;
	}

	rich_text_t layout_rich_text(const std::string& text, font_face face, float size, float max_width)
	{
		rich_text_t layout{};
		layout.m_line_height = std::max(size * 1.5f, size + 6.f);

		const float emoji_size = size * k_emoji_scale;

		rich_line_t current{};
		float cursor = 0.f;

		const auto break_line = [&]()
			{
				current.m_width = cursor;
				layout.m_width = std::max(layout.m_width, cursor);

				layout.m_lines.push_back(std::move(current));

				current = rich_line_t{};
				cursor = 0.f;
			};

		const auto place = [&](std::string content, bool is_emoji, float width)
			{
				// a token that no longer fits starts the next line, unless it is alone and too wide
				if (cursor > 0.f && cursor + width > max_width)
					break_line();

				rich_run_t run{};
				run.m_text = std::move(content);
				run.m_emoji = is_emoji;
				run.m_x = cursor;
				run.m_width = width;

				current.m_runs.push_back(std::move(run));
				cursor += width;
			};

		for (const auto& run : emoji::segment(text))
		{
			if (run.m_emoji)
			{
				place(run.m_text, true, emoji_size);
				continue;
			}

			std::size_t index = 0;
			while (index < run.m_text.size())
			{
				if (run.m_text[index] == '\n')
				{
					break_line();
					++index;
					continue;
				}

				std::size_t end = index;
				while (end < run.m_text.size() && run.m_text[end] != ' ' && run.m_text[end] != '\n')
					++end;

				// trailing spaces ride with the word they follow, so a wrap never orphans them
				while (end < run.m_text.size() && run.m_text[end] == ' ')
					++end;

				std::string token = run.m_text.substr(index, end - index);
				place(token, false, text_width(token, face, size));

				index = end;
			}
		}

		break_line();

		layout.m_height = static_cast<float>(layout.m_lines.size()) * layout.m_line_height;
		return layout;
	}

	void draw_rich_text(const rich_text_t& layout, float x, float y, font_face face, float size, hue::c_color color)
	{
		const float emoji_size = size * k_emoji_scale;
		const int emoji_pixels = std::max(1, static_cast<int>(std::lround(emoji_size)));

		float line_y = y;

		for (const auto& line : layout.m_lines)
		{
			for (const auto& run : line.m_runs)
			{
				if (run.m_emoji)
				{
					const ImTextureID id = emoji::texture(run.m_text, emoji_pixels);
					if (id == 0)
						continue;

					// emoji carry their own colour, only the opacity of the message applies
					draw::texture(id, x + run.m_x, line_y + (layout.m_line_height - emoji_size) * 0.5f,
						emoji_size, emoji_size, 0.f, hue::c_color(255, 255, 255, color.a));

					continue;
				}

				draw_text(run.m_text, x + run.m_x, line_y + layout.m_line_height * 0.5f,
					face, size, color, align_left | align_middle);
			}

			line_y += layout.m_line_height;
		}
	}
}
