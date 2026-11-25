#pragma once

/* @dt::anim:
	the easing set the gui animates with.
	lerp() is framerate independent, every widget stores its own progress value and pulls it
	towards a target each frame, exactly like the reference implementation does.
*/
namespace dt::anim
{
	/* @lerp:
		exponential smoothing. speed is "how aggressively" we converge, delta is the frame time.
	*/
	inline float lerp(float current, float target, float speed, float delta) {
		const float factor = 1.f - std::exp(-speed * delta);
		return current + (target - current) * factor;
	}

	inline float saturate(float t) {
		return std::clamp(t, 0.f, 1.f);
	}

	/* vape v4 cosine ease in out: -(cos(pi * t) - 1) / 2 */
	inline float ease_in_out_sine(float t) {
		t = saturate(t);
		return static_cast<float>(-(std::cos(3.14159265358979323846 * t) - 1.0) / 2.0);
	}

	inline float ease_out_cubic(float t) {
		t = saturate(t);
		const float f = t - 1.f;
		return f * f * f + 1.f;
	}

	inline float ease_out_quart(float t) {
		t = saturate(t);
		const float f = t - 1.f;
		return 1.f - f * f * f * f;
	}

	inline float ease_out_quint(float t) {
		t = saturate(t);
		return static_cast<float>(1.0 - std::pow(1.0 - t, 5.0));
	}

	inline float ease_out_expo(float t) {
		t = saturate(t);
		return (t == 1.f) ? 1.f : static_cast<float>(1.0 - std::pow(2.0, -10.0 * t));
	}

	/* tuned snappy spring bounce */
	inline float ease_out_back(float t) {
		t = saturate(t);
		constexpr float c1 = 1.4f;
		constexpr float c3 = c1 + 1.f;
		const float f = t - 1.f;
		return 1.f + c3 * (f * f * f) + c1 * (f * f);
	}

	inline float ease_in_out_cubic(float t) {
		t = saturate(t);
		return t < 0.5f
			? 4.f * t * t * t
			: static_cast<float>(1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0);
	}

	/* @pulse_t:
		a one shot ripple.

		springs and lerps describe a state; this describes an event. it is kicked the moment an
		action lands and decays on its own, which is what lets a control acknowledge a click
		without that acknowledgement becoming part of its resting appearance.
	*/
	struct pulse_t
	{
		float m_value{ 0.f };

		/* @kick:
			fires the ripple. calling it again restarts it rather than stacking.
		*/
		void kick(float strength = 1.f)
		{
			this->m_value = std::max(this->m_value, saturate(strength));
		}

		/* @update:
			decays towards rest and hands back the current strength
		*/
		float update(float delta, float speed = 7.5f)
		{
			this->m_value = lerp(this->m_value, 0.f, speed, delta);

			if (this->m_value < 0.002f)
				this->m_value = 0.f;

			return this->m_value;
		}

		bool active() const { return this->m_value > 0.002f; }
	};

	/* @spring_t:
		damped harmonic oscillator.

		an exponential lerp always decelerates into its target; a spring carries momentum, so a
		panel that slides in settles with a hint of overshoot instead of creeping to a stop.
		used for anything that travels a real distance: the settings drawer, the config list, the
		selection pill, the collapsing rail.
	*/
	struct spring_t
	{
		float m_value{ 0.f };
		float m_velocity{ 0.f };

		/* @update:
			integrates one frame towards the target and returns the new value.
			stiffness sets how hard it pulls, damping how quickly the momentum bleeds off.
		*/
		float update(float target, float stiffness, float damping, float delta)
		{
			// fixed substeps, an explicit integrator goes unstable on a long frame otherwise
			delta = std::clamp(delta, 0.f, 0.1f);

			constexpr float step_size = 1.f / 240.f;
			const int steps = std::max(1, static_cast<int>(std::ceil(delta / step_size)));
			const float step = delta / static_cast<float>(steps);

			for (int index = 0; index < steps; ++index)
			{
				const float force = (target - this->m_value) * stiffness - this->m_velocity * damping;

				this->m_velocity += force * step;
				this->m_value += this->m_velocity * step;
			}

			return this->m_value;
		}

		/* @snap:
			teleports the spring, used on the first frame so nothing animates in from zero
		*/
		void snap(float value)
		{
			this->m_value = value;
			this->m_velocity = 0.f;
		}

		bool at_rest(float target) const
		{
			return std::fabs(target - this->m_value) < 0.001f && std::fabs(this->m_velocity) < 0.001f;
		}
	};
}
