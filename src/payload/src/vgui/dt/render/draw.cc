#include "../../includes.hh"

namespace dt::draw
{
	namespace
	{
		/* segments used by the fading hairline divider */
		constexpr int k_divider_segments = 64;

		constexpr float k_pi = 3.14159265358979323846f;
	}

	void rounded_rect(float x, float y, float w, float h, float radius, hue::c_color color)
	{
		if (w <= 0.f || h <= 0.f || color.a <= 0)
			return;

		g_render->draw_list()->AddRectFilled(
			ImVec2(x, y), ImVec2(x + w, y + h),
			color.transform(),
			radius,
			ImDrawFlags_RoundCornersAll
		);
	}

	void border(float x, float y, float w, float h, float radius, float thickness, hue::c_color color)
	{
		if (w <= 0.f || h <= 0.f || color.a <= 0)
			return;

		// the reference draws the outline centered on the outside edge of the shape
		const float inset = thickness * 0.5f;

		g_render->draw_list()->AddRect(
			ImVec2(x - inset, y - inset), ImVec2(x + w + inset, y + h + inset),
			color.transform(),
			radius + inset,
			ImDrawFlags_RoundCornersAll,
			thickness
		);
	}

	void shadow(float x, float y, float w, float h, float radius, float spread,
		float offset_x, float offset_y, hue::c_color color)
	{
		if (w <= 0.f || h <= 0.f || spread <= 0.f || color.a <= 0)
			return;

		g_render->draw_list()->AddShadowRect(
			ImVec2(x, y), ImVec2(x + w, y + h),
			color.transform(),
			spread,
			ImVec2(offset_x, offset_y),
			ImDrawFlags_ShadowCutOutShapeBackground | ImDrawFlags_RoundCornersAll,
			radius
		);
	}

	void circle(float center_x, float center_y, float radius, hue::c_color color)
	{
		if (radius <= 0.f || color.a <= 0)
			return;

		g_render->draw_list()->AddCircleFilled(ImVec2(center_x, center_y), radius, color.transform(), 0);
	}

