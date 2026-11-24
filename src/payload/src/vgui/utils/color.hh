#pragma once

/* @hue:
	color container the whole render engine works with.
	channels are kept as plain ints so the draw list macros can consume them directly.
*/
namespace hue
{
	class c_color
	{
	public:
		int r{ 255 }, g{ 255 }, b{ 255 }, a{ 255 };
	public:
		constexpr c_color() = default;
		constexpr c_color(int r, int g, int b) : r(r), g(g), b(b), a(255) { }
		constexpr c_color(int r, int g, int b, int a) : r(r), g(g), b(b), a(a) { }

		/* @transform:
			packs the color into the format the draw list expects
		*/
		ImU32 transform() const {
			return IM_COL32(
				std::clamp(this->r, 0, 255),
				std::clamp(this->g, 0, 255),
				std::clamp(this->b, 0, 255),
				std::clamp(this->a, 0, 255)
			);
		}

		/* @modulate:
			scales only the alpha channel, used by every opacity animation
		*/
		c_color modulate(float alpha) const {
			return c_color(this->r, this->g, this->b, static_cast<int>(this->a * std::clamp(alpha, 0.f, 1.f)));
		}

		/* @alpha:
			hard overrides the alpha channel
		*/
		c_color alpha(int value) const {
			return c_color(this->r, this->g, this->b, value);
		}

		c_color alpha(float scalar) const {
			return c_color(this->r, this->g, this->b, static_cast<int>(255.f * std::clamp(scalar, 0.f, 1.f)));
		}

		/* @lerp:
			blends every channel towards the target color
		*/
		c_color lerp(const c_color& target, float step) const {
			const float t = std::clamp(step, 0.f, 1.f);
			return c_color(
				static_cast<int>(this->r + (target.r - this->r) * t),
				static_cast<int>(this->g + (target.g - this->g) * t),
				static_cast<int>(this->b + (target.b - this->b) * t),
				static_cast<int>(this->a + (target.a - this->a) * t)
			);
		}

		bool operator==(const c_color& other) const {
			return this->r == other.r && this->g == other.g && this->b == other.b && this->a == other.a;
		}
		bool operator!=(const c_color& other) const { return !(*this == other); }
	};
}
