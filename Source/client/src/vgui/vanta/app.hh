#pragma once

/* @vanta::c_app:
	the client shell.

	owns the three surfaces the gui is made of (sidebar, header, module grid), lays them out as
	floating cards inside the borderless window, and drives the frame loop.
*/
namespace vanta
{
	class c_app
	{
	public:
		/* @initialize:
			brings the overlay, the render engine, the typography and the icon atlas up
		*/
		bool initialize();

		/* @frame:
			paint one frame. Called from the wglSwapBuffers detour, on the
			game's render thread, with our GL context current and an ImGui
			frame already begun. The client no longer owns a loop of its own —
			the game's frame rate is the client's frame rate.
		*/
		void frame();

		/* the panel's own visibility, toggled by the bind key */
		bool visible() const { return this->m_visible; }

		/* @is one of our own carets holding the keyboard.

		   gl_hook lets W/A/S/D/space/shift/ctrl fall through to the game while
		   the panel is open, gated on ImGui::GetIO().WantCaptureKeyboard —
		   which nothing in this client ever raises, because every field here is
		   hand-drawn and there is not an ImGui::InputText in the tree. So the
		   flag is false while you are typing, and searching for a player whose
		   name contains an 'a' or a 'd' walks you sideways into a wall.

		   Read from the window thread, not this one. It is a plain bool read of
		   a value that only ever changes on a click, so the worst a race can
		   cost is one movement key on the frame focus changed — and the
		   existing ImGui::GetIO() call it replaces was a far wider cross-thread
		   read than this. */
		bool text_focus() const
		{
			/* The hex field and the value chip count too: their alphabet is
			   0-9 a-f, so 'a' and 'd' are both legal input AND strafe keys. */
			return this->m_players.search_focused()
				|| this->m_sidebar.search_focused()
				|| (this->m_settings && this->m_settings->input_focused())
				|| c_color_setting::hex_focus()   != nullptr
				|| c_block_picker_setting::search_focus() != nullptr
                || c_slider_setting::value_focus() != nullptr;
		}

		/* @reveal:
			0 while the panel is away, 1 while it is up, and everything in between
			for the moment either side.

			Read by gl_hook, which is the only place a fade like this can live. The
			views draw in fixed colours and a hundred of them would each have to
			learn about it; the finished vertices, on the other hand, are one buffer
			with one alpha channel, and that buffer is already being walked to place
			the canvas in the frame.
		*/
		/* @the player list lives OUTSIDE the panel, to its left.

		   Authored in the same fixed canvas as everything else, just at a
		   NEGATIVE x. gl_hook's transform is a plain affine with no bounds test,
		   so canvas x = -456 lands at panel_x - 456*s: left of the panel, at the
		   panel's scale, inheriting the entrance fade with no new code at all.

		   It no longer inherits the DRAG. The list keeps a framebuffer position
		   of its own (m_players_x/y) and paint() converts that back into the
		   canvas coordinates the draw calls need, which is the only way to be
		   independent of a transform every vertex shares:

		       canvas_x = (m_players_x - m_panel_x) / m_panel_scale

		   so the canvas position COMPENSATES for wherever the panel was put.
		   -k_players_extent is now only the DEFAULT, the value that reproduces
		   the old fixed placement when the list has not been dragged.

		   The extent is a CONSTANT and must stay one. It feeds composition_width,
		   which feeds the scale fit, which feeds both the eased panel scale AND
		   g_fonts->set_pending_atlas_scale — so animating it would rebake the
		   font atlas on every frame of the entrance. It would also move the
		   composition centre, silently rewriting wherever the user dragged the
		   panel to. The list has no open state of its own; it arrives with the
		   panel because it fades with the same reveal. */
		/* LANDSCAPE. 640 x 400 is 1.6:1 — the 560 x 430 before it was 1.3:1,
		   which is nominally wider than tall and reads as a square, and the
		   440 x 700 before THAT was a column. The reference is a wide card with
		   the grid and the detail side by side; a tall one has to stack them or
		   starve both. The width buys a 3 x 112px grid with no dead gutter and
		   a 215px detail column; the height is exactly three rows of cards. */
		static constexpr int   k_players_width  = 640;   // canvas px
		/* 418: 42 of header, the 44px search field, the 16px gap under it and the
		   16px of bottom padding leave exactly 300 for the grid, which is three
		   whole 92px rows and their two 12px gaps. The arithmetic is in
		   player_list.cc's render(). */
		static constexpr int   k_players_height = 418;
		static constexpr int   k_players_gap    = 16;
		static constexpr float k_players_extent =
			static_cast<float>(k_players_width + k_players_gap);

