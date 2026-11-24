#pragma once

/* @vanta::c_player_list:
	the people on the server, drawn OUTSIDE the panel and to its left.

	it is not a page. every other surface in this client swaps into the content card and only one
	is up at a time; this one is up whenever the panel is, on every category, which is why the
	shell draws it at a NEGATIVE canvas x rather than routing it through m_category.

	that negative x is the whole placement trick. gl_hook's canvas-to-framebuffer transform is a
	plain affine with no bounds test, so a vertex at canvas x = -656 lands at panel_x - 656*s:
	left of the panel, at the panel's scale, fading with it when it opens.

	it no longer FOLLOWS the panel. the shell keeps this card's position in framebuffer pixels of
	its own and converts it back into a canvas x on the way in, so the canvas coordinate this view
	is handed compensates for wherever the panel was dragged to and the two move separately.
	-656 is now only the default. the gutter c_app::composition_width() reserves still sizes the
	scale fit, but it is no longer a no-go area for the panel: this card clamps itself to the
	framebuffer instead, and either surface can be moved out of the other's way.

	being free to overlap is why this view now publishes drag_rect(). it is drawn LAST of
	everything except the colour editor, so the shell hit-tests its handle before the panel's and
	withholds the cursor from every surface it happens to be sitting on.

	@LAYOUT, and why the numbers are derived rather than typed.

	the card is a landscape 640 x 418. inside it the grid is sized to a WHOLE number of cards and
	the detail column takes whatever is left, so there is never a strip of dead gutter between
	the last column and the divider — the arithmetic is in render() and it closes exactly.
*/
namespace vanta
{
	class c_player_list
	{
	public:
		void render(float x, float y, float w, float h, const frame_input_t& input);
		void on_scroll(float wheel);

		/* @hovered:
			is the cursor over this surface.

			The shell routes the wheel by asking every scrollable surface in turn, and this one
			is not a page — there is no category to test against. It answers from the rect it
			was last drawn into, which is the only place that rect is known. */
		bool hovered(const frame_input_t& input) const;

		/* @drag_rect:
			the header strip, which is this card's title bar, in the card's OWN
			LOCAL coordinates — (0, 0) is its top-left corner, wherever the shell
			has put it this frame.

			LOCAL, and that is the one place this deviates from c_sidebar's
			brand_rect and c_header's drag_rect. Those publish absolute canvas
			coordinates because their owners sit at a fixed spot inside the panel,
			so a rect that is one frame stale is a rect that has not moved. This
			card is DRAGGED: its absolute position changes on the very frame the
			handle is being hit-tested, so an absolute rect published last frame
			would lag the cursor for the whole drag. The shell already knows where
			it is about to draw the card, so it adds the origin itself.

			Recorded by render() rather than returned as a constant, so the 42px
			header height stays defined exactly once, in render()'s own arithmetic.
			Zero height until the list has been rendered at least once.

			The strip holds the title and the count and nothing else: the search
			sits on its own bar below it, and the divider rule and the detail
			column both start at its bottom edge. So the handle cannot swallow a
			press meant for the field, a card or the detail tabs.
		*/
		void drag_rect(float& x, float& y, float& w, float& h) const
		{
			x = this->m_drag[0]; y = this->m_drag[1];
			w = this->m_drag[2]; h = this->m_drag[3];
		}

		/* the shell's keyboard chain, the same contract the sidebar's module search uses */
		void on_character(char character);
		void on_backspace();
		bool search_focused() const { return this->m_search_focused; }
		void set_search_focused(bool focused) { this->m_search_focused = focused; }

		/* @selected:
			who the detail column is describing, or empty. the shell reads it so a selection can
			outlive a category change.
		*/
		const std::string& selected() const { return this->m_selected; }

	private:
		struct card_state_t
		{
			float m_hover{ 0.f };
			anim::spring_t m_appear{};
			bool  m_ready{ false };

			/* @where the card IS, as opposed to where it belongs.

			   Filtering re-slots every card at once, and jumping them straight
			   to their new grid position reads as the list being replaced rather
			   than filtered. Each card eases towards its slot from wherever it
			   was, so the ones that survive the filter visibly slide up into the
			   gaps the others left. Snapped, not eased, the first time a card is
			   seen -- otherwise every newcomer flies in from the origin. */
			float m_x{ 0.f }, m_y{ 0.f };
			bool  m_placed{ false };
		};

		card_state_t& state_for(const std::string& name);

		/* @render_card:
			one person in the grid. returns true when it was clicked this frame.
		*/
		bool render_card(const roster_entry_t& person, float x, float y, float w, float h,
			bool selected, const frame_input_t& input);

		void render_search(float x, float y, float w, float h, const frame_input_t& input);
		void render_detail(float x, float y, float w, float h, const frame_input_t& input);

		std::unordered_map<std::string, card_state_t> m_cards{};

		std::string m_search{};
		bool        m_search_focused{ false };
		anim::spring_t m_search_glow{};

		/* a NAME, not a pointer. the roster is a vector and it reallocates the moment somebody
		   new is remembered, so a pointer held across a frame is a dangling read waiting for the
		   next stranger to walk past. */
		std::string m_selected{};
        std::string m_detail_for{};
        int m_preview_tab{0}, m_name_choice{0};
        bool m_preview_drag{false};
        float m_preview_yaw{3.49f}, m_preview_last_x{0};
        double m_copy_until{0};
        std::string m_history_cape{},m_history_copy{};
        int m_cape_index{0},m_name_history_scroll{0};
        bool m_detail_hovered{false};

		float m_rect[4]{ 0.f, 0.f, 0.f, 0.f };   /* x, y, w, h as last drawn */

		/* the drag handle, LOCAL to m_rect — see drag_rect() */
		float m_drag[4]{ 0.f, 0.f, 0.f, 0.f };

		float m_scroll{ 0.f };
		float m_target_scroll{ 0.f };

		/* @the range filter: only people who are actually in the world.

		   m_only_nearby is the state; m_range_glow is what the button is drawn
		   from, so the toggle fades between its two looks instead of snapping.
		   m_range_hit is the rect it was last drawn into, recorded for the same
		   reason every other control here records one. */
		bool  m_only_nearby{ false };
		float m_range_glow{ 0.f };
		float m_range_hover{ 0.f };
		float m_range_hit[4]{ 0.f, 0.f, 0.f, 0.f };
	};
}
