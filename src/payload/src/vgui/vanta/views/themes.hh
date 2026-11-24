#pragma once

/* @vanta::c_themes_view:
	the Themes & Appearance page that owns the content card when the sidebar's
	"Themes" entry is selected.
*/
namespace vanta
{
	class c_boolean_setting;

	class c_themes_view
	{
	public:
		c_themes_view();

		/* @render:
			paints the Themes page and handles preset selection
		*/
		void render(float x, float y, float w, float h, const frame_input_t& input);

		void on_scroll(float wheel);

	private:
		void render_section(const char* title, float x, float& cursor_y, float w, float alpha);
		void render_theme_card(float cx, float cy, float cw, float ch,
			const theme::theme_preset_t& preset, bool selected, float hover,
			const frame_input_t& input, float alpha);

		float m_scroll{ 0.f };
		float m_target_scroll{ 0.f };
		float m_max_scroll{ 0.f };
		float m_entrance{ 0.f };

		struct card_state_t
		{
			float m_hover{ 0.f };
			anim::pulse_t m_pulse{};
		};
		std::array<card_state_t, 8> m_cards{};

		std::shared_ptr<c_boolean_setting> m_glass{};
		float m_reset_hover{ 0.f };
		anim::pulse_t m_reset_pulse{};
		double m_reset_done_until{ -10.0 };
	};
}
