#pragma once

/* The strip above the module list. */
namespace dt
{
	class c_header
	{
	public:
		/* @render:
			paints the header and consumes its own input.
			active_modules feeds the metrics strip, it is the only number the header cannot read itself.
		*/
		void render(const std::string& current_category, int active_modules,
			float x, float y, float w, float h, const frame_input_t& input);

		/* Whether the player list is open. */
		bool players_shown() const { return false; }

		/* @drag_rect:
			the empty span before the right-side buttons, in canvas coordinates.

			Published rather than recomputed by the shell for the same reason
			brand_rect is: the header owns where its controls sit, and a second copy
			of that arithmetic living in app.cc would drift the first time a button
			moved. Zero width until the header has been rendered once.
		*/
		void drag_rect(float& x, float& y, float& w, float& h) const
		{
			x = this->m_drag_x;
			y = this->m_drag_y;
			w = this->m_drag_w;
			h = this->m_drag_h;
		}

	private:
		/* @render_metrics:
			the live strip that fills the empty middle of the bar. every number is measured, none
			of them are invented: frame rate off the io, working set off the process, and the
			enabled module count handed in by the shell.
		*/
		void render_metrics(float right_edge, float center_y, int active_modules, const frame_input_t& input);

		/* metrics strip */
		static constexpr int k_history = 72;

		anim::spring_t m_fps{};
		anim::spring_t m_active{};
		float m_memory{ 0.f };

		std::array<float, k_history> m_fps_history{};
		int m_fps_cursor{ 0 };
		bool m_history_ready{ false };

		float m_sample_timer{ 0.f };
		float m_memory_timer{ 0.f };

		/* the draggable span, measured during render */
		float m_drag_x{ 0.f };
		float m_drag_y{ 0.f };
		float m_drag_w{ 0.f };
		float m_drag_h{ 0.f };
	};
}
