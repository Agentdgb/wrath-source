#pragma once

/* @vanta::c_header:
	the strip that sits on top of the content card.

	[ A-Z | Active ] segmented control, the bookmark drawer that expands into a config list,
	and the eraser button pinned to the far right.
*/
namespace vanta
{
	enum class filter_mode : int
	{
		alphabetical = 0,
		active
	};

	inline const char* filter_label(filter_mode mode)
	{
		return mode == filter_mode::active ? "Active" : "A-Z";
	}

	class c_header
	{
	public:
		/* @render:
			paints the header and consumes its own input.
			active_modules feeds the metrics strip, it is the only number the header cannot read itself.
		*/
		void render(const std::string& current_category, int active_modules,
			float x, float y, float w, float h, const frame_input_t& input);

		/* @set_wipe_callback:
			what the eraser does.

			A callback rather than a reference to the module list: the header knows
			what category is on screen and nothing else about modules, and handing
			it the database so it can walk it would make a strip of chrome the owner
			of the client's state.
		*/
		void set_wipe_callback(std::function<void()> callback) { this->m_wipe = std::move(callback); }

		filter_mode filter() const { return this->m_filter; }
		const std::string& selected_config() const { return this->m_selected_config; }

		/* @players_shown:
			is the player list up.

			Owned HERE, and read by the shell, rather than the other way round. It is the same
			shape filter() and selected_config() already have: this strip is where the control
			lives, so this is where the state lives, and the shell asks. A callback pair would
			mean the header had to be told its own button's state back before it could draw it.
		*/
		bool players_shown() const { return this->m_players_on; }

		/* @drag_rect:
			the empty span of the bar, in canvas coordinates — everything between the
			config drawer and the reset button, which is chrome and nothing else.

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

		std::function<void()> m_wipe{};

		filter_mode m_filter{ filter_mode::alphabetical };
		anim::spring_t m_slide{};

		std::string m_selected_config{ "default" };
		std::vector<std::string> m_configs{ "[Save current]", "default", "legit", "hypixel", "blatant" };

		bool m_config_expanded{ false };
		anim::spring_t m_config_expand{};
		bool m_dropdown_open{ false };
		anim::spring_t m_dropdown{};

		float m_config_hover{ 0.f };
		float m_eraser_hover{ 0.f };

		/* the player-list switch, left of the reset button. m_people_glow is what it is DRAWN
		   from, so the button eases between its two looks instead of snapping. */
		bool  m_players_on{ false };
		float m_people_hover{ 0.f };
		float m_people_glow{ 0.f };
		float m_collapse_arrow_hover{ 0.f };

		/* one shot feedback, fired the moment each control is actually used */
		anim::pulse_t m_filter_pulse{};
		anim::pulse_t m_drawer_pulse{};
		anim::pulse_t m_config_pulse{};
		anim::pulse_t m_eraser_pulse{};
		anim::pulse_t m_people_pulse{};

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