		float composition_width() const
		{ return static_cast<float>(k_window_width) + k_players_extent; }

		/* ONE definition of where the composition sits. place() and
		   handle_window_drag each used to compute their own centre; if the two
		   disagree by so much as the reserved gutter, the panel teleports
		   sideways on every frame of a drag. */
		void composition_centre(int fb_w, int fb_h, float& cx, float& cy) const
		{
			const float pw  = static_cast<float>(k_window_width)  * this->m_panel_scale;
			const float ph  = static_cast<float>(k_window_height) * this->m_panel_scale;
			const float ext = k_players_extent * this->m_panel_scale;

			cx = (static_cast<float>(fb_w) - (pw + ext)) * 0.5f + ext;
			cy = (static_cast<float>(fb_h) -  ph)        * 0.5f;
		}

		/* @HOME: where the player list sits when it has not been dragged, in
		   FRAMEBUFFER pixels.

		   Beside composition_centre for the same reason it exists: place_players()
		   and handle_players_drag() both need it, and two definitions that
		   disagree make the card jump on every drag frame.

		   Derived from the composition centre, NOT from m_panel_x. That is the
		   whole point: the centre is a function of the framebuffer and the scale
		   only, so dragging the PANEL does not move the list's home and the list
		   stays exactly where it was left.

		   The numbers are the placement the shell used to hardcode, mapped
		   through the same affine: canvas -k_players_extent and canvas
		   (700 - 412) / 2, which is why an offset of zero reproduces today's
		   position exactly rather than merely approximately. */
		void players_home(int fb_w, int fb_h, float& hx, float& hy) const
		{
			float cx = 0.f, cy = 0.f;
			this->composition_centre(fb_w, fb_h, cx, cy);

			hx = cx - k_players_extent * this->m_panel_scale;
			hy = cy + (static_cast<float>(k_window_height)
				- static_cast<float>(k_players_height)) * 0.5f * this->m_panel_scale;
		}

		float reveal() const { return this->m_reveal; }

		/* @geometry has its own clock, and that is the whole fix.

		   The fade used to drive the motion as well, and the motion was 4% of
		   scale spent inside three frames — authored as "grows into place",
		   landing as a pop, with the fade being the only thing you actually
		   perceived.

		   Worse, the fade is a per-vertex alpha multiply over a STACK of opaque
		   layers, not a composite of the finished panel. At half opacity that is
		   not a half-transparent panel; it is six independently translucent
		   sheets with the drop shadow reading through the cards. So the cure is
		   to spend as little time there as possible: the alpha finishes early
		   and the rest of the entrance is a solid object still moving. */
		float open_travel() const { return this->m_travel; }

