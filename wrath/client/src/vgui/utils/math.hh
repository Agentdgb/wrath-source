#pragma once

/* @math:
	minimal vector / rect primitives the render engine and the gui are built on top of.
	kept header only, everything is trivially copyable so it can live on the stack.
*/
namespace math
{
	class c_vector_2d
	{
	public:
		float x{}, y{};
	public:
		constexpr c_vector_2d() = default;
		constexpr c_vector_2d(float x, float y) : x(x), y(y) { }
		c_vector_2d(const ImVec2& vec) : x(vec.x), y(vec.y) { }

		/* @transform:
			converts our vector into the type the draw list expects
		*/
		ImVec2 transform() const {
			return ImVec2(this->x, this->y);
		}

		c_vector_2d operator+(const c_vector_2d& other) const { return c_vector_2d(this->x + other.x, this->y + other.y); }
		c_vector_2d operator-(const c_vector_2d& other) const { return c_vector_2d(this->x - other.x, this->y - other.y); }
		c_vector_2d operator*(float scalar) const { return c_vector_2d(this->x * scalar, this->y * scalar); }
		c_vector_2d operator/(float scalar) const { return c_vector_2d(this->x / scalar, this->y / scalar); }

		c_vector_2d& operator+=(const c_vector_2d& other) { this->x += other.x; this->y += other.y; return *this; }
		c_vector_2d& operator-=(const c_vector_2d& other) { this->x -= other.x; this->y -= other.y; return *this; }
		c_vector_2d& operator*=(float scalar) { this->x *= scalar; this->y *= scalar; return *this; }

		bool operator==(const c_vector_2d& other) const { return this->x == other.x && this->y == other.y; }
		bool operator!=(const c_vector_2d& other) const { return !(*this == other); }

		float length() const { return std::sqrt(this->x * this->x + this->y * this->y); }
		bool empty() const { return this->x == 0.f && this->y == 0.f; }
	};

	class c_rect
	{
	public:
		c_rect() = default;
		c_rect(const c_vector_2d& pos, const c_vector_2d& size) : m_pos(pos), m_size(size) { }
		c_rect(float x, float y, float w, float h) : m_pos(x, y), m_size(w, h) { }

		const c_vector_2d& pos() const { return this->m_pos; }
		const c_vector_2d& size() const { return this->m_size; }

		float left() const { return this->m_pos.x; }
		float top() const { return this->m_pos.y; }
		float right() const { return this->m_pos.x + this->m_size.x; }
		float bottom() const { return this->m_pos.y + this->m_size.y; }

		bool contains(const c_vector_2d& point) const {
			return point.x >= this->left() && point.x <= this->right()
				&& point.y >= this->top() && point.y <= this->bottom();
		}
	private:
		c_vector_2d m_pos{}, m_size{};
	};

	/* @lerp:
		framerate independent interpolation, mirrors the exponential smoothing the gui uses everywhere
	*/
	inline float lerp(float current, float target, float speed, float delta) {
		const float factor = 1.f - std::exp(-speed * delta);
		return current + (target - current) * std::clamp(factor, 0.f, 1.f);
	}

	inline float saturate(float value) {
		return std::clamp(value, 0.f, 1.f);
	}
}
