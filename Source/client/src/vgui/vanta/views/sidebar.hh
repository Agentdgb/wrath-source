#pragma once

/* @vanta::c_sidebar:
	the floating navigation card.

	branding, the two navigation sections, the search field and the profile card. collapsing swaps
	it for a 54px rail plus a detached top card, everything in between is interpolated.
*/
namespace vanta
{
	struct nav_item_t
	{
		std::string m_title{};
		std::string m_icon{};

		bool m_selected{ false };
		float m_hover{ 0.f };
		float m_select{ 0.f };
		anim::pulse_t m_picked{};

		/* Where the row is actually drawn, eased towards where the layout says it
		   should be. Needed the moment the list became reorderable: without it a
		   row that changes place teleports, and a list that teleports gives no clue
		   which row went where. */
		float m_visual_y{ -1.f };

		nav_item_t(std::string title, std::string icon, bool selected)
			: m_title(std::move(title)), m_icon(std::move(icon)), m_selected(selected), m_select(selected ? 1.f : 0.f)
		{
		}
	};

	class c_sidebar
	{
	public:
		c_sidebar();

		using nav_callback_t = std::function<void(const nav_item_t&)>;

		void set_callback(nav_callback_t callback) { this->m_callback = std::move(callback); }

		/* @sync_category:
			moves the highlight to `title` WITHOUT firing the callback.

			select() is the click path, and it was the only thing that could move
			the highlight -- so every category the shell changed on its own (leaving
			the HUD page on Escape, the editor's own exit) left the rail still
			lighting up the entry it had left. The panel reopened showing "HUD"
			selected over a Combat list: the one state the user reads to know where
			they are, disagreeing with where they actually were.

			No callback, because the shell is the caller -- firing it would re-enter
			on_category_changed for a change the shell has already made. */
		void sync_category(const std::string& title);

		/* @render:
			paints the card and consumes its own input
		*/
		void render(float x, float y, float w, float h, int search_matches, const frame_input_t& input);

		/* @on_character / @on_backspace:
			text entry for the search field, fed by the platform layer
		*/
		void on_character(char character);
		void on_backspace();

		bool collapsed() const { return this->m_collapsed; }
		void toggle_collapsed() { this->m_collapsed = !this->m_collapsed; }

		bool search_focused() const { return this->m_search_focused; }

		/* @set_search_focused:
			focus, and the ONLY place a finished search is remembered.

			Leaving the field is what ends a search, so leaving the field is what
			records it. Enter was the wrong choice: filtering here is live, so the
			results are already on screen by the time the query is finished and
			nothing about the control asks for a keypress to confirm it. Almost
			nobody pressed it, so almost nothing was ever remembered.
		*/
		void set_search_focused(bool focused);

		/* @submit_search:
			remembers whatever is currently in the field.

			On a completed search and not on every keystroke, because a search is
			only worth remembering once the user has decided it was the search they
			meant — recording as they type would fill the list with every prefix of
			it. Idempotent and safe to call on an empty or one-character field; it
			decides for itself whether there is anything worth keeping.
		*/
		void submit_search();

		const std::string& search_text() const { return this->m_search_text; }
		const std::string& active_category() const { return this->m_active_category; }

		/* @collapse_progress:
			0 expanded, 1 collapsed. the shell drives the rail width from this so both stay in sync.
		*/
		float collapse_progress() const { return this->m_collapse.m_value; }

		/* ------------------------------------------------------------------ */
		/* the account panel                                                   */
		/* ------------------------------------------------------------------ */

		/* @render_overlay:
			paints the account panel, and nothing else.

			Split out of render() and called LAST by the shell because the panel
			opens upwards out of a card that lives at the bottom of the sidebar, and
			it is wider than the sidebar itself — drawn in place it would be painted
			over by the content card and by the search field below it. Everything
			that must sit on top of the whole client has to be drawn after the whole
			client.
		*/
		void render_overlay(const frame_input_t& input);

		/* @overlay_rect:
			the panel's rect as it was drawn on the last frame, or false when it is
			closed.

			Published for the same reason brand_rect is: the shell has to know where
			the panel is BEFORE it draws the views underneath it, so it can withhold
			the cursor from them. An immediate-mode view has no other way to be told
			that something it cannot see is covering it.
		*/
		bool overlay_rect(float& x, float& y, float& w, float& h) const;