		/* Published rather than recomputed at each site, so the vertex path and
		   the blur callback — which is not vertices and never sees the vertex
		   transform — cannot drift apart. */
		float open_grow() const { return 0.94f + 0.06f * this->m_travel; }
		float open_rise() const { return 10.f * (1.f - this->m_travel); }   /* canvas px */
		/* What the entrance scales about, in CANVAS units. The centre of
		   panel-plus-reserved-gutter, because pivoting on the panel alone would
		   swing a surface lying entirely to its left in from off screen.

		   It stays a pure function of the CONSTANTS even now that the list can
		   be dragged out of that gutter. Three consumers read it — gl_hook's
		   vertex pass, render.cc's cursor un-transform and panel_glass — and the
		   un-transform inverts it exactly, so what is drawn and what is clickable
		   agree whatever the value is. Making it follow the list would be legal
		   arithmetic and the wrong behaviour: moving the LIST would change how
		   the PANEL arrives. The cost of leaving it is that the entrance's 6%
		   grow is taken about a point the list may now be far from, so a list
		   parked at the far edge swings in from about half as far again as it
		   does at home — enough, at that extreme, for the drag band to sit off
		   its own drawn header for the ~150 ms the entrance lasts. That still
		   reads as one composition arriving, and it settles on its own. */
		float pivot_x() const
		{ return -k_players_extent + this->composition_width() * 0.5f; }
		float pivot_y() const { return static_cast<float>(k_window_height) * 0.5f; }

		/* @set_visible is a plain flag, on purpose.

		   The INSERT hotkey calls it from the poll thread, so it must not reach
		   into anything the render thread owns. Disarming a bind pill on hide is
		   done by frame(), which is the render thread and can see the flag. */
		void set_visible(bool v) { this->m_visible = v; if (!v) c_block_picker_setting::clear_search_focus(); }
		void toggle_visible() { this->m_visible = !this->m_visible; }

		/* @shutdown:
			releases everything initialize() created
		*/
		void shutdown();

		/* the canvas the layout is authored against. Everything the views draw
		   is in this space; gl_hook transforms it into the game's framebuffer
		   after ImGui::Render(), so no view code knows about placement. */
		static constexpr int k_window_width = 1120;
		static constexpr int k_window_height = 700;

		/* share of the game's framebuffer the panel occupies at 100% */
		static constexpr float k_fill_fraction = 0.80f;

		/* the panel may never exceed this share of the framebuffer, whatever the
		   size slider says. Above 1.0 the panel is larger than the frame it is
		   drawn into: the overflow is scissored away by ImGui and simply
		   vanishes, so a slider that "does nothing" past a point is the good
		   outcome and a gui hanging off the screen is the bad one. Slightly
		   under 1 because the cards cast an 18px shadow past their own edge. */
		static constexpr float k_fit_fraction = 0.98f;

		static constexpr float k_min_scale = 0.35f;
		static constexpr float k_max_scale = 2.00f;

		/* where the canvas ended up inside the framebuffer, read by gl_hook to
		   transform the draw data and by the input layer to un-transform the
		   cursor */
		float panel_x() const { return this->m_panel_x; }
		float panel_y() const { return this->m_panel_y; }
		float panel_scale() const { return this->m_panel_scale; }

		/* true when the game window, not the slider, is deciding the size — the
		   settings page says so, because otherwise dragging the slider with
		   nothing happening reads as a broken control */
		bool scale_is_capped() const { return this->m_scale_capped; }

		/* the size slider's value, 1.0 == k_fill_fraction of the framebuffer */
		float m_user_scale{ 1.15f };

		/* the previous category, remembered when we enter HUD mode so the
		   "Exit" pill in the editor can return to exactly where the user
		   was rather than defaulting to Combat. */
		std::string m_pre_hud_category{ "Combat" };

		void reload_module_cards();

		/* Tracks the previous frame's visibility so we can detect the
		   hide transition and auto-return out of the HUD tab — otherwise
		   the user reopens the panel with category still on HUD and the
		   editor takes over unwantedly.

		   Seeded to match m_visible, so the very first frame is not read as a
		   hide transition that never happened. */
		bool m_visible_prev{ false };

	private:
		void collect_input();
		void dispatch_keyboard();
		void place(int fb_w, int fb_h);
		void handle_window_drag(int fb_w, int fb_h);

		/* The player list's own placement and its own drag. Same shape as the two
		   above and for the same reasons: resolved in framebuffer pixels, clamped
		   every frame so a resize or a scale change cannot strand it. */
		void place_players(int fb_w, int fb_h);
		void handle_players_drag(int fb_w, int fb_h);

