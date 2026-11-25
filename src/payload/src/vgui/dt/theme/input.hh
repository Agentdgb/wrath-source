#pragma once

/* @dt::frame_input_t:
	the input snapshot every widget is rendered against.

	the gui is immediate mode: a widget receives the cursor plus the one shot click flags for the
	current frame, decides whether it was hit, and reports back. nothing is retained between frames
	except the animation progress each widget owns.
*/
namespace dt
{
	struct frame_input_t
	{
		math::c_vector_2d m_cursor{};

		bool m_left_down{ false };
		bool m_left_clicked{ false };
		bool m_right_clicked{ false };

		float m_wheel{ 0.f };
		float m_delta{ 0.f };

		/* @hovered:
			axis aligned hit test against the cursor
		*/
		bool hovered(float x, float y, float w, float h) const
		{
			return this->m_cursor.x >= x && this->m_cursor.x <= x + w
				&& this->m_cursor.y >= y && this->m_cursor.y <= y + h;
		}
	};
}
