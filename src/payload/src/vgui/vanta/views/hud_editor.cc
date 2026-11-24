#include "modules/render/keybindhud/keybindhud.h"
#include "../../includes.hh"

#include "hud_editor.hh"
#include "../../../modules/render/arraylist/font_pool.h"
#include "../../../modules/render/arraylist/arraylist.h"
#include "../../../modules/render/watermark/watermark.h"
#include "../../../modules/render/targethud/targethud.h"
#include "../../../modules/render/radar/radar.h"
#include "../../../modules/render/notifier/notifier.h"
#include "../../../modules/render/spotify/spotify.h"

#include "core/config/ConfigManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

/* ────────────────────────────────────────────────────────────────────────────
   hud_editor.cc — minimal fullscreen mover for HUD elements.

   The editor renders while the sidebar's "HUD" entry is selected and the
   panel is visible. It draws:
     * a soft grid across the whole framebuffer,
     * a small "Exit" pill top-left that returns to the previous category,
     * hover/drag handles around every HUD element that publishes a bbox
       (today: the ArrayList, the Watermark and the Target HUD).

   No dock, no popover, no per-element settings — the element's own module
   drawer in the ClickGui owns those. This surface exists solely to let the
   user physically move the HUD around on screen.

   Everything renders on ImGui's foreground draw list (gl_hook exempts that
   list from the panel-scale transform), so coordinates are framebuffer
   pixels.
   ──────────────────────────────────────────────────────────────────────── */

namespace vanta::hud_editor
{
	namespace
	{
		/* ── Palette ────────────────────────────────────────────────────── */
		constexpr ImU32 col_card        = IM_COL32(  26,  26,  30, 255);
		constexpr ImU32 col_card_hi     = IM_COL32(  36,  36,  42, 255);
		constexpr ImU32 col_border      = IM_COL32(  52,  52,  62, 220);
		constexpr ImU32 col_text        = IM_COL32( 242, 242, 246, 255);
		constexpr ImU32 col_accent      = IM_COL32( 140, 165, 255, 255);
		constexpr ImU32 col_accent_soft = IM_COL32( 140, 165, 255, 100);
		constexpr ImU32 col_danger      = IM_COL32( 255, 120, 120, 255);
		constexpr ImU32 col_shadow      = IM_COL32(   0,   0,   0, 140);

		/* ── State ──────────────────────────────────────────────────────── */
		bool  g_active       = false;
		float g_active_anim  = 0.f;

		std::function<void()> g_exit_cb{};

		float g_hover_exit = 0.f;

		int      g_dragging_index = -1;
		float    g_drag_grab_x    = 0.f;
		float    g_drag_grab_y    = 0.f;

		bool  g_left_prev       = false;
		bool  g_click_consumed  = false;
		std::unordered_map<std::string, std::pair<float, float>> g_saved_positions;

		/* ── Helpers ────────────────────────────────────────────────────── */
		inline ImU32 with_alpha(ImU32 c, float a)
		{
			int aa = static_cast<int>(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.f, 1.f));
			return (c & 0x00FFFFFFu) | (aa << IM_COL32_A_SHIFT);
		}

		bool point_in(ImVec2 p, float x, float y, float w, float h)
		{
			return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
		}

		ImFont* font_at(float size)
		{
			auto& pool = vanta::modules::arraylist::shared_pool();
			if (!pool.ensure()) return nullptr;
			return pool.get(size);
		}
		ImTextureID pool_tex()
		{
			return vanta::modules::arraylist::shared_pool().texture_id();
		}