		void paint();

		c_sidebar m_sidebar{};
		c_header m_header{};
		std::unique_ptr<c_module_manager> m_modules{};

		/* not a unique_ptr: it has no forward-declared dependency and it is up
		   whenever the panel is, so there is nothing to defer. */
		c_player_list m_players{};
		std::unique_ptr<c_settings> m_settings{};

		/* Configs swaps the module grid for a page of its own.
		   HUD is a special case: it takes over the whole screen (see hud_editor). */
		bool on_settings_page() const { return this->m_category == "Configs"; }
		bool on_hud_page() const { return this->m_category == "HUD"; }
		bool on_module_page() const
		{
			return !this->on_settings_page() && !this->on_hud_page();
		}

		frame_input_t m_input{};
		std::string m_category{ "Combat" };

		/* @closed on inject.

		   The client arrives over a running game, and arriving with the panel
		   already up puts a menu in front of someone who did not ask for one —
		   and captures their pointer with it, since the cursor guard follows this
		   flag. The inject sequence plays regardless (it is drawn from the hook,
		   not from the panel), so there is still something to see; the panel
		   itself waits for the toggle key. */
		bool m_visible{ false };

		/* eased visibility — see reveal() and open_travel() */
		float m_reveal{ 0.f };
		float m_travel{ 0.f };
		anim::spring_t m_open{};

		/* the panel is a fixed-size card placed inside the game's framebuffer;
		   these are its top-left corner and scale within it */
		float m_panel_x{ 0.f };
		float m_panel_y{ 0.f };
		float m_panel_scale{ 1.f };
		bool  m_scale_capped{ false };
		bool  m_scale_settled{ false };

		/* Where the user dragged the panel, as an offset from centred — stored
		   as an offset rather than an absolute position so that resizing the
		   game window, or changing the scale, keeps the panel where it was put
		   instead of teleporting it. */
		float m_offset_x{ 0.f };
		float m_offset_y{ 0.f };

		/* @the player list's own position, and it is INDEPENDENT of the panel's.

		   Framebuffer pixels, exactly like m_offset_x/y, and an offset from
		   players_home() rather than an absolute position for exactly the same
		   reason: resizing the game window or changing the scale must keep the
		   card where it was put.

		   Zero is not an arbitrary default. players_home() is the old hardcoded
		   placement mapped through the affine, so a fresh client puts the list
		   precisely where it has always been.

		   Not persisted, and that is a deliberate match rather than an omission:
		   ConfigManager serialises ModuleManager and nothing else, so the panel's
		   own offset and the size slider do not survive a restart either. A
		   window position joins that decision or it invents a second config
		   surface for one value. */
		float m_players_offset_x{ 0.f };
		float m_players_offset_y{ 0.f };

		/* Where that offset actually resolved to this frame, after clamping.
		   paint() converts it back into canvas space; the drag hit-tests against
		   it. Written by place_players(). */
		float m_players_x{ 0.f };
		float m_players_y{ 0.f };

		/* @how much of the card is on screen, 0 .. 1.

		   The header owns the SWITCH; this owns the ENTRANCE, because the card is drawn
		   outside the panel at a coordinate only the shell knows.

		   One is 1 rather than 0 because the list ships up: the switch starts latched, and a
		   reveal starting at zero would play the entrance once, unasked, on the first frame
		   the panel is opened. */
		float m_players_reveal{ 0.f };

		/* the rail animates between 200px expanded and 54px collapsed */
		float m_sidebar_width{ 200.f };

		bool m_dragging{ false };
		math::c_vector_2d m_drag_offset{};

		/* The list's latch. Exactly one of these two may be live: the list is
		   tested first and refuses to start while m_dragging is set, and
		   handle_window_drag returns immediately while this is set. */
		bool m_players_dragging{ false };
		math::c_vector_2d m_players_drag_offset{};
	};
	inline auto g_app = std::make_unique<c_app>();
}
