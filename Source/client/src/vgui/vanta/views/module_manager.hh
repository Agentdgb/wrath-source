#pragma once

/* @vanta::c_module_manager:
	owns the module database and paints the content card: the scrolling module grid on the left
	and the settings sidecar that slides in over the right hand side.
*/
namespace vanta
{
	class c_module_manager
	{
	public:
		c_module_manager();

		/* @on_category_changed:
			resets the scroll and re-arms the spring entrance for the incoming category
		*/
		void on_category_changed(const std::string& category);

		/* @on_scroll:
			wheel delta, clamped so the list can never scroll past its top
		*/
		void on_scroll(float wheel);

		/* @render:
			paints the grid, then the sidecar on top of it
		*/
		void render(const std::string& category, const std::string& search, const std::string& filter_tab,
			float x, float y, float w, float h, const frame_input_t& input);

		/* @open_sidecar / @close_sidecar:
			drawer state, the manager keeps the last opened module around so it can animate out
		*/
		/* @wipe_category:
			turns every module in one category off and forgets its keybind.

			Lives here rather than in the shell because "which modules are in this
			category" is a question only the database can answer, and answering it
			correctly means the same case-insensitive comparison the grid filters
			with — not a second copy of it somewhere else that drifts.
		*/
		void wipe_category(const std::string& category);

		void open_sidecar(const std::shared_ptr<c_module>& module_);
		void close_sidecar();
		void reload_cards();

		const std::vector<std::shared_ptr<c_module>>& modules() const { return this->m_modules; }

		/* @count_matching:
			how many modules a search query hits, across every category. the search field shows it,
			and asking the manager keeps one definition of what "matches" means.
		*/
		int count_matching(const std::string& query) const;

	private:
		void build_default_modules();
		void render_sidecar(const std::shared_ptr<c_module>& module_, float x, float y, float w, float h,
			float sidecar_width, const frame_input_t& input, float progress, float travel);

		std::vector<std::shared_ptr<c_module>> m_modules{};

		/* @reordering.

		   Grabbed by a GRIP on the card's left edge, not by the card itself. The
		   card's own body already means something — a left click toggles the module
		   — and a press that has to wait and see whether it becomes a drag cannot
		   also fire on contact. A grip has no second meaning, so there is nothing
		   to disambiguate: press it and you are dragging.

		   m_drag is the card being carried; m_drag_slot is where it would land if
		   dropped now. The other cards need no state at all — they are laid out in
		   the provisional order every frame, and their own springs carry them to
		   the slot that order gives them. */
		std::shared_ptr<c_module> m_drag{};
		float m_drag_offset{ 0.f };
		int   m_drag_slot{ -1 };

		void commit_reorder(const std::vector<std::shared_ptr<c_module>>& order);

		float m_scroll{ 0.f };
		float m_target_scroll{ 0.f };

		std::string m_category{ "Combat" };
		float m_category_transition{ 1.f };

		std::shared_ptr<c_module> m_sidecar{};
		std::shared_ptr<c_module> m_last_open{};

		anim::spring_t m_sidecar_anim{};
		float m_back_hover{ 0.f };

		/* Sidecar (drawer) scroll — routed independently from the grid
		   scroll when the cursor is over the drawer, so a wheel spin
		   over a long settings list scrolls that list rather than the
		   grid underneath. */
		float m_sidecar_scroll{ 0.f };
		float m_target_sidecar_scroll{ 0.f };
		float m_sidecar_content_height{ 0.f };   // measured on last render
		float m_sidecar_body_height{ 0.f };      // measured on last render

		/* @the same pair for the GRID, and for the same reason.

		   on_scroll runs outside the render, so it cannot see how many cards the
		   category has or how tall the viewport is. The sidecar already solved
		   this by publishing both from its draw; the grid never did, which is
		   why its wheel had a top stop and no bottom one. */
		float m_grid_content_height{ 0.f };      // measured on last render
		float m_grid_body_height{ 0.f };         // measured on last render
		/* Cached sidecar rectangle — filled by render_sidecar so
		   on_scroll can detect whether the cursor is over the drawer. */
		float m_sidecar_rect_x{ 0.f };
		float m_sidecar_rect_y{ 0.f };
		float m_sidecar_rect_w{ 0.f };
		float m_sidecar_rect_h{ 0.f };
	};
}