	void gradient_line(float x, float y, float w, float thickness, hue::c_color color)
	{
		if (w <= 0.f || thickness <= 0.f || color.a <= 0)
			return;

		ImDrawList* draw_list = g_render->draw_list();

		const float top = y - thickness * 0.5f;
		const float bottom = top + thickness;
		const ImVec2 uv = draw_list->_Data->TexUvWhitePixel;

		const auto alpha_at = [&](float t) -> ImU32
			{
				const float curve = std::pow(std::sin(k_pi * std::clamp(t, 0.f, 1.f)), 1.8f);
				return hue::c_color(color.r, color.g, color.b, static_cast<int>(color.a * curve)).transform();
			};

		draw_list->PrimReserve(k_divider_segments * 6, (k_divider_segments + 1) * 2);

		const ImDrawIdx base = static_cast<ImDrawIdx>(draw_list->_VtxCurrentIdx);

		for (int step = 0; step <= k_divider_segments; ++step)
		{
			const float t = static_cast<float>(step) / static_cast<float>(k_divider_segments);
			const float position = x + w * t;
			const ImU32 tint = alpha_at(t);

			draw_list->PrimWriteVtx(ImVec2(position, top), uv, tint);
			draw_list->PrimWriteVtx(ImVec2(position, bottom), uv, tint);
		}

		for (int step = 0; step < k_divider_segments; ++step)
		{
			const ImDrawIdx index = static_cast<ImDrawIdx>(base + step * 2);

			draw_list->PrimWriteIdx(index);
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 1));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));

			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 1));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 3));
		}
	}

	void gradient_rect(float x, float y, float w, float h, float radius,
		hue::c_color from, hue::c_color to, bool vertical)
	{
		if (w <= 0.f || h <= 0.f || (from.a <= 0 && to.a <= 0))
			return;

		ImDrawList* draw_list = g_render->draw_list();

		const int first = draw_list->VtxBuffer.Size;

		/* Laid down opaque white, NOT in `from`. Every vertex is recoloured below,
		   so the fill colour only decides whether the shape is emitted at all — and
		   AddRectFilled discards a fully transparent one outright, which is exactly
		   the case a fade-in-from-nothing gradient needs. */
		draw_list->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
			IM_COL32_WHITE, radius, ImDrawFlags_RoundCornersAll);

		const int last = draw_list->VtxBuffer.Size;
		if (last <= first)
			return;

		/* @why this does not use set_linear_alpha.

		   That helper OVERWRITES each vertex colour, alpha included, with a value
		   computed purely from position. A rounded, anti-aliased fill carries a
		   fringe of vertices whose whole job is to be partly transparent — that is
		   what the smooth edge IS — and overwriting their alpha with the gradient's
		   makes them fully opaque. The corners then come out as visible stair-steps,
		   which is exactly the pixelated edge the colour picker's square had.

		   So the gradient is MULTIPLIED into what the fill already laid down: the
		   RGB is replaced, the existing alpha is scaled. The shape keeps its own
		   edge and only its colour changes. */
		const float from_a = static_cast<float>(std::clamp(from.a, 0, 255)) / 255.f;
		const float to_a = static_cast<float>(std::clamp(to.a, 0, 255)) / 255.f;

		const ImVec2 axis = vertical ? ImVec2(0.f, h) : ImVec2(w, 0.f);
		const float length_sq = std::max(1.f, axis.x * axis.x + axis.y * axis.y);

		for (int index = first; index < last; ++index)
		{
			ImDrawVert& vertex = draw_list->VtxBuffer[index];

			const float dx = vertex.pos.x - x;
			const float dy = vertex.pos.y - y;

			const float t = std::clamp((dx * axis.x + dy * axis.y) / length_sq, 0.f, 1.f);

			const int r = static_cast<int>(from.r + (to.r - from.r) * t);
			const int g = static_cast<int>(from.g + (to.g - from.g) * t);
			const int b = static_cast<int>(from.b + (to.b - from.b) * t);

			/* the fill's own alpha is the coverage; the gradient's is an opacity on
			   top of it, so they multiply rather than one replacing the other */
			const float coverage = static_cast<float>((vertex.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
			const int a = static_cast<int>(255.f * coverage * (from_a + (to_a - from_a) * t));

			vertex.col = hue::c_color(r, g, b, a).transform();
		}
	}

	void glow(float center_x, float center_y, float radius, float spread, hue::c_color color)
	{
		if (radius <= 0.f || spread <= 0.f || color.a <= 0)
			return;

		g_render->draw_list()->AddShadowCircle(
			ImVec2(center_x, center_y), radius, color.transform(), spread, ImVec2(0.f, 0.f),
			ImDrawFlags_None, 0);
	}

	void spectrum_capsule(float x, float y, float w, float h, float alpha)
	{
		if (w <= 1.f || h <= 0.f || alpha <= 0.f)
			return;

		ImDrawList* draw_list = g_render->draw_list();

		/* @why columns and not a gradient:
			the hue rail is six stops, not two, so the vertex-recolouring trick
			gradient_rect() uses does not apply — a rounded fill only has vertices
			on its perimeter, and the interior of a wide rail would interpolate
			between corners rather than through the spectrum.

			So the rail is drawn as one-pixel columns instead, each carrying its own
			hue. The rounded ends come from insetting each column's top and bottom
			onto the cap circle, which is what keeps this a capsule rather than the
			square-ended block a clipped rainbow would be.
		*/
		/* the six-stop wheel, walked directly rather than through hsv */
		static constexpr int stops[7][3] = {
			{ 255,   0,   0 }, { 255, 255,   0 }, {   0, 255,   0 },
			{   0, 255, 255 }, {   0,   0, 255 }, { 255,   0, 255 },
			{ 255,   0,   0 },
		};

		const int columns = std::max(2, static_cast<int>(std::ceil(w)));
		const float step = w / static_cast<float>(columns);
		const float cap = std::min(h * 0.5f, w * 0.5f);

		const ImVec2 uv = draw_list->_Data->TexUvWhitePixel;

		draw_list->PrimReserve(columns * 6, columns * 4);

		for (int index = 0; index < columns; ++index)
		{
			const float left = x + step * static_cast<float>(index);
			const float right = left + step;
			const float centre = (left + right) * 0.5f;

			/* how far this column sits inside the cap circle, if at all */
			float inset = 0.f;

			const float from_left = centre - (x + cap);
			const float from_right = (x + w - cap) - centre;

			const float overhang = std::min(from_left, from_right);

			if (overhang < 0.f)
			{
				const float reach = std::min(cap, -overhang);
				inset = cap - std::sqrt(std::max(0.f, cap * cap - reach * reach));
			}

			const float t = (centre - x) / w;

			const float sector = std::clamp(t, 0.f, 1.f) * 6.f;
			const int   slot = std::min(5, static_cast<int>(sector));
			const float fade = sector - static_cast<float>(slot);

			const hue::c_color tint(
				static_cast<int>(stops[slot][0] + (stops[slot + 1][0] - stops[slot][0]) * fade),
				static_cast<int>(stops[slot][1] + (stops[slot + 1][1] - stops[slot][1]) * fade),
				static_cast<int>(stops[slot][2] + (stops[slot + 1][2] - stops[slot][2]) * fade),
				static_cast<int>(255.f * std::clamp(alpha, 0.f, 1.f)));

			const ImU32 packed = tint.transform();

			const ImDrawIdx base = static_cast<ImDrawIdx>(draw_list->_VtxCurrentIdx);

			draw_list->PrimWriteVtx(ImVec2(left, y + inset), uv, packed);
			draw_list->PrimWriteVtx(ImVec2(right, y + inset), uv, packed);
			draw_list->PrimWriteVtx(ImVec2(right, y + h - inset), uv, packed);
			draw_list->PrimWriteVtx(ImVec2(left, y + h - inset), uv, packed);

			draw_list->PrimWriteIdx(base);
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
			draw_list->PrimWriteIdx(base);
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
			draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
		}
	}

	void chevron(float center_x, float center_y, float size, int direction, float thickness, hue::c_color color)
	{
		if (size <= 0.f || color.a <= 0)
			return;

		const float half = size * 0.5f;

		// direction < 0 points left, direction >= 0 points right
		const float tip = (direction >= 0) ? half * 0.5f : -half * 0.5f;

		ImDrawList* draw_list = g_render->draw_list();

		draw_list->PathClear();
		draw_list->PathLineTo(ImVec2(center_x - tip, center_y - half));
		draw_list->PathLineTo(ImVec2(center_x + tip, center_y));
		draw_list->PathLineTo(ImVec2(center_x - tip, center_y + half));
		draw_list->PathStroke(color.transform(), ImDrawFlags_None, thickness);
	}

	void disclosure_arrow(float center_x, float center_y, float size, float facing,
		float thickness, hue::c_color color)
	{
		if (size <= 0.f || color.a <= 0)
			return;

		facing = std::clamp(facing, -1.f, 1.f);

		/* @a chevron, built as one closed shape.

		   The three versions before this were all the same mistake: trying to make a
		   SOLID triangle look like a disclosure mark. At nine pixels a filled wedge
		   is a squat blob with a heavy base — it reads as a thick dash however the
		   proportions are tuned. The mark that survives this size is a stroked
		   chevron, which is what every interface that has solved the problem uses.

		   It is ONE concave polygon rather than two quads and three discs. Those
		   would look identical while the colour is opaque and seam wherever they
		   overlap once it is not — and this is drawn at whatever alpha the card is
		   fading through. A single fill cannot seam.

		   The ends are rounded because ImGui strokes with square caps, and a square
		   cap on a one-pixel arm is a visible notch at this size.
		*/
		const float half = size * 0.38f;
		const float reach = size * 0.21f;
		const float band = size * (0.115f + 0.030f * thickness);

		const float half_band = band * 0.5f;

		/* Only ever the up chevron or its exact mirror, with the direction snapping
		   at the midpoint — one caught at forty degrees is not a chevron, so nothing
		   here rotates. What animates is a squash, which never reaches zero and so
		   never passes through a degenerate frame. */
		const float squash = 0.55f + 0.45f * std::fabs(facing);
		const float rise = reach * squash;

		const float run = std::sqrt(half * half + 4.f * rise * rise);
		if (run < 0.001f || half < 0.001f)
			return;

		/* how far past the centreline the outer corner sits where two arms of this
		   width meet at this angle */
		const float mitre = half_band * run / half;

		/* the right arm's outward normal, pointing up and to the right */
		const float nx = (2.f * rise) / run;
		const float ny = -half / run;

		const float apex_y = center_y - rise;
		const float end_y = center_y + rise;
		const float end_x = center_x + half;

		/* @half the outline, then reflected.

		   Symmetry by construction rather than by algebra. Deriving the left arm's
		   normals separately is the same arithmetic done twice with the signs
		   flipped, and getting one of them wrong produces a self-intersecting
		   outline that a concave fill turns into a mess. */
		constexpr int k_arc = 5;
		constexpr int k_right = 3 + k_arc;          /* apex, end, arc, end, apex */

		std::array<ImVec2, k_right> right{};

		right[0] = ImVec2(center_x, apex_y - mitre);
		right[1] = ImVec2(end_x + nx * half_band, end_y + ny * half_band);

		const float from = std::atan2(ny, nx);

		for (int step = 1; step <= k_arc; ++step)
		{
			const float angle = from + 3.14159265358979323846f * (static_cast<float>(step) / (k_arc + 1.f));

			right[static_cast<std::size_t>(1 + step)] = ImVec2(
				end_x + std::cos(angle) * half_band,
				end_y + std::sin(angle) * half_band);
		}

		right[k_right - 1] = ImVec2(center_x, apex_y + mitre);

		std::array<ImVec2, k_right * 2 - 2> outline{};

		int count = 0;

		for (const ImVec2& point : right)
			outline[static_cast<std::size_t>(count++)] = point;

		/* everything between the two apex points, mirrored back down the left side */
		for (int index = k_right - 2; index >= 1; --index)
		{
			outline[static_cast<std::size_t>(count++)] = ImVec2(
				2.f * center_x - right[static_cast<std::size_t>(index)].x,
				right[static_cast<std::size_t>(index)].y);
		}

		ImDrawList* draw_list = g_render->draw_list();

		draw_list->PathClear();

		if (facing >= 0.f)
		{
			/* pointing down: reflected in y, and walked backwards so the winding
			   survives the reflection */
			for (int index = count - 1; index >= 0; --index)
			{
				draw_list->PathLineTo(ImVec2(
					outline[static_cast<std::size_t>(index)].x,
					2.f * center_y - outline[static_cast<std::size_t>(index)].y));
			}
		}
		else
		{
			for (int index = 0; index < count; ++index)
				draw_list->PathLineTo(outline[static_cast<std::size_t>(index)]);
		}

		draw_list->PathFillConcave(color.transform());
	}

	void restart_glyph(float x, float y, float size, float thickness, hue::c_color color)
	{
		if (size <= 0.f || color.a <= 0)
			return;

		ImDrawList* draw_list = g_render->draw_list();

		const float cx = x + size * 0.5f;
		const float cy = y + size * 0.5f;

		const float radius = size * 0.34f;

		/* @an arc with a bite taken out of it, and a head on the loose end.

		   Left open at the top rather than closed into a ring: a closed circle with
		   an arrowhead on it reads as a clock or a target, and the gap is the whole
		   reason the eye follows the stroke round instead of taking it in at once.

		   Drawn rather than shipped as an SVG because the rasteriser here has no
		   arc command — only lines and béziers — and approximating a circle in
		   cubics to draw a circle is a poor trade when the draw list has arcs. */
		draw_list->PathClear();
		draw_list->PathArcTo(ImVec2(cx, cy), radius, -0.55f, 4.35f, 22);
		draw_list->PathStroke(color.transform(), ImDrawFlags_None, thickness);

		/* the head, on the end the stroke stops at */
		const float head = size * 0.19f;

		const float tip_x = cx + radius * std::cos(-0.55f);
		const float tip_y = cy + radius * std::sin(-0.55f);

		draw_list->PathClear();
		draw_list->PathLineTo(ImVec2(tip_x - head * 0.15f, tip_y - head));
		draw_list->PathLineTo(ImVec2(tip_x + head * 0.95f, tip_y + head * 0.15f));
		draw_list->PathLineTo(ImVec2(tip_x - head * 0.55f, tip_y + head * 0.7f));
		draw_list->PathFillConvex(color.transform());
	}

	void power_glyph(float x, float y, float size, float thickness, hue::c_color color)
	{
		if (size <= 0.f || color.a <= 0)
			return;

		ImDrawList* draw_list = g_render->draw_list();

		const float cx = x + size * 0.5f;
		const float cy = y + size * 0.55f;

		const float radius = size * 0.36f;

		/* The gap at the top is the whole mark. A closed ring with a line through
		   it is a plug symbol; it is the BREAK that the vertical stroke rises out
		   of which makes this one read as power. */
		draw_list->PathClear();
		draw_list->PathArcTo(ImVec2(cx, cy), radius, -0.95f, 3.14159265f + 0.95f, 24);
		draw_list->PathStroke(color.transform(), ImDrawFlags_None, thickness);

		draw_list->PathClear();
		draw_list->PathLineTo(ImVec2(cx, y + size * 0.10f));
		draw_list->PathLineTo(ImVec2(cx, cy - radius * 0.35f));
		draw_list->PathStroke(color.transform(), ImDrawFlags_None, thickness);
	}

	void copy_glyph(float x, float y, float size, float thickness, hue::c_color color)
	{
		if (size <= 0.f || color.a <= 0)
			return;

		ImDrawList* draw_list = g_render->draw_list();

		/* two sheets, the back one peeking out top-right of the front one */
		const float sheet = size * 0.72f;
		const float offset = size - sheet;
		const float rounding = size * 0.16f;

		draw_list->AddRect(
			ImVec2(x + offset, y), ImVec2(x + offset + sheet, y + sheet),
			color.modulate(0.55f).transform(), rounding, ImDrawFlags_RoundCornersAll, thickness);

		draw_list->AddRect(
			ImVec2(x, y + offset), ImVec2(x + sheet, y + offset + sheet),
			color.transform(), rounding, ImDrawFlags_RoundCornersAll, thickness);
	}

	void check_glyph(float x, float y, float size, float thickness, hue::c_color color)
	{
		if (size <= 0.f || color.a <= 0)
			return;

		ImDrawList* draw_list = g_render->draw_list();

		draw_list->PathClear();
		draw_list->PathLineTo(ImVec2(x + size * 0.12f, y + size * 0.54f));
		draw_list->PathLineTo(ImVec2(x + size * 0.40f, y + size * 0.80f));
		draw_list->PathLineTo(ImVec2(x + size * 0.88f, y + size * 0.22f));
		draw_list->PathStroke(color.transform(), ImDrawFlags_None, thickness);
	}

	void texture(ImTextureID id, float x, float y, float w, float h, float radius, hue::c_color color)
	{
		if (id == 0 || w <= 0.f || h <= 0.f || color.a <= 0)
			return;

		g_render->image(
			static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
			static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h)),
			id, color, radius
		);
	}

	void icon(const std::string& name, float x, float y, float size, hue::c_color color)
	{
		rounded_icon(name, x, y, size, 0.f, color);
	}

	void rounded_icon(const std::string& name, float x, float y, float size, float radius, hue::c_color color)
	{
		// the icon is baked at the exact pixel size it lands on, and snapped to the pixel grid,
		// so every texel maps one to one and nothing has to be filtered down
		const int pixels = std::max(1, static_cast<int>(std::lround(size)));
		const float slack = (size - static_cast<float>(pixels)) * 0.5f;

		const ImTextureID id = g_icons->get(name, pixels);
		if (id == 0)
			return;

		texture(id, x + slack, y + slack, static_cast<float>(pixels), static_cast<float>(pixels), radius, color);
	}

	void push_clip(float x, float y, float w, float h)
	{
		g_render->push_clip(
			static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
			static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))
		);
	}

	void pop_clip()
	{
		g_render->restore_clip();
	}

	namespace
	{
		/* @trace_bubble:
			lays a rounded rectangle path down with an independent radius on every corner
		*/
		void trace_bubble(ImDrawList* draw_list, float x, float y, float w, float h,
			float top_left, float top_right, float bottom_right, float bottom_left)
		{
			const float limit = std::min(w, h) * 0.5f;

			top_left = std::clamp(top_left, 0.f, limit);
			top_right = std::clamp(top_right, 0.f, limit);
			bottom_right = std::clamp(bottom_right, 0.f, limit);
			bottom_left = std::clamp(bottom_left, 0.f, limit);

			draw_list->PathClear();
			draw_list->PathArcToFast(ImVec2(x + top_left, y + top_left), top_left, 6, 9);
			draw_list->PathArcToFast(ImVec2(x + w - top_right, y + top_right), top_right, 9, 12);
			draw_list->PathArcToFast(ImVec2(x + w - bottom_right, y + h - bottom_right), bottom_right, 0, 3);
			draw_list->PathArcToFast(ImVec2(x + bottom_left, y + h - bottom_left), bottom_left, 3, 6);
		}
	}

	void bubble(float x, float y, float w, float h,
		float top_left, float top_right, float bottom_right, float bottom_left, hue::c_color color)
	{
		if (w <= 0.f || h <= 0.f || color.a <= 0)
			return;

		ImDrawList* draw_list = g_render->draw_list();

		trace_bubble(draw_list, x, y, w, h, top_left, top_right, bottom_right, bottom_left);
		draw_list->PathFillConvex(color.transform());
	}

	void bubble_border(float x, float y, float w, float h,
		float top_left, float top_right, float bottom_right, float bottom_left,
		float thickness, hue::c_color color)
	{
		if (w <= 0.f || h <= 0.f || color.a <= 0)
			return;

		ImDrawList* draw_list = g_render->draw_list();
		const float inset = thickness * 0.5f;

		trace_bubble(draw_list, x - inset, y - inset, w + thickness, h + thickness,
			top_left + inset, top_right + inset, bottom_right + inset, bottom_left + inset);

		draw_list->PathStroke(color.transform(), ImDrawFlags_Closed, thickness);
	}
}