		void draw_text_centered(ImDrawList* dl, const char* text, float cx, float y, float size, ImU32 col)
		{
			ImFont* f = font_at(size);
			if (!f) return;
			const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, -1.f, text);
			dl->PushTextureID(pool_tex());
			dl->AddText(f, size, ImVec2(cx - sz.x * 0.5f, y), col, text);
			dl->PopTextureID();
		}

		/* ── HUD elements table ──────────────────────────────────────────

		   Each entry describes a HUD element the editor can move. Adding a
		   new one is a single push_back inside modules_ref() at the bottom
		   of the file. */
		struct hud_module_entry
		{
			const char* name;
			std::function<void(float&, float&, float&, float&)> get_bbox;
			std::function<void(float, float)>                    set_top_left;
		};
		std::vector<hud_module_entry>& modules_ref();

		/* ── Primitives ────────────────────────────────────────────────── */

		void draw_soft_shadow(ImDrawList* dl, float x, float y, float w, float h, float radius, float alpha)
		{
			if (alpha <= 0.f) return;
			for (int i = 0; i < 4; ++i) {
				const float off = 3.f + i * 3.f;
				dl->AddRectFilled(
					ImVec2(x - off * 0.3f, y + off),
					ImVec2(x + w + off * 0.3f, y + h + off),
					with_alpha(col_shadow, alpha * (0.30f - i * 0.07f)), radius);
			}
		}

		bool draw_pill_button(ImDrawList* dl, const char* label, float x, float y, float w, float h,
			ImVec2 mouse, bool left_edge, float& hover_pulse, float fade, ImU32 accent = col_accent)
		{
			const bool hovered = point_in(mouse, x, y, w, h);
			if (hovered) hover_pulse = 1.f;

			const float radius = h * 0.5f;
			draw_soft_shadow(dl, x, y, w, h, radius, fade);
			dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
				with_alpha(col_card, fade), radius);
			dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
				with_alpha(col_card_hi, fade * hover_pulse), radius);

			const ImU32 hi = with_alpha(IM_COL32(255, 255, 255, 18), fade);
			dl->AddRectFilledMultiColor(
				ImVec2(x + 1.f, y + 1.f), ImVec2(x + w - 1.f, y + h * 0.5f),
				hi, hi, hi & 0x00FFFFFFu, hi & 0x00FFFFFFu);

			dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
				with_alpha(col_border, fade * (1.f + 0.3f * hover_pulse)),
				radius, 0, 1.f);

			const float ts = h * 0.36f;
			draw_text_centered(dl, label, x + w * 0.5f, y + (h - ts) * 0.5f - 1.f,
				ts, with_alpha(col_text, fade));

			// Silence the compiler about the unused accent hint — kept in
			// the signature so a caller can still colour-code a pill without
			// touching every call site if we change how the accent is used.
			(void)accent;

			if (hovered && left_edge && !g_click_consumed) {
				g_click_consumed = true;
				return true;
			}
			return false;
		}

		void draw_grid(ImDrawList* dl, int fb_w, int fb_h, float fade)
		{
			if (fade <= 0.f) return;

			// Full-screen dim behind the grid.
			dl->AddRectFilled(ImVec2(0.f, 0.f), ImVec2(float(fb_w), float(fb_h)),
				with_alpha(IM_COL32(0, 0, 0, 65), fade));

			const int step = 40;

			/* Black + purple palette: minor cells in a soft black hairline,
			   major cells (every 4th) in a rich violet. Reads calm on both
			   bright and dark scenes and matches the client accent. */
			const ImU32 minor_col  = with_alpha(IM_COL32(  0,   0,   0,  70), fade);
			const ImU32 major_col  = with_alpha(IM_COL32(155,  75, 220, 120), fade);
			const ImU32 minor_glow = with_alpha(IM_COL32(155,  75, 220,  22), fade);

			/* Collect live HUD element bboxes and split each grid line into
			   segments that skip those rects — otherwise the lines march
			   right across the arraylist and read as visual noise on top
			   of the element the editor is meant to help you place. */
			struct rect { float x, y, w, h; };
			std::vector<rect> holes;
			holes.reserve(modules_ref().size());
			for (const auto& m : modules_ref()) {
				float bx = 0, by = 0, bw = 0, bh = 0;
				if (m.get_bbox) m.get_bbox(bx, by, bw, bh);
				if (bw > 1.f && bh > 1.f)
					holes.push_back({ bx - 6.f, by - 6.f, bw + 12.f, bh + 12.f });
			}

			// Draw a single line from a-b with any hole overlaps stripped.
			auto draw_segmented_v = [&](float x, float y0, float y1, ImU32 c) {
				// Collect [top, bot] intervals to skip on this column.
				std::vector<std::pair<float, float>> gaps;
				for (const auto& h : holes) {
					if (x < h.x || x >= h.x + h.w) continue;
					gaps.emplace_back(h.y, h.y + h.h);
				}
				std::sort(gaps.begin(), gaps.end(),
					[](auto& a, auto& b) { return a.first < b.first; });
				float cursor = y0;
				for (const auto& g : gaps) {
					if (g.first > cursor)
						dl->AddLine(ImVec2(x, cursor), ImVec2(x, g.first), c, 1.f);
					cursor = std::max(cursor, g.second);
				}
				if (cursor < y1)
					dl->AddLine(ImVec2(x, cursor), ImVec2(x, y1), c, 1.f);
			};
			auto draw_segmented_h = [&](float y, float x0, float x1, ImU32 c) {
				std::vector<std::pair<float, float>> gaps;
				for (const auto& h : holes) {
					if (y < h.y || y >= h.y + h.h) continue;
					gaps.emplace_back(h.x, h.x + h.w);
				}
				std::sort(gaps.begin(), gaps.end(),
					[](auto& a, auto& b) { return a.first < b.first; });
				float cursor = x0;
				for (const auto& g : gaps) {
					if (g.first > cursor)
						dl->AddLine(ImVec2(cursor, y), ImVec2(g.first, y), c, 1.f);
					cursor = std::max(cursor, g.second);
				}
				if (cursor < x1)
					dl->AddLine(ImVec2(cursor, y), ImVec2(x1, y), c, 1.f);
			};

			for (int x = step; x < fb_w; x += step) {
				const bool is_major = (x % (step * 4) == 0);
				const ImU32 c = is_major ? major_col : minor_col;
				// Faint glow under the major violet lines gives them body
				// against a dark scene without pushing the alpha higher.
				if (is_major)
					draw_segmented_v(float(x), 0.f, float(fb_h), minor_glow);
				draw_segmented_v(float(x), 0.f, float(fb_h), c);
			}
			for (int y = step; y < fb_h; y += step) {
				const bool is_major = (y % (step * 4) == 0);
				const ImU32 c = is_major ? major_col : minor_col;
				if (is_major)
					draw_segmented_h(float(y), 0.f, float(fb_w), minor_glow);
				draw_segmented_h(float(y), 0.f, float(fb_w), c);
			}

			// Top / bottom vignette so the exit pill and drag hints pop.
			const ImU32 v0 = with_alpha(IM_COL32(0, 0, 0, 120), fade);
			const ImU32 v1 = with_alpha(IM_COL32(0, 0, 0,   0), fade);
			dl->AddRectFilledMultiColor(ImVec2(0.f, 0.f), ImVec2(float(fb_w), 120.f), v0, v0, v1, v1);
			dl->AddRectFilledMultiColor(ImVec2(0.f, float(fb_h - 140)), ImVec2(float(fb_w), float(fb_h)), v1, v1, v0, v0);
		}

		void handle_drag(ImDrawList* dl, int fb_w, int fb_h,
			ImVec2 mouse, bool left_down, bool left_edge, float fade)
		{
			auto& mods = modules_ref();

			struct live { int index; float x, y, w, h; };
			std::vector<live> boxes;
			boxes.reserve(mods.size());
			for (int i = 0; i < int(mods.size()); ++i) {
				float x = 0, y = 0, w = 0, h = 0;
				if (mods[i].get_bbox) mods[i].get_bbox(x, y, w, h);
				if (w > 1.f && h > 1.f) boxes.push_back({ i, x, y, w, h });
			}

			int hover_idx = -1;
			for (const auto& b : boxes)
				if (point_in(mouse, b.x, b.y, b.w, b.h)) hover_idx = b.index;

			if (left_edge && !g_click_consumed && hover_idx >= 0 && g_dragging_index < 0) {
				for (const auto& b : boxes) {
					if (b.index != hover_idx) continue;
					g_dragging_index = hover_idx;
					g_drag_grab_x = mouse.x - b.x;
					g_drag_grab_y = mouse.y - b.y;
					g_click_consumed = true;
					break;
				}
			}

			if (!left_down) {
				if (g_dragging_index >= 0) {
					vanta::ConfigManager::get().markDirty();
				}
				g_dragging_index = -1;
			}

			for (const auto& b : boxes) {
				const bool is_hover = hover_idx == b.index;
				const bool is_drag  = g_dragging_index == b.index;
				if (!is_hover && !is_drag) continue;

				const ImU32 col = is_drag
					? with_alpha(IM_COL32(255, 210, 100, 255), fade)
					: with_alpha(col_accent, fade * 0.85f);

				dl->AddRect(ImVec2(b.x - 4.f, b.y - 4.f),
					ImVec2(b.x + b.w + 4.f, b.y + b.h + 4.f),
					col, 6.f, 0, 1.5f);

				const std::pair<float, float> corners[4] = {
					{ b.x - 4.f, b.y - 4.f },
					{ b.x + b.w + 4.f, b.y - 4.f },
					{ b.x - 4.f, b.y + b.h + 4.f },
					{ b.x + b.w + 4.f, b.y + b.h + 4.f },
				};
				for (const auto& [cx, cy] : corners)
					dl->AddCircleFilled(ImVec2(cx, cy), 3.f, col);

				// Label with the element's name — floats above the hover box.
				if (is_hover || is_drag) {
					draw_text_centered(dl, mods[b.index].name,
						b.x + b.w * 0.5f, b.y - 24.f, 13.f,
						with_alpha(col_text, fade));
				}
			}

			if (g_dragging_index >= 0) {
				for (const auto& b : boxes) {
					if (b.index != g_dragging_index) continue;

					float nx = mouse.x - g_drag_grab_x;
					float ny = mouse.y - g_drag_grab_y;

					const float keep_x = std::min(b.w, 48.f);
					const float keep_y = std::min(b.h, 24.f);

					nx = std::clamp(nx, keep_x - b.w, float(fb_w) - keep_x);
					ny = std::clamp(ny, keep_y - b.h, float(fb_h) - keep_y);

					if (mods[g_dragging_index].set_top_left) {
						mods[g_dragging_index].set_top_left(nx, ny);
						g_saved_positions[mods[g_dragging_index].name] = { nx, ny };
					}
					break;
				}
			}
		}
	} // namespace

	/* ── Public API ────────────────────────────────────────────────── */

	bool is_active() { return g_active; }

	void set_active(bool a)
	{
		if (g_active == a) return;
		g_active = a;
		if (!a) {
			if (g_dragging_index >= 0) {
				vanta::ConfigManager::get().markDirty();
			}
			g_dragging_index = -1;
		}
	}

	void set_exit_callback(std::function<void()> cb) { g_exit_cb = std::move(cb); }

	// Kept as no-ops for compatibility with older headers that referenced
	// this per-element offset accessor.
	void arraylist_offset(float& out_x, float& out_y) { out_x = 0.f; out_y = 0.f; }
	void set_arraylist_offset(float, float) {}

	void tick(float delta)
	{
		const float k = 14.f;
		const float t = 1.f - std::exp(-k * std::clamp(delta, 0.f, 0.1f));

		g_active_anim  += ((g_active ? 1.f : 0.f) - g_active_anim) * t;
		g_hover_exit   += (0.f - g_hover_exit) * t;
	}

	void render(int fb_w, int fb_h)
	{
		/* Cleared whenever the editor is asleep and set on its first live frame,
		   so a button that was already down when it woke is not read as a press
		   on the grid. See the ledge computation below. */
		static bool s_awake = false;

		const float fade = std::clamp(g_active_anim, 0.f, 1.f);
		if (fade <= 0.001f && !g_active) {
			g_left_prev = false;
			g_dragging_index = -1;
			s_awake = false;
			return;
		}

		ImDrawList* dl = ImGui::GetForegroundDrawList();
		if (dl == nullptr) return;

		const ImVec2 mouse = ImGui::GetMousePos();
		const bool ldown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

		/* @the click that opened this page is not a click on the grid.

		   g_left_prev is forced false while the editor is asleep, and the sidebar
		   switches category on the press frame — so the first frame the editor
		   runs, the button is still physically down and looks exactly like a
		   fresh press. Anything the user happened to park under the sidebar was
		   grabbed and dragged for the rest of that hold. A press already in
		   progress when the editor wakes up belongs to whatever woke it. */
		const bool ledge = ldown && !g_left_prev && s_awake;

		s_awake = true;

		g_click_consumed = false;

		dl->PushClipRect(ImVec2(0.f, 0.f),
			ImVec2(float(fb_w), float(fb_h)), false);

		draw_grid(dl, fb_w, fb_h, fade);

		/* The Exit pill used to sit here. Removed on request — and it had been
		   drawing a broken glyph anyway: its arrow was U+2190, which is outside
		   the range the display atlas bakes, so it came out as a missing-character
		   box. HUD mode is still left the way it is entered: toggle the panel off
		   and on, which the shell already rebinds back to the previous category. */

		// The whole point of the editor: drag HUD elements around on the grid.
		if (!g_click_consumed)
			handle_drag(dl, fb_w, fb_h, mouse, ldown, ledge, fade);

		dl->PopClipRect();

		g_left_prev = ldown;
	}

} // namespace vanta::hud_editor

