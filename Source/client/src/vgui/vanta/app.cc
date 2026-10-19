#include "../includes.hh"

#include "jvm/JvmContext.h"
#include "core/config/ConfigManager.h"

#include "views/hud_editor.hh"
#include "views/intro.hh"
#include "widgets/bind_popup.hh"
#include "../../modules/render/chams/chams.h"
#include "../../modules/misc/spectate/spectate.h"
#include "../../modules/render/outline/outline.h"
#include "../../modules/render/targetesp/target_attack.h"
#include "../../modules/render/targethud/head_cache.h"
#include "../../modules/render/targethud/hud_font.h"

namespace vanta
{
	namespace
	{
		/* floating card layout */
		constexpr float k_margin = 12.f;
		constexpr float k_gap = 10.f;
		constexpr float k_window_radius = 16.f;
		constexpr float k_card_radius = 14.f;
		constexpr float k_header_height = 60.f;

		/* the strip along the top that drags the window, minus the controls that live in it */

		math::c_vector_2d cursor_on_desktop()
		{
			POINT point{};
			GetCursorPos(&point);

			return math::c_vector_2d(static_cast<float>(point.x), static_cast<float>(point.y));
		}
	}

	bool c_app::initialize()
	{
		// resolved before anything loads, fonts and textures are addressed through it
		core::locate_asset_root();

		/* No window and no device to create any more: gl_hook has already put
		   our GL context current and begun an ImGui frame by the time this
		   runs, on the game's render thread. */
		g_render->setup();

		g_fonts->initialize();

		// the atlas grew, force the backend to re-upload it before the first frame
		ImGui_ImplOpenGL3_DestroyDeviceObjects();

		g_icons->initialize();
		emoji::initialize();

		this->m_modules = std::make_unique<c_module_manager>();
		this->m_settings = std::make_unique<c_settings>();

		const float loaded_scale = vanta::vgui::bridge::consumePendingScale();
		if (loaded_scale >= 0.5f && loaded_scale <= 2.5f)
			this->m_user_scale = loaded_scale;
		this->m_sidebar.sync_from_theme();

		this->m_sidebar.set_callback([this](const nav_item_t& item)
			{
				if (this->m_category == item.m_title)
					return;

				/* Remember where we came from before hopping into HUD mode so
				   the editor's Exit pill can put us back on the same tab. */
				if (item.m_title == "HUD" && this->m_category != "HUD")
					this->m_pre_hud_category = this->m_category;

				c_color_setting::clear_hex_focus();
				c_block_picker_setting::clear_search_focus();
				c_slider_setting::clear_value_focus();
				if (this->m_settings && item.m_title != "Configs")
					this->m_settings->set_input_focused(false);

				this->m_category = item.m_title;
				this->m_modules->on_category_changed(this->m_category);
				if (this->m_category == "Configs" && this->m_settings)
					this->m_settings->refresh_profiles();

				vanta::hud_editor::set_active(this->m_category == "HUD");
			});

		/* @the eraser.

		   Scoped to the category on screen, not to the whole client. The button
		   lives in the header of one category's list and sits directly above that
		   list, so what it clears is what is under it — wiping every module in the
		   client from a control that looks like it belongs to five of them would be
		   a nasty surprise exactly once. */
		this->m_header.set_wipe_callback([this]()
			{
				this->m_modules->wipe_category(this->m_category);
			});

		/* Wire the editor's Exit button back through to us — the editor
		   itself has no idea what a "category" is. */
		vanta::hud_editor::set_exit_callback([this]() {
			this->m_category = this->m_pre_hud_category.empty()
				? std::string{"Combat"} : this->m_pre_hud_category;
			this->m_modules->on_category_changed(this->m_category);
			this->m_sidebar.sync_category(this->m_category);
			vanta::hud_editor::set_active(false);
		});

		/* Arm the inject sequence. It does not start playing here — it waits a
		   moment for the account name to come back from the JVM, because the first
		   thing it says is addressed to whoever just injected. */
		vanta::intro::begin();

		slog::log::success("vanta client is up");
		return true;
	}

	void c_app::collect_input()
	{
		const ImGuiIO& io = ImGui::GetIO();

		this->m_input.m_cursor = g_input->get_mouse_position();
		this->m_input.m_left_down = g_input->click_down(input::mouse_buttons::left);
		this->m_input.m_left_clicked = g_input->clicked(input::mouse_buttons::left);
		this->m_input.m_right_clicked = g_input->clicked(input::mouse_buttons::right);
		this->m_input.m_wheel = g_input->get_wheel_value();

		// a long stall (a drag, a resize) must not teleport every animation to its target
		this->m_input.m_delta = std::min(io.DeltaTime, 0.1f);

	}

	void c_app::dispatch_keyboard()
	{
		ImGuiIO& io = ImGui::GetIO();

		/* @ctrl + wheel resizes the client.

		   It replaces a slider on a page that no longer exists, and it is the better
		   control regardless: a zoom belongs on the thing being zoomed. Held
		   modifier, so an ordinary wheel still scrolls the list under the cursor —
		   and it consumes the event, because a scroll that both resized the panel
		   and moved the list would be neither. */
		if (io.KeyCtrl && this->m_input.m_wheel != 0.f)
		{
			constexpr float k_step = 0.05f;

			const float old_scale = this->m_user_scale;
			this->m_user_scale = std::clamp(
				this->m_user_scale + this->m_input.m_wheel * k_step, 0.55f, 1.50f);

			if (std::abs(this->m_user_scale - old_scale) > 0.001f)
			{
				ConfigManager::get().markDirty();
			}

			this->m_input.m_wheel = 0.f;
		}

		if (this->m_input.m_wheel != 0.f)
		{
			const float wheel_cx =
				(this->m_players_x - this->m_panel_x) / this->m_panel_scale;
			const float wheel_cy =
				(this->m_players_y - this->m_panel_y) / this->m_panel_scale;

			if (!this->on_hud_page()
				&& this->m_header.players_shown()
				&& this->m_input.hovered(wheel_cx, wheel_cy,
					static_cast<float>(k_players_width),
					static_cast<float>(k_players_height)))
				this->m_players.on_scroll(this->m_input.m_wheel);
			else if (this->on_settings_page())
				this->m_settings->on_scroll(this->m_input.m_wheel);
			else
				this->m_modules->on_scroll(this->m_input.m_wheel);
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			if (c_color_setting::hex_focus() != nullptr)
				c_color_setting::clear_hex_focus();
			else if (c_block_picker_setting::search_focus() != nullptr)
                c_block_picker_setting::clear_search_focus();
            else if (c_slider_setting::value_focus() != nullptr)
				c_slider_setting::clear_value_focus();
			else if (c_module::binding_active())
				c_module::cancel_binding();
			else if (vanta::vgui::bridge::isMenuKeyBinding())
				return;
			else if (this->m_players.search_focused())
				this->m_players.set_search_focused(false);
			else if (this->m_sidebar.search_focused())
				this->m_sidebar.set_search_focused(false);
			else if (this->m_settings && this->m_settings->input_focused())
				this->m_settings->set_input_focused(false);
			else
				this->set_visible(false);

			return;
		}

		if (c_color_setting* hex = c_color_setting::hex_focus(); hex != nullptr)
		{
			if (io.KeyCtrl)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_V, false))
					hex->paste_clipboard();