		/* @set_account:
			the expiry the panel prints.

			Held here rather than read from anywhere because no auth layer publishes
			it yet; the panel is wired to this string so that when one does, it has a
			single place to write to.
		*/
		void set_account(std::string licence, std::string expires_on)
		{
			this->m_licence = std::move(licence);
			this->m_expires_on = std::move(expires_on);
		}

		/* @brand_rect:
			the logo + wordmark block, in canvas coordinates, as it was drawn on
			the last frame — the shell uses it as the panel's drag handle.

			Recorded rather than recomputed because the block slides and shrinks
			with the collapse spring: a second copy of that arithmetic would
			drift out of step with the drawing every time either side changed.
			The collapse button is deliberately excluded, so grabbing the handle
			can never also toggle the rail.
		*/
		void brand_rect(float& x, float& y, float& w, float& h) const
		{
			x = this->m_brand_x; y = this->m_brand_y;
			w = this->m_brand_w; h = this->m_brand_h;
		}

	private:
		struct typed_char_t
		{
			char m_character{};
			float m_anim{ 0.f };
		};

		void update(float delta);
		void render_branding(float x, float y, float w, const frame_input_t& input);
		void render_profile_panel(const frame_input_t& input);

		/* @colour_row:
			one "Accent / Sidebar / ..." line in the panel, plus the picker that
			accordions out of it when its swatch is clicked
		*/
		struct colour_row_t
		{
			std::string m_title{};
			std::shared_ptr<c_color_setting> m_picker{};

			/* where this row writes to. A pointer into the live palette rather than
			   a copy plus a switch statement: adding a fifth customisable surface
			   then costs one line here and nothing anywhere else. */
			hue::c_color* m_target{ nullptr };

			/* the accent is not one colour but a family derived from one, so it
			   cannot be written through m_target — see theme::set_accent */
			bool m_is_accent{ false };
		};

		void sync_colour_rows();
		void render_selection_pill(float x, float w, float item_height, const frame_input_t& input);
		void render_nav_item(nav_item_t& item, float x, float target_y, float w, float h,
			const frame_input_t& input, bool pinned = false);

		/* @reorder_nav:
			runs the drag for one navigation group and leaves the vector in whatever
			order the cursor currently implies.

			Per group, not across the whole rail: the two lists are separated by a
			divider because they mean different things, and a row dragged over that
			divider would be asking to become something it is not.
		*/
		void reorder_nav(std::vector<nav_item_t>& items, bool other, float x, float top,
			float w, float item_height, float stride, const frame_input_t& input);
		void render_profile_card(float x, float y, float w, float h, const frame_input_t& input);
		void render_search(float x, float y, float w, float h, int matches, const frame_input_t& input);

		/* @render_search_history:
			the recent-searches list that opens above the field. Returns the rect it
			covered, so the rows underneath can be told not to answer clicks that
			never reached them.
		*/
		void render_search_history(float x, float field_top, float w, const frame_input_t& input);

		void select(nav_item_t& item);

		std::vector<nav_item_t> m_main_items{};
		std::vector<nav_item_t> m_other_items{};

		std::string m_active_category{ "Combat" };
		nav_callback_t m_callback{};

		/* @carrying a nav row.

		   Identified by TITLE rather than by index or pointer. The vector is
		   reordered underneath while the drag is running, so an index means
		   something different every frame and a pointer is invalidated outright;
		   the title is the one handle that survives the thing it names moving. */
		std::string m_nav_drag{};
		bool  m_nav_drag_other{ false };
		float m_nav_grab{ 0.f };

		/* the press that has not yet decided whether it is a drag */
		std::string m_nav_press{};
		bool  m_nav_press_other{ false };
		float m_nav_press_y{ 0.f };

		bool m_collapsed{ false };
		anim::spring_t m_collapse{};
		float m_collapse_button_hover{ 0.f };
		float m_top_card_hover{ 0.f };

		/* the drag handle, published by render_branding each frame */
		float m_brand_x{ 0.f };
		float m_brand_y{ 0.f };
		float m_brand_w{ 0.f };
		float m_brand_h{ 0.f };