/* ──────────────────────────────────────────────────────────────────────
   HUD elements the editor knows how to move.
   ──────────────────────────────────────────────────────────────────── */

namespace vanta::hud_editor
{
	namespace
	{
		std::vector<hud_module_entry>& modules_ref()
		{
			static std::vector<hud_module_entry> mods = []() {
				std::vector<hud_module_entry> out;

				hud_module_entry al{};
				al.name         = "Array List";
				al.get_bbox     = [](float& x, float& y, float& w, float& h) {
					vanta::modules::arraylist::query_bbox(x, y, w, h);
				};
				al.set_top_left = [](float x, float y) {
					vanta::modules::arraylist::set_top_left(x, y);
				};
				out.push_back(std::move(al));

                hud_module_entry kb{};
                kb.name = "Keybinds";
                kb.get_bbox = [](float& x,float& y,float& w,float& h) { modules::keybindhud::query_bbox(x,y,w,h); };
                kb.set_top_left = [](float x,float y) { modules::keybindhud::set_top_left(x,y); };
                out.push_back(std::move(kb));

				hud_module_entry wm{};
				wm.name         = "Watermark";
				wm.get_bbox     = [](float& x, float& y, float& w, float& h) {
					vanta::modules::watermark::query_bbox(x, y, w, h);
				};
				wm.set_top_left = [](float x, float y) {
					vanta::modules::watermark::set_top_left(x, y);
				};
				out.push_back(std::move(wm));

				hud_module_entry th{};
				th.name         = "Target Display";
				th.get_bbox     = [](float& x, float& y, float& w, float& h) {
					vanta::modules::targethud::query_bbox(x, y, w, h);
				};
				th.set_top_left = [](float x, float y) {
					vanta::modules::targethud::set_top_left(x, y);
				};
				out.push_back(std::move(th));

				hud_module_entry rd{};
				rd.name         = "Radar";
				rd.get_bbox     = [](float& x, float& y, float& w, float& h) {
					vanta::modules::radar::query_bbox(x, y, w, h);
				};
				rd.set_top_left = [](float x, float y) {
					vanta::modules::radar::set_top_left(x, y);
				};
				out.push_back(std::move(rd));

				hud_module_entry sp{};
				sp.name         = "Spotify";
				sp.get_bbox     = [](float& x, float& y, float& w, float& h) {
					vanta::modules::spotify::query_bbox(x, y, w, h);
				};
				sp.set_top_left = [](float x, float y) {
					vanta::modules::spotify::set_top_left(x, y);
				};
				out.push_back(std::move(sp));

				/* Last in the list, and that is deliberate: hover resolution
				   keeps the LAST box the cursor is inside, so the notification
				   stack wins any overlap. It is the only element here that
				   appears over the others by design. */
				hud_module_entry nf{};
				nf.name         = "Notifier";
				nf.get_bbox     = [](float& x, float& y, float& w, float& h) {
					vanta::modules::notifier::query_bbox(x, y, w, h);
				};
				nf.set_top_left = [](float x, float y) {
					vanta::modules::notifier::set_top_left(x, y);
				};
				out.push_back(std::move(nf));

				return out;
			}();
			return mods;
		}
	}

	void save_positions(nlohmann::json& out)
	{
		out = nlohmann::json::object();
		for (const auto& [name, pos] : g_saved_positions) {
			out[name] = { {"x", pos.first}, {"y", pos.second} };
		}
	}

	void load_positions(const nlohmann::json& in)
	{
		if (!in.is_object()) return;
		auto& mods = modules_ref();
		for (auto& mod : mods) {
			if (in.contains(mod.name) && in[mod.name].is_object()) {
				const auto& p = in[mod.name];
				if (p.contains("x") && p.contains("y") && p["x"].is_number() && p["y"].is_number()) {
					float x = p["x"].get<float>();
					float y = p["y"].get<float>();
					g_saved_positions[mod.name] = { x, y };
					if (mod.set_top_left) mod.set_top_left(x, y);
				}
			}
		}
	}
}