				if (ImGui::IsKeyPressed(ImGuiKey_C, false))
					hex->copy_clipboard();

				return;
			}

			for (const ImWchar character : io.InputQueueCharacters)
			{
				if (character < 128)
					hex->on_character(static_cast<char>(character));
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
				hex->on_backspace();

			if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
				c_color_setting::clear_hex_focus();

			return;
		}

		if (c_block_picker_setting* pk = c_block_picker_setting::search_focus(); pk != nullptr)
		{
			for (const ImWchar character : io.InputQueueCharacters)
			{
				if (character < 128)
					pk->on_character(static_cast<char>(character));
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
				pk->on_backspace();

			if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
				c_block_picker_setting::clear_search_focus();

			return;
		}

		if (c_slider_setting* chip = c_slider_setting::value_focus(); chip != nullptr)
		{
			for (const ImWchar character : io.InputQueueCharacters)
			{
				if (character < 128)
					chip->on_character(static_cast<char>(character));
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
				chip->on_backspace();

			if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
				c_slider_setting::clear_value_focus();

			return;
		}

		if (this->m_players.search_focused())
		{
			if (this->m_sidebar.search_focused())
				this->m_sidebar.set_search_focused(false);

			if (this->m_settings && this->m_settings->input_focused())
				this->m_settings->set_input_focused(false);

			for (const ImWchar character : io.InputQueueCharacters)
			{
				if (character < 128)
					this->m_players.on_character(static_cast<char>(character));
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
				this->m_players.on_backspace();

			if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
				this->m_players.set_search_focused(false);

			return;
		}

		if (this->on_settings_page() && this->m_settings && this->m_settings->input_focused())
		{
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
				this->m_settings->paste_clipboard(); return;
			}
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
				this->m_settings->select_all(); return;
			}
			if (this->m_sidebar.search_focused())
				this->m_sidebar.set_search_focused(false);
			if (this->m_players.search_focused())
				this->m_players.set_search_focused(false);

			if (!io.InputQueueCharacters.empty())
			{
				for (const ImWchar character : io.InputQueueCharacters)
				{
					if (character < 128)
						this->m_settings->on_character(static_cast<char>(character));
				}
			}
			else
			{
				const bool shift = io.KeyShift;
				for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k)
				{
					if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false))
					{
						char ch = static_cast<char>((shift ? 'A' : 'a') + (k - ImGuiKey_A));
						this->m_settings->on_character(ch);
					}
				}
				for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k)
				{
					if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false))
					{
						char ch = static_cast<char>('0' + (k - ImGuiKey_0));
						this->m_settings->on_character(ch);
					}
				}
				if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
					this->m_settings->on_character(' ');
				if (ImGui::IsKeyPressed(ImGuiKey_Minus, false))
					this->m_settings->on_character(shift ? '_' : '-');
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
				this->m_settings->on_backspace();

			if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
				this->m_settings->on_submit();

			return;
		}

		if (this->m_sidebar.search_focused())
		{
			for (const ImWchar character : io.InputQueueCharacters)
			{
				if (character < 128)
					this->m_sidebar.on_character(static_cast<char>(character));
			}

			if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true))
				this->m_sidebar.on_backspace();

			/* Enter is no longer the thing that records the search — leaving the
			   field is, and this leaves the field. It survives as a way to dismiss
			   the caret once the results are on screen. */
			if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
				this->m_sidebar.set_search_focused(false);

			return;
		}

		/* @the bind pill no longer listens here, and that is the fix.

		   This used to walk ImGuiKey_NamedKey_BEGIN..END and hand every press to
		   the module list. Two things were wrong with it. ImGuiKey is not a Win32
		   virtual key and nothing in this client translates between them, so what
		   the pill captured could never match what the engine compares against.
		   And that range is not keys — ImGui puts MouseLeft, MouseRight and
		   MouseWheelY inside it and refreshes them every frame, so clicking the
		   pill a second time to cancel bound the module to the mouse, and one
		   scroll of the module list bound it to the wheel.

		   The pill reads the real keyboard itself now, in c_module::poll_bind_capture. */
	}

	void c_app::place(int fb_w, int fb_h)
	{
		const float pw  = static_cast<float>(k_window_width)  * this->m_panel_scale;
		const float ph  = static_cast<float>(k_window_height) * this->m_panel_scale;
		float centre_x = 0.f, centre_y = 0.f;
		this->composition_centre(fb_w, fb_h, centre_x, centre_y);

		/* Contained, always. The panel is drawn into the game's framebuffer and
		   anything outside it is scissored away, so a panel dragged past the
		   edge does not hang off the window — it gets sliced. Clamping the
		   offset (not just the position) is what stops a drag against the edge
		   from banking up travel that has to be undone before it moves back.

		   @the reserved gutter is GONE from this clamp, and only from this clamp.

		   min_x used to be the width of the player list's gutter, and it was the
		   whole rule keeping that list on screen: the panel simply could not be
		   dragged into it. The list has its own framebuffer position now and
		   place_players() clamps it to the frame itself, so the reservation is no
		   longer load-bearing — and left standing it is actively wrong. It bars
		   the panel from about six hundred pixels of the left of the screen even
		   after the list has been dragged to the right, and it makes the panel
		   impossible to centre: composition_centre already shifts it right by the
		   gutter, and a floor at the gutter forbids dragging it back.

		   composition_width() and composition_centre() are NOT touched. The
		   extent still has to reserve room in the scale fit, because the list is
		   still on screen at the panel's scale and both must fit; and it must
		   stay a constant because it feeds set_pending_atlas_scale and because
		   moving the composition centre would silently rewrite both this offset
		   and the list's. See the note on it in app.hh.

		   max_x is still floored at min_x so std::clamp is never called with
		   inverted bounds — which is undefined and terminates on this toolchain
		   rather than misbehaving. */
		constexpr float min_x = 0.f;
		const float max_x = std::max(min_x, static_cast<float>(fb_w) - pw);
		const float max_y = std::max(0.f, static_cast<float>(fb_h) - ph);

		this->m_panel_x = std::clamp(centre_x + this->m_offset_x, min_x, max_x);
		this->m_panel_y = std::clamp(centre_y + this->m_offset_y, 0.f, max_y);

		this->m_offset_x = this->m_panel_x - centre_x;
		this->m_offset_y = this->m_panel_y - centre_y;
	}

	void c_app::handle_window_drag(int fb_w, int fb_h)
	{
		/* @not on the HUD page.

		   That page does not paint the panel at all — the editor takes the whole
		   screen — so the bands are invisible there, and the editor hit-tests the
		   same raw cursor. Dragging a HUD element that happened to start inside one
		   of them moved the unseen panel underneath at the same time. */
		if (this->on_hud_page())
		{
			this->m_dragging = false;
			return;
		}

		/* Latched drags do not survive the panel going away. frame() returns early
		   once the dismiss fade is done, so a drag still in progress at that moment
		   was never given the mouse-up that ends it. */
		if (!this->m_visible)
		{
			this->m_dragging = false;
			return;
		}

		/* @the player list wins, and it has to.

		   Both drags run in the same frame off the same mouse. The list is tested
		   FIRST (see frame()) because it is painted LAST — dragged over the panel
		   its handle sits on top of the top margin, the brand band and the header
		   band, and a hit test that disagreed with the paint order would hand the
		   press to a surface the user cannot see.

		   This is the other half of the interlock. handle_players_drag refuses to
		   start while m_dragging is latched, so at most one of the two is ever
		   live; m_dragging is therefore already false here and the assignment is
		   belt and braces rather than logic. */
		if (this->m_players_dragging)
		{
			this->m_dragging = false;
			return;
		}

		/* Resolved in FRAMEBUFFER pixels, never in canvas space. A canvas-space
		   drag would feed its own result back into its input: the offset moves
		   the panel, the panel re-maps canvas -> framebuffer, and the grab point
		   crawls out from under the cursor. Absolute cursor motion has no such
		   loop. This is the same reason the size slider is screen-anchored. */
		const ImVec2 mouse = ImGui::GetMousePos();
		const bool   down  = ImGui::IsMouseDown(ImGuiMouseButton_Left);

		if (!down)
		{
			this->m_dragging = false;
			return;
		}

		if (!this->m_dragging)
		{
			/* Only a press that STARTS on the handle begins a drag — a button
			   already held when the cursor wanders onto the logo must not. */
			if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				return;

			/* Canvas rect -> framebuffer rect, then hit tested against the absolute
			   cursor. The handles are published in canvas coordinates because that is
			   the space their owners lay out in; the test has to happen in the space
			   the cursor is reported in. */
			const auto grabbed = [&](float hx, float hy, float hw, float hh) -> bool
			{
				if (hw <= 0.f || hh <= 0.f)
					return false;

				const float sx = this->m_panel_x + hx * this->m_panel_scale;
				const float sy = this->m_panel_y + hy * this->m_panel_scale;
				const float sw = hw * this->m_panel_scale;
				const float sh = hh * this->m_panel_scale;

				return mouse.x >= sx && mouse.x <= sx + sw
					&& mouse.y >= sy && mouse.y <= sy + sh;
			};

			/* @the whole top of the window is the handle, not two small patches.

			   The wordmark alone was a handle you had to be told about, and even
			   with the header's empty band added you still had to find the one
			   stretch that was live. A title bar is grabbed without looking, so the
			   rule here is the one a title bar implies: everything along the top of
			   the panel drags it, EXCEPT the controls that actually live up there —
			   the collapse chevron, the filter tabs with the config drawer, and the
			   reset button. Those keep their presses.

			   Rather than re-deriving where the controls are (a second copy of that
			   arithmetic drifts the moment either side changes), the two views keep
			   publishing exactly the free space they left, and the shell only
			   EXTENDS each band upward to the panel edge. Nothing is drawn above
			   them, so the strip over a handle is always free. */
			const auto extended = [&](float hx, float hy, float hw, float hh) -> bool
			{
				/* y is dropped, not shrunk: the band starts at the very top of the
				   canvas so the outermost pixels of the panel are grabbable too,
				   which is exactly where a cursor aiming for a title bar lands. */
				return grabbed(hx, 0.f, hw, hy + hh);
			};

			float bx = 0.f, by = 0.f, bw = 0.f, bh = 0.f;
			this->m_sidebar.brand_rect(bx, by, bw, bh);

			float tx = 0.f, ty = 0.f, tw = 0.f, th = 0.f;
			this->m_header.drag_rect(tx, ty, tw, th);

			/* @the brand band is off while the rail is collapsed, and it has to be.

			   Collapsing detaches the expand button into its own little card at the
			   TOP of the rail — above the wordmark, inside the strip this band now
			   claims — and while collapsed the band reserves nothing for it, because
			   the in-card chevron it normally steps around is not drawn. The band
			   would sit exactly on the only control that can bring the rail back and
			   eat its press: the sidebar would collapse and refuse to reopen. */
			const bool brand_band = this->m_sidebar.collapse_progress() <= 0.01f;

			/* The header only exists on module pages; the chat, configs and
			   settings pages draw their own bar in that space. Using a band left over
			   from the last module page there would hand their controls' presses to
			   the window drag. */
			const bool header_band = this->on_module_page();

			/* The full-width strip above both cards. Twelve pixels of margin that
			   were dead space, and they are the top edge of the window — the first
			   thing a cursor coming from outside the panel touches. */
			const bool top_margin = grabbed(0.f, 0.f,
				static_cast<float>(k_window_width), k_margin);

			/* The gutter between the rail and the content card, over the header row:
			   no control on either side reaches into it. */
			const bool gutter = grabbed(k_margin + this->m_sidebar_width, 0.f,
				k_gap, k_margin + k_header_height);

			if (!top_margin && !gutter
				&& !(brand_band && extended(bx, by, bw, bh))
				&& !(header_band && extended(tx, ty, tw, th)))
				return;

			/* The colour editor floats free and can be parked anywhere, including
			   on top of these bands. It is shielded from every other view the same
			   way; the window drag is just the last one that had not been told. */
			{
				float px = 0.f, py = 0.f, pw = 0.f, ph = 0.f;

				if (colour_popup::rect(px, py, pw, ph)
					&& mouse.x >= this->m_panel_x + px * this->m_panel_scale
					&& mouse.x <= this->m_panel_x + (px + pw) * this->m_panel_scale
					&& mouse.y >= this->m_panel_y + py * this->m_panel_scale
					&& mouse.y <= this->m_panel_y + (py + ph) * this->m_panel_scale)
					return;
			}

			this->m_dragging = true;
			this->m_drag_offset = math::c_vector_2d(mouse.x - this->m_panel_x,
			                                        mouse.y - this->m_panel_y);
		}

		/* The SAME centre place() uses. Two definitions that disagree by the
		   reserved gutter make the panel jump sideways every drag frame. */
		float centre_x = 0.f, centre_y = 0.f;
		this->composition_centre(fb_w, fb_h, centre_x, centre_y);

		this->m_offset_x = (mouse.x - this->m_drag_offset.x) - centre_x;
		this->m_offset_y = (mouse.y - this->m_drag_offset.y) - centre_y;

		this->place(fb_w, fb_h);
	}

	void c_app::place_players(int fb_w, int fb_h)
	{
		/* @the same contract place() has, for the same reasons.

		   Run every frame, not only while dragging: the framebuffer can be
		   resized and ctrl+wheel can change the scale underneath a card that is
		   sitting still, and either can put it off screen. Re-clamping the OFFSET
		   in place (rather than only the position) is what stops a card pushed
		   against an edge from banking up travel that has to be undone before it
		   moves back.

		   Contained whole, like the panel. A handle-only rule would let the card
		   hang three quarters off the frame, and the part hanging off is not
		   translucent — it is scissored away by ImGui, so it is simply gone. */
		const float lw = static_cast<float>(k_players_width)  * this->m_panel_scale;
		const float lh = static_cast<float>(k_players_height) * this->m_panel_scale;

		float home_x = 0.f, home_y = 0.f;
		this->players_home(fb_w, fb_h, home_x, home_y);

		/* max() guards a framebuffer too small for the card at this scale, so
		   std::clamp is never handed inverted bounds — undefined, and a process
		   abort on this toolchain rather than a misbehaviour. */
		const float max_x = std::max(0.f, static_cast<float>(fb_w) - lw);
		const float max_y = std::max(0.f, static_cast<float>(fb_h) - lh);

		this->m_players_x = std::clamp(home_x + this->m_players_offset_x, 0.f, max_x);
		this->m_players_y = std::clamp(home_y + this->m_players_offset_y, 0.f, max_y);

		this->m_players_offset_x = this->m_players_x - home_x;
		this->m_players_offset_y = this->m_players_y - home_y;
	}

	void c_app::handle_players_drag(int fb_w, int fb_h)
	{
		/* Not on the HUD page: paint() is skipped there, so the handle this would
		   hit-test is wherever the list was last drawn before we left, and the
		   editor hit-tests the same raw cursor. Not while the panel is away
		   either: frame() returns early once the dismiss fade is done, so a latch
		   still held at that moment would never be given its mouse-up. Both are
		   exactly what handle_window_drag does.

		   And never while the panel itself is being dragged — see the interlock
		   note over there. */
		if (this->on_hud_page() || !this->m_visible || this->m_dragging
			|| !this->m_header.players_shown())
		{
			this->m_players_dragging = false;
			return;
		}

		/* FRAMEBUFFER pixels, never canvas. A canvas-space drag feeds its own
		   result back into its input — and worse here than for the panel, since
		   this card's canvas position is DERIVED from the placement, so the grab
		   point would crawl out from under the cursor on the first frame. */
		const ImVec2 mouse = ImGui::GetMousePos();

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			this->m_players_dragging = false;
			return;
		}

		if (!this->m_players_dragging)
		{
			/* Only a press that STARTS on the handle begins a drag — a button
			   already held when the cursor wanders onto the title must not. */
			if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				return;

			/* The band is published LOCAL to the card, so it is offset by the
			   card's CURRENT framebuffer position rather than by the panel's.
			   Nothing here is one frame stale. */
			float hx = 0.f, hy = 0.f, hw = 0.f, hh = 0.f;
			this->m_players.drag_rect(hx, hy, hw, hh);

			/* Zero until the list has been drawn once. */
			if (hw <= 0.f || hh <= 0.f)
				return;

			const float sx = this->m_players_x + hx * this->m_panel_scale;
			const float sy = this->m_players_y + hy * this->m_panel_scale;
			const float sw = hw * this->m_panel_scale;
			const float sh = hh * this->m_panel_scale;

			if (mouse.x < sx || mouse.x > sx + sw
				|| mouse.y < sy || mouse.y > sy + sh)
				return;

			/* The colour editor floats over everything and can be parked on top
			   of this handle. It is shielded from every other view the same way;
			   the window drag has the identical block. It is still addressed in
			   the PANEL's space, because that is where it is drawn. */
			{
				float px = 0.f, py = 0.f, pw = 0.f, ph = 0.f;

				if (colour_popup::rect(px, py, pw, ph)
					&& mouse.x >= this->m_panel_x + px * this->m_panel_scale
					&& mouse.x <= this->m_panel_x + (px + pw) * this->m_panel_scale
					&& mouse.y >= this->m_panel_y + py * this->m_panel_scale
					&& mouse.y <= this->m_panel_y + (py + ph) * this->m_panel_scale)
					return;
			}

			this->m_players_dragging = true;
			this->m_players_drag_offset = math::c_vector_2d(mouse.x - this->m_players_x,
			                                                mouse.y - this->m_players_y);
		}

		/* The SAME home place_players() uses, for the same reason the panel drag
		   shares composition_centre: two definitions that disagree teleport the
		   card sideways on every frame of the drag. */
		float home_x = 0.f, home_y = 0.f;
		this->players_home(fb_w, fb_h, home_x, home_y);

		this->m_players_offset_x = (mouse.x - this->m_players_drag_offset.x) - home_x;
		this->m_players_offset_y = (mouse.y - this->m_players_drag_offset.y) - home_y;

		this->place_players(fb_w, fb_h);
	}

	void c_app::paint()
	{
        if(!this->m_visible) bind_popup::close();
        if(theme::glass_enabled && this->m_visible) panel_glass::capture_game();
		/* The canvas is fixed; gl_hook maps it into the game framebuffer. */
		const float width  = static_cast<float>(k_window_width);
		const float height = static_cast<float>(k_window_height);

		/* the rail width is driven by the sidebar's own collapse spring rather than a second
		   animation, otherwise the card and the layout around it can drift out of step */
		this->m_sidebar_width = 200.f + (54.f - 200.f) * anim::saturate(this->m_sidebar.collapse_progress());

		/* @the player list, converted from ITS space into THIS one, once.

		   The card keeps a framebuffer position of its own so that it does not
		   follow the panel. Everything drawn here is authored in the canvas and
		   gl_hook maps the finished vertices with one affine anchored at the
		   panel, so "independent" can only be expressed as a canvas coordinate
		   that COMPENSATES for where the panel ended up:

		       canvas = (framebuffer - panel_origin) / panel_scale

		   which is exactly the inverse of what gl_hook is about to do, so the
		   card lands on m_players_x/y whatever the panel did.

		   Resolved here and used three times below — the glass pane queued in a
		   moment, the shield that keeps every surface underneath from answering
		   this card's clicks, and the draw itself. Three copies of this division
		   would come apart the first time one of them was retuned, and the glass
		   would then blur a rectangle the card is not in.

		   m_panel_scale is clamped to a positive floor by frame() before place()
		   runs, and paint() only ever runs after both. */
		const float players_w  = static_cast<float>(k_players_width);
		const float players_h  = static_cast<float>(k_players_height);
		const float players_cx = (this->m_players_x - this->m_panel_x) / this->m_panel_scale;
		const float players_cy = (this->m_players_y - this->m_panel_y) / this->m_panel_scale;

		/* @the player list's entrance.

		   Eased here rather than in the header because the header owns the switch and this owns
		   the card: the surface is drawn outside the panel at a coordinate only the shell
		   knows, and the animation is a property of the drawing.

		   A fade AND a small slide, not one or the other. A pure fade at this size reads as the
		   card being turned down rather than put away, and a pure slide has to travel the
		   card's whole width to get out of sight — which at this scale is a long, slow shove
		   across the game. Twenty-odd px of drift under the fade is enough to give the fade a
		   direction, and the direction is left, back into the gutter it came from.

		   The caret goes with it. Left focused, a hidden search field keeps swallowing every
		   keystroke — and through text_focus() that means gl_hook keeps eating W/A/S/D, so
		   switching the list off would leave the user unable to walk. */
		const bool players_on = this->m_header.players_shown();

		this->m_players_reveal = anim::lerp(this->m_players_reveal, players_on ? 1.f : 0.f,
			14.f, this->m_input.m_delta);

		if (!players_on)
			this->m_players.set_search_focused(false);

		/* Still drawn while it is on its way out, and gone once there is nothing left of it. */
		const bool players_live = this->m_players_reveal > 0.004f;

		/* 1. window backdrop.

		   In glass mode this is where the whole effect comes from. It is the FIRST
		   thing the client draws, so a blur queued here samples a framebuffer that
		   still holds nothing but the game — which is why this pane can show a
		   blurred world where the account panel, drawn last over opaque cards, can
		   only ever show a blurred client. */
		/* @gated on the boolean, not on the fade.

		   A blur is not a vertex. Everything else the client draws is faded out by
		   scaling the alpha of the finished buffer, and the glass composite is
		   simply not in that buffer — so while the panel faded away over its
		   quarter second, a full-strength blurred rectangle stayed behind on the
		   game with nothing left inside it.

		   Fading the glass instead of gating it does not work here either: it fades
		   by easing its brightness back to 1, and a blurred copy of the GAME laid
		   over the game is still obviously a blurred copy of the game. Over the
		   client's own flat cards that trick is invisible; over a world it is not.

		   So it stops the moment the panel is dismissed. The pane is mostly blur to
		   begin with, so losing it makes the panel read as gone immediately, which
		   is what pressing the key meant. */
		/* @BOTH panes, in ONE composite, and it has to be one.

		   There is a single blur pipeline behind panel_glass, and it sizes its
		   scene texture and both ping-pong buffers to whatever region it was
		   asked to capture. Two separate calls at two different sizes made it
		   tear all three down and rebuild them twice a frame, forever.

		   Queuing the player list HERE rather than beside its own draw is the
		   other half. Its capture is padded by sixty canvas px so the blur kernel
		   has surround to reach into, and the gutter between it and the panel is
		   only sixteen — so a capture taken at its own draw site, last in the
		   frame, would pull forty-odd px of already-composited sidebar into its
		   right edge. Taken here, first, both panes sample a framebuffer that
		   still holds nothing but the game, which is the whole reason the panel's
		   own pane has always been first. */
		if (theme::glass_enabled && this->m_visible)
		{
			/* The list's pane is the rect resolved at the top of this function,
			   not the old fixed gutter. panel_glass converts a canvas rect back
			   with panel_x / panel_scale, which is precisely the inverse of the
			   division up there, so the blur lands on m_players_x/y exactly.

			   Both panes still go in ONE composite: there is a single blur
			   pipeline and it sizes its buffers to the region it captures, so two
			   calls at two sizes rebuild all three textures twice a frame. That is
			   unchanged by the card being movable — the captured region is the
			   UNION of the panes, so it now breathes as the card is dragged and
			   the pipeline reallocates while the mouse is down. It settles the
			   moment the drag ends, which is the acceptable end of that trade. */
			const panel_glass::pane_t panes[] = {
				{ 0.f, 0.f, width, height, k_window_radius },
				{ players_cx, players_cy, players_w, players_h, 16.f },
			};

			/* @the list's pane is gated on the BOOLEAN, exactly like the panel's own.

			   A blur is not a vertex. The fade above scales the alpha of finished geometry and
			   the composite is simply not in that buffer, so a pane kept alive for the fade
			   would leave a full-strength blurred rectangle sitting on the game with the card
			   dissolving out of it — which is the bug the note above this one describes for
			   the panel, in miniature. The pane is mostly blur to begin with, so dropping it
			   at the press makes the card read as gone immediately, which is what the press
			   meant. */
			panel_glass::blur_under(panes, players_on ? 2 : 1, 1.f,
				/*brightness=*/0.62f, /*darken=*/0.34f);
		}

		draw::rounded_rect(0.f, 0.f, width, height, k_window_radius,
			theme::pane(theme::bg_window, 0.34f));

		const float sidebar_x = k_margin;
		const float sidebar_y = k_margin;
		const float sidebar_height = height - k_margin * 2.f;

		const float content_x = sidebar_x + this->m_sidebar_width + k_gap;
		const float content_y = k_margin;
		const float content_width = width - content_x - k_margin;
		const float content_height = height - k_margin * 2.f;

		/* 2. ambient depth under the floating content card */
		draw::shadow(content_x, content_y, content_width, content_height, k_card_radius, 18.f, 0.f, 2.f, hue::c_color(0, 0, 0, 120));

		/* @shielded:
			the cursor, withheld from everything the account panel is covering.

			An immediate-mode view decides it was clicked by hit-testing the cursor
			itself, so a view drawn UNDER an open panel would still answer a click
			that never reached it — the search field sitting directly behind the
			panel would take focus through it, and the nav entries behind that would
			change category. There is no z-order to consult, so the shell removes
			the cursor from the input the covered views are rendered against and
			hands the real one only to the panel.

			The rect is the one the panel published last frame; it is the same
			one-frame-behind contract the drag handle is written against, and for
			the same reason — the layout that produces it has not run yet. */
		const auto withhold = [](frame_input_t& target)
			{
				target.m_cursor = math::c_vector_2d(-10000.f, -10000.f);
				target.m_left_down = false;
				target.m_left_clicked = false;
				target.m_right_clicked = false;
				target.m_wheel = 0.f;
			};

		/* The colour editor floats over EVERYTHING, including the account panel it
		   was opened from, so it shields in two stages: the panel gets a cursor
		   with the editor cut out of it, and the views underneath get one with both
		   cut out. */
		frame_input_t panel_input = this->m_input;
        if(bind_popup::active()) withhold(panel_input);

		{
			float px = 0.f, py = 0.f, pw = 0.f, ph = 0.f;

			if (colour_popup::rect(px, py, pw, ph) && panel_input.hovered(px, py, pw, ph))
				withhold(panel_input);
		}

		/* @and now the player list is over all of it.

		   It used to be the one surface that could not overlap anything: it lived
		   in a gutter the panel was forbidden to enter, so nothing was ever drawn
		   underneath it and nothing had to be shielded from it. Dragged freely it
		   can sit on the rail, the module grid, the header or the account panel —
		   and it is painted after every one of them (step 7b), so an immediate
		   mode view underneath would answer a click that visibly landed on a
		   player card.

		   list_input is taken BEFORE the cut, so the card itself still gets the
		   real cursor. The resulting order is popup > list > account panel >
		   views, which is the paint order read backwards, which is the only order
		   that can be right. */
		const auto displace = [](frame_input_t& target)
			{
				/* @the cursor goes away. The CLICK does not, and that is the
				   whole difference between this and withhold().

				   withhold() clears the buttons as well, and for the account
				   overlay that is right: it is a transient modal, up only while
				   it is open, and a press on it should not read to the views
				   behind it as a press at all.

				   This card is permanent — it is drawn on every category, on
				   every frame the panel is up — so clearing the button here
				   would mean a click on the player list NEVER AGAIN reaches the
				   panel as a miss. A miss is how every caret and every flyout in
				   this client is dismissed: c_sidebar's search runs
				   set_search_focused(hovered) on any left click, c_color_setting
				   drops the hex caret and the value chip the same way, and the
				   search-history flyout closes on a click that is not on it.
				   Swallow the button and the rail's search stays focused for
				   ever — which through c_app::text_focus() keeps gl_hook eating
				   W/A/S/D, so clicking a player card would leave the user unable
				   to walk until they clicked back inside the panel.

				   Nothing can be falsely HIT by a click that survives this,
				   because every one of those sites is guarded by a hovered()
				   test against a cursor that is now ten thousand pixels away.
				   The wheel still goes, because dispatch_keyboard has already
				   routed it and a second consumer would double-scroll.

				   m_left_down, though, MUST go, and for the opposite reason.
				   m_left_clicked is only ever an ARMING flag; m_left_down is the
				   CONTINUATION flag, and every latched drag in this client reads
				   the cursor with no hover test at all once it is latched — the
				   slider thumbs, the colour picker's bars, the rail's nav
				   reorder. Leave the button down and sweeping this card over a
				   slider mid-drag feeds it a cursor ten thousand px to the left,
				   which slams the value to its minimum. Cleared, those drags see
				   the button released and let go cleanly, which is exactly what
				   withhold() already does for the account overlay. */
				target.m_cursor    = math::c_vector_2d(-10000.f, -10000.f);
				target.m_left_down = false;
				target.m_wheel     = 0.f;
			};

		frame_input_t list_input = panel_input;

		/* On the boolean, not on the fade: for the ~200ms it is dissolving, the card is a ghost
		   and the rail underneath it is the thing you are actually looking at. Shielding until
		   the last pixel is gone would eat the first click of whatever you switched it off to
		   get to. */
		if (players_on && panel_input.hovered(players_cx, players_cy, players_w, players_h))
			displace(panel_input);

		frame_input_t shielded = panel_input;

		{
			float panel_x = 0.f, panel_y = 0.f, panel_w = 0.f, panel_h = 0.f;

			if (this->m_sidebar.overlay_rect(panel_x, panel_y, panel_w, panel_h)
				&& shielded.hovered(panel_x, panel_y, panel_w, panel_h))
			{
				withhold(shielded);
			}
		}

		/* 3. sidebar card */
		const int search_matches = this->m_modules->count_matching(this->m_sidebar.search_text());

		this->m_sidebar.render(sidebar_x, sidebar_y, this->m_sidebar_width, sidebar_height,
			search_matches, shielded);

		/* 4. content card */
		draw::rounded_rect(content_x, content_y, content_width, content_height, k_card_radius,
			theme::pane(theme::bg_card, 0.42f));
		draw::border(content_x, content_y, content_width, content_height, k_card_radius, 1.f, theme::border);

		/* 5. the page that owns the content card */
		if (this->on_settings_page())
		{
			this->m_settings->render(content_x, content_y, content_width, content_height, shielded);
		}
		else
		{
			this->m_modules->render(
				this->m_category, this->m_sidebar.search_text(), filter_label(this->m_header.filter()),
				content_x, content_y + k_header_height, content_width, content_height - k_header_height,
				shielded
			);
		}

		/* 6. header, so the config drawer always overlays the grid.
		   the chat page draws its own bar, the module chrome would mean nothing there. */
		if (this->on_module_page())
		{
			int active_modules = 0;
			for (const auto& entry : this->m_modules->modules())
			{
				if (entry->enabled())
					++active_modules;
			}

			this->m_header.render(this->m_category, active_modules, content_x, content_y, content_width, k_header_height, shielded);
		}

		/* 7. the account panel, over everything the client draws.

		   Late for the same reason it is shielded: it opens upwards out of a card
		   at the bottom of the sidebar and is wider than the sidebar itself, so any
		   earlier position would put it under the content card. */
		this->m_sidebar.render_overlay(panel_input);

		/* 7b. the player list, OUTSIDE the panel and to its left.

		   Drawn at a NEGATIVE canvas x, into the gutter composition_width()
		   reserves for it. The clip rect must NOT intersect the one already on
		   the stack: ImGui seeds every draw list's clip stack with the canvas
		   rect, and this surface sits deliberately outside it, so an
		   intersecting push would scissor the whole thing away.

		   The roster is walked here rather than inside the view. A view that
		   fetches its own data does it once per draw and gets no say in whether
		   it was worth doing; gated on visibility, a hidden list costs nothing. */
		if (players_live)
		{
			/* Not walked while the list is away. The gate was already there for a hidden
			   panel; a switched-off list is the same argument — a few hundred JNI calls a
			   frame for a surface nobody is looking at. */
			g_roster->poll_if_due(ImGui::GetTime(), this->m_visible && players_on);

			/* Where the card put itself, converted into canvas space at the top of
			   paint() — see the note there. Its UNDRAGGED value is the placement
			   that used to be spelled out here: canvas -k_players_extent, and a y
			   that centres it against the panel rather than pinning it to the top,
			   because two cards of different heights sharing a top edge read as one
			   having been cut short.

			   This surface's glass is queued at the top of paint(), in the same
			   composite as the panel's, off these same numbers. */
			const float fade = anim::saturate(this->m_players_reveal);

			/* Canvas px. The slide is applied to the ORIGIN the card is drawn from, so the clip
			   rect below travels with it and nothing is sheared by a scissor it has drifted
			   out of. */
			const float px = players_cx - (1.f - fade) * 26.f;
			const float py = players_cy;
			const float pw = players_w;
			const float ph = players_h;

			/* @the clip goes on THE LIST THE PRIMITIVES ACTUALLY USE.

			   ImGui::PushClipRect pushes onto the CURRENT WINDOW's draw list.
			   Everything drawn here goes through g_render->draw_list(), which is
			   the BACKGROUND list (render.cc: set_draw_list(GetBackgroundDrawList)),
			   so a window-level push is a no-op for it — and that list's own stack
			   is still seeded with ImRect(0, 0, DisplaySize), the canvas. This
			   surface is not bound by that rect: undragged it lies entirely at
			   NEGATIVE x, outside it, and dragged it can be anywhere at all. The
			   first version of this scissored the whole thing away and drew
			   nothing.

			   Not intersecting is the other half, and it matters MORE now than it
			   did. Intersecting used to give an empty rect; it would now give a
			   rect that is correct at some positions and shears the card at
			   others, which is a far worse failure to read off a screenshot. The
			   absolute push is right in every position. */
			ImDrawList* dl = g_render->draw_list();

			dl->PushClipRect(ImVec2(px - 40.f, py - 40.f),
				ImVec2(px + pw + 40.f, py + ph + 40.f),
				/*intersect_with_current=*/false);

			/* @the fade is applied to the FINISHED vertices, not threaded through the view.

			   The same trick gl_hook plays on the whole buffer for the panel's own entrance,
			   scoped to one surface: remember where this list's vertices start, let the view
			   draw exactly as it always does, then scale the alpha of everything it added.

			   The alternative is teaching sixty draw calls about an opacity none of them care
			   about — and every one of them would be a place for the fade to be forgotten the
			   next time a control is added to the card. This cannot be forgotten: it is one
			   loop over one range, and it covers whatever was drawn.

			   The range is contiguous because every primitive in this file lands on that one
			   background list, in order, and nothing else draws between the two marks. */
			const int first_vertex = dl->VtxBuffer.Size;

			this->m_players.render(px, py, pw, ph, list_input);

			if (fade < 0.999f)
			{
				for (int v = first_vertex; v < dl->VtxBuffer.Size; ++v)
				{
					ImDrawVert& vertex = dl->VtxBuffer[v];

					const unsigned int a = static_cast<unsigned int>(
						((vertex.col >> IM_COL32_A_SHIFT) & 0xFF) * fade);

					vertex.col = (vertex.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
				}
			}

			dl->PopClipRect();
		}

		/* 8. and the colour editor over that, on the real cursor.

		   Absolutely last, and deliberately not owned by whatever opened it: it is
		   a window, it can be dragged anywhere in the canvas, and the thing being
		   recoloured is usually the thing it needs to be sitting next to rather
		   than inside. */
		colour_popup::render(this->m_input);
        bind_popup::render(this->m_input);
	}

	void c_app::frame()
	{
		/* Sync the HUD-editor lifecycle with the panel visibility and the current
		   tab. Three bugs have lived here:

		     * Leaving the category at "HUD" across a hide, so reopening the panel
		       snapped straight back into the editor overlay.

		     * After that, clicking "HUD" in the sidebar was a no-op, because the
		       callback's "same category, ignore" short-circuit hit before it could
		       reactivate the editor.

		     * Rebinding on the HIDE transition, which fixed both of those and
		       introduced a flash: the rebind lands one frame ahead of the paint
		       decision at the bottom of this function, so the frame that dismissed
		       the panel became the first frame that painted it.

		   The rebind therefore happens on the SHOW transition, where it is ahead of
		   the same paint decision for the frame the user is about to look at, and
		   the hidden frames in between paint nothing at all. */
		{
			const bool prev_visible = this->m_visible_prev;
			this->m_visible_prev = this->m_visible;

			/* Closing the panel ends whatever was being typed into it. Without this
			   the field kept its focus across the hide, so the search you were in the
			   middle of was never recorded — you would reopen the client and find the
			   recents list had not noticed the last thing you looked for. */
			if (prev_visible && !this->m_visible)
			{
				this->m_sidebar.set_search_focused(false);

				/* The player list keeps a caret of its own and is now FIRST in
				   the keyboard chain. Its field cannot drop focus by itself
				   across a hide — the fade zeroes the click before render_search
				   ever sees it — so left focused it would still be first in line
				   when the panel came back, and the next thing typed would
				   disappear into an off-screen filter instead of the module
				   search. */
				this->m_players.set_search_focused(false);

				/* Every key and button ImGui still believes is held was pressed
				   while the panel was open; its release is going to the game now,
				   not to us. Left alone, a stuck Escape re-fires through
				   IsKeyPressed(repeat) the moment the panel comes back and shuts
				   any popup listening for it, and a stuck mouse button eats the
				   first click after reopen. The WndProc forwards releases while
				   closed as well; this is the belt for a release that never
				   arrives (focus lost before the key went up, the panic key).
				   Runs here, on the render thread inside the ImGui frame — NOT in
				   set_visible/toggle_visible, which the hotkey poll thread calls. */
				ImGuiIO& io = ImGui::GetIO();
				io.ClearInputKeys();
				io.ClearInputMouse();

				/* Independent of paint(), which the HUD page skips. */
				bind_popup::close();
			}

			/* @rebound on the way BACK IN, not on the way out.

			   Doing it on the hide transition put the category change one frame
			   ahead of the paint decision below, so the single frame that dismissed
			   the panel was also the first frame that painted it: the whole client
			   flashed into view at ~74% opacity showing the pre-HUD category, then
			   faded away. Leaving the category alone while the panel is hidden keeps
			   the HUD branch selected, nothing is painted, and the editor's own fade
			   finishes undisturbed.

			   And tell the rail, which is the bug the user actually reported. The
			   category used to move here with the sidebar never being told, because
			   the only thing that had ever moved its highlight was a click on it. So
			   Escape out of the HUD page and reopen: the rail lit up "HUD" over a
			   Combat list, the editor was not running, and clicking HUD did nothing
			   because the shell already thought you had left it. It looked exactly
			   like being in a section you are not in. */
			if (!prev_visible && this->m_visible && this->on_hud_page())
			{
				this->m_category = this->m_pre_hud_category.empty()
					? std::string{ "Combat" } : this->m_pre_hud_category;
				this->m_modules->on_category_changed(this->m_category);
				this->m_sidebar.sync_category(this->m_category);
			}
		}
		vanta::hud_editor::set_active(this->m_visible && this->on_hud_page());
		vanta::hud_editor::tick(ImGui::GetIO().DeltaTime);

		/* The inject sequence is NOT ticked here. It has to outrank the client's
		   own HUD, and the arraylist and watermark queue their text on the same
		   foreground list after this function returns — so gl_hook plays it once
		   they are done, as the last thing in the frame. */

		/* @the panel does not appear, it arrives.

		   Eased on its own clock rather than on the toggle, and the frame loop keeps
		   painting for as long as anything is still on screen — a panel that stops
		   being drawn the instant the key is pressed has no way to leave. */
		/* @the panel does not appear, it arrives — and it leaves the instant it
		   is told to.

		   Asymmetric on purpose. Opening is an arrival and may take its time;
		   closing answers a keypress and has to feel immediate. A spring
		   accelerates, decelerates and settles, which is what arriving looks
		   like; an exponential's greatest speed is at t = 0, so it flashes and
		   then creeps — which is what this was. */
		const float rdelta = std::min(ImGui::GetIO().DeltaTime, 0.1f);

		if (this->m_visible)
		{
			/* 400/38 is wn 20 with zeta 0.95: near critical, so there is a real
			   velocity profile and no bounce. Half travelled by ~90 ms. */
			this->m_travel = anim::saturate(this->m_open.update(1.f, 400.f, 38.f, rdelta));

			/* ALPHA LEADS THE GEOMETRY. Opaque by travel 0.55, about 105 ms,
			   while the panel is still gliding the last 45% into place. That is
			   the cure for the layered-alpha x-ray described on open_travel():
			   the phase you can see through is cut to a fraction of what it was,
			   and the rest of the entrance is a solid object moving. */
			this->m_reveal = anim::ease_out_quart(anim::saturate(this->m_travel / 0.55f));
		}
		else
		{
			/* A dismissal must not bounce, so the close is exponential rather
			   than sprung. The square front-loads the fade: gone to the eye at
			   about 75 ms while the geometry quietly finishes behind it. */
			this->m_open.m_value    = anim::lerp(this->m_open.m_value, 0.f, 26.f, rdelta);
			this->m_open.m_velocity = 0.f;
			this->m_travel = anim::saturate(this->m_open.m_value);
			this->m_reveal = this->m_travel * this->m_travel;
		}

		/* saturate() above is not tidiness. spring_t::update returns an
		   unbounded value, and an overshoot of 1.02 squared is 1.04 — which
		   through gl_hook's alpha multiply is 255 * 1.04 = 265, wrapping in a
		   byte to 9, and the whole panel flashes near-invisible for one frame. */

		if (this->m_visible && this->m_travel > 0.999f)
		{
			this->m_open.snap(1.f);
			this->m_travel = 1.f;
			this->m_reveal = 1.f;
		}

		/* @a hidden panel has nothing waiting for a key.

		   Done here rather than in set_visible because set_visible is called from
		   the hotkey poll thread by the INSERT toggle, and the bind pills belong
		   to the render thread — this is it. It also has to sit ABOVE the early
		   return below: once the panel is fully hidden the shell stops
		   dispatching, so a pill left armed would silently swallow the first key
		   pressed after the panel came back. */
		if (!this->m_visible)
			c_module::cancel_binding();

		if (!this->m_visible && this->m_travel < 0.004f)
		{
			this->m_open.snap(0.f);
			this->m_travel = 0.f;
			this->m_reveal = 0.f;
			return;
		}

		g_render->set_draw_list(ImGui::GetBackgroundDrawList());

		/* Place the 1120x700 canvas inside the game's framebuffer.

		   This is all that is left of what used to be a whole subsystem:
		   window tracking, z-order, focus stealing, minimise handling, drag
		   clamping and a geometry animation existed only because the panel
		   lived in a window of its own. Drawing inside the game's frame makes
		   every one of those the game's problem. */
		int fb_w = 0, fb_h = 0;
		core::gl_hook::framebuffer_size(fb_w, fb_h);
		if (fb_w <= 0 || fb_h <= 0)
			return;

		/* The COMPOSITION, panel plus reserved gutter. It has to go into both
		   denominators, not just this one: widening `want` alone lets the
		   requested scale exceed the fit ceiling, and std::clamp with lo > hi is
		   undefined — MSVC's implementation terminates the process on it. */
		const float comp_w = this->composition_width();

		const float want = std::min(
			(static_cast<float>(fb_w) * k_fill_fraction) / comp_w,
			(static_cast<float>(fb_h) * k_fill_fraction) / static_cast<float>(k_window_height));

		/* The largest the panel can be and still fit in the frame. Without this
		   the slider multiplies straight through: at 150% the canvas became 120%
		   of the framebuffer and the parts past the edge were scissored off. */
		const float fit = std::min(
			(static_cast<float>(fb_w) * k_fit_fraction) / comp_w,
			(static_cast<float>(fb_h) * k_fit_fraction) / static_cast<float>(k_window_height));

		const float requested = want * this->m_user_scale;
		const float ceiling   = std::min(k_max_scale, fit);

		/* k_min_scale is a FLOOR, and on a small enough window the fit ceiling
		   drops below it. clamp(v, 0.35, 0.30) is the same undefined inversion
		   as above; this was already latent, the gutter only makes it reachable
		   sooner. */
		const float lo_scale = std::min(k_min_scale, ceiling);
		const float target   = std::clamp(requested, lo_scale, ceiling);
		this->m_scale_capped = requested > ceiling + 0.001f;

		/* Eased, not assigned. The slider moves in 5% steps and the fit cap
		   changes in one jump when the window is resized, so writing the target
		   straight in made the panel snap between sizes. Easing costs nothing in
		   correctness here: the size slider resolves its drag in desktop pixels
		   and the cursor is converted through whatever scale is current on the
		   frame it is read, so a moving scale never feeds back into its own
		   input. */
		const float delta = std::min(ImGui::GetIO().DeltaTime, 0.1f);

		if (!this->m_scale_settled)
		{
			// first frame: adopt the fitted size rather than animating up to it
			this->m_panel_scale  = target;
			this->m_scale_settled = true;
		}
		else
		{
			this->m_panel_scale = anim::lerp(this->m_panel_scale, target, 16.f, delta);

			// land exactly, so the panel does not creep for ever at the tail
			if (std::fabs(target - this->m_panel_scale) < 0.0005f)
				this->m_panel_scale = target;
		}

		/* Publish the atlas-scale request for the next frame. gl_hook picks it up BEFORE
		   the next NewFrame and only then rebakes — Fonts->Locked between NewFrame and
		   Render, so we must not touch the atlas from inside the frame we are painting. */
		g_fonts->set_pending_atlas_scale(target);

		this->place(fb_w, fb_h);

		/* The list's own placement, re-clamped every frame off the scale that was
		   just settled — so a window resize or a ctrl+wheel cannot leave it
		   hanging off the frame. It does not depend on m_panel_x, which is the
		   whole reason dragging the panel no longer moves it. */
		this->place_players(fb_w, fb_h);

		this->collect_input();

		/* @the list is asked FIRST, and the order is the whole arbitration.

		   It is painted LAST of everything but the colour editor, so once it can
		   be dragged on top of the panel its handle overlaps the top margin, the
		   brand band and the header band. Whichever is tested first wins the
		   press, so the one on top has to be the one asked first or the user
		   drags a window they cannot see.

		   Each refuses to start while the other is latched, so exactly one of the
		   two is ever live and neither can be stolen mid-drag. */
		this->handle_players_drag(fb_w, fb_h);
		this->handle_window_drag(fb_w, fb_h);

		/* A window drag holds the left button down while the cursor sweeps over
		   whatever the panel is sitting on. Hiding the button from the widgets
		   for the duration is what stops that sweep from grabbing every slider
		   it crosses. */
		if (this->m_dragging || this->m_players_dragging)
		{
			this->m_input.m_left_down = false;
			this->m_input.m_left_clicked = false;
			this->m_input.m_right_clicked = false;
		}

		/* A panel on its way out is still drawn but is no longer a control surface.
		   Without this a click landing during the fade would hit whatever happened
		   to be under it on the frame it was dismissed. */
		if (!this->m_visible)
		{
			this->m_input.m_left_down = false;
			this->m_input.m_left_clicked = false;
			this->m_input.m_right_clicked = false;
			this->m_input.m_wheel = 0.f;
		}

		this->dispatch_keyboard();

		/* HUD mode owns the whole screen — skip the normal panel paint and
		   let the editor take over. The lifecycle sync at the top of
		   frame() has already ticked hud_editor, so render alone here. */
		if (this->on_hud_page()) {
			vanta::hud_editor::render(fb_w, fb_h);
		} else {
			this->paint();
			// Still render the editor pass so a mid-fade-out completes when
			// the user just tabbed away from HUD.
			vanta::hud_editor::render(fb_w, fb_h);
		}
	}

	void c_app::shutdown()
	{
		this->m_settings.reset();
		this->m_modules.reset();

		emoji::shutdown();
		panel_glass::shutdown();
		profile::release_head_texture();

		/* @The camera goes back before anything else.

		   A client unloaded mid-spectate would otherwise leave the player looking
		   out of somebody else's head with nothing left running to put it back —
		   and the only way out of that is to rejoin the world. */
		{
			vanta::jvm::JniScope s;
			if (s) vanta::modules::spectate::shutdown(s.get());
		}

		/* @chams first, because it unhooks the driver.

		   It patches the ICD's own draw, program and texture stubs and installs
		   a process-wide exception handler, and nothing was calling this — the
		   patches and the handler outlived the DLL. The first inject was fine;
		   the second landed on trampolines pointing into freed memory. It has to
		   run before gl_hook tears its own hooks down, which is why it is here
		   and not in the module's onDisable. */
		vanta::modules::chams::shutdown();

		/* The Outline's own JNI cache, released while a JNIEnv is still to be
		   had. Its framebuffers and shaders go with the pipeline object. */
		vanta::modules::targetesp::attack::shutdown();
		vanta::modules::outline::shutdown();

		/* Same reason, one layer out: the Target HUD's head cache owns a texture
		   per name it has drawn, and this is the last point in the process where
		   the GL context is still current. */
		vanta::modules::targethud::heads::shutdown();
		vanta::modules::targethud::destroy();
		g_icons->destroy();
		core::scrub_paths();
	}

	void c_app::reload_module_cards()
	{
		if (this->m_modules)
			this->m_modules->reload_cards();
		this->m_sidebar.sync_from_theme();
	}
}