		/* the selection pill is a single element that travels between entries */
		anim::spring_t m_pill_y{};
		bool m_pill_ready{ false };
		float m_selected_y{ 0.f };

		std::string m_search_text{};
		std::vector<typed_char_t> m_typed{};
		bool m_search_focused{ false };
		anim::spring_t m_search_focus{};
		float m_search_hover{ 0.f };
		float m_cursor_x{ 0.f };
		float m_clear_hover{ 0.f };
		float m_clear_reveal{ 0.f };
		anim::pulse_t m_clear_pulse{};
		float m_matches_reveal{ 0.f };

		/* @recent searches.

		   Kept in most-recent-first order with duplicates lifted rather than
		   appended: searching the same thing twice should move it up the list, not
		   put it in twice. Capped, because a list nobody can see the bottom of is
		   not a list. */
		std::vector<std::string> m_history{};

		/* @what the panel is currently drawing: m_history narrowed to what the field
		   holds, so typing "t" leaves only the searches that began with t.

		   A snapshot rather than a view, because the panel outlives what it is
		   showing: emptying the list with the eraser, or typing a letter that nothing
		   continues, still has to leave something on screen to fade out. Sizing the
		   box from the live list instead made it snap from five rows to none on the
		   frame of the keystroke and then fade an empty box — the rows vanished under
		   the cursor that was reaching for them. Refreshed only while the panel is
		   wanted, so it is frozen for exactly the length of the fade. */
		std::vector<std::string> m_history_shown{};
		std::vector<float> m_history_hover{};

		anim::spring_t m_history_reveal{};
		float m_history_clear_hover{ 0.f };

		/* @pinned open by a right click.

		   Held separately from focus because the two mean different things: focus
		   says the field is taking keystrokes, this says the user asked to see what
		   they have searched before. Right-clicking is the gesture for "show me the
		   other thing about this control", and it works whether or not the field
		   already has something in it. */
		bool m_history_pinned{ false };

		/* the panel's rect, so the nav rows it covers can be shielded */
		float m_history_x{ 0.f };
		float m_history_y{ 0.f };
		float m_history_w{ 0.f };
		float m_history_h{ 0.f };
		bool  m_history_live{ false };

		float m_profile_hover{ 0.f };
		float m_timer{ 0.f };

		/* ---------------- account panel ---------------- */

		bool m_profile_expanded{ false };

		/* a spring, not a lerp: the panel travels a real distance out of the card
		   and has to settle with weight rather than creep to a stop */
		anim::spring_t m_profile_expand{};
		float m_profile_arrow_hover{ 0.f };

		/* the profile card's rect this frame, handed to the panel so it can hang
		   off the card without recomputing the collapse interpolation a second
		   time and drifting out of step with it */
		float m_card_x{ 0.f };
		float m_card_y{ 0.f };
		float m_card_w{ 0.f };

		/* published for the shell — see overlay_rect */
		float m_panel_x{ 0.f };
		float m_panel_y{ 0.f };
		float m_panel_w{ 0.f };
		float m_panel_h{ 0.f };
		bool  m_panel_live{ false };

		std::string m_licence{ "Lifetime" };
		std::string m_expires_on{ "1 Jan 2026" };

		std::vector<colour_row_t> m_colours{};

		/* the one appearance control that is not a colour */
		std::shared_ptr<c_boolean_setting> m_glass{};
        std::shared_ptr<c_boolean_setting> m_keybinds{};

		float m_reset_hover{ 0.f };
		anim::pulse_t m_reset_pulse{};

		/* @unload.

		   Two-stage, because it is the one control in the client that cannot be
		   undone: the first press arms it and it says so, the second goes through.
		   The arming lapses on its own so a button left armed by accident does not
		   stay a trap. */
		float m_destruct_hover{ 0.f };
		float m_destruct_armed{ 0.f };

		/* @menu activation keybind button at the bottom of the profile popup */
		bool  m_menu_key_binding{ false };
		float m_menu_key_hover{ 0.f };
		anim::pulse_t m_menu_key_pulse{};
		bool  m_menu_key_held[256]{};

	public:
		void sync_from_theme();
	};
}
