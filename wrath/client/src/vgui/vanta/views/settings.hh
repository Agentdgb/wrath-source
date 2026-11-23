#pragma once

/* @vanta::c_settings:
	the Configs page that owns the content card when the sidebar's "Configs"
	entry is selected.
*/
namespace vanta
{
	class c_settings
	{
	public:
		c_settings();

		/* @render:
			paints the Configs page and consumes its own input
		*/
		void render(float x, float y, float w, float h, const frame_input_t& input);

		void on_scroll(float wheel);

		bool input_focused() const { return this->m_input_focused; }
		void set_input_focused(bool focused) { this->m_input_focused = focused; }

		void on_character(char character);
		void on_backspace();
		void on_submit();
		void paste_clipboard();
		void select_all() { this->m_select_all = true; }

		void refresh_profiles();

	private:
		void render_section(const char* title, float x, float& cursor_y, float w, float alpha);

		std::string m_new_name{};
		std::string m_rename_from{};
		bool m_select_all{false};
		bool m_input_focused{ false };
		float m_input_hover{ 0.f };
		anim::spring_t m_input_focus_anim{};

		float m_create_hover{ 0.f };
		anim::pulse_t m_create_pulse{};
		double m_create_done_until{ -10.0 };

		float m_save_current_hover{ 0.f };
		anim::pulse_t m_save_current_pulse{};
		double m_save_current_done_until{ -10.0 };

		std::array<float, 4> m_preset_hover{};
		std::string m_status_text{};
		double m_status_until{ -10.0 };

		std::vector<std::string> m_profiles{};
		double m_last_refresh_time{ -10.0 };

		struct row_anim_t
		{
			float m_row_hover{ 0.f };
			float m_load_hover{ 0.f };
			float m_save_hover{ 0.f };
			float m_share_hover{ 0.f };
			float m_rename_hover{ 0.f };
			float m_del_hover{ 0.f };
			double m_saved_until{ -10.0 };
			anim::pulse_t m_pulse{};
		};
		std::vector<row_anim_t> m_row_anims{};

		float m_scroll{ 0.f };
		float m_target_scroll{ 0.f };
		float m_max_scroll{ 0.f };
		float m_entrance{ 0.f };
	};
}
