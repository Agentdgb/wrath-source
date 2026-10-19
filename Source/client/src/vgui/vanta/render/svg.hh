#pragma once

/* @vanta::svg:
	self contained svg -> antialiased mask rasterizer.

	the gui ships its icons as vector sources so they stay crisp at any size, this is the
	replacement for the awt based rasterizer the reference implementation used.
	supported geometry: path (M L H V C S Q T Z, absolute and relative), rect (with rx),
	circle, ellipse, line, polygon and polyline. arcs are not used by any shipped icon.
*/
namespace vanta::svg
{
	/* @bitmap_t:
		straight rgba8 buffer, top left origin, ready to be uploaded as a texture
	*/
	struct bitmap_t
	{
		int width{};
		int height{};
		std::vector<std::uint8_t> pixels{};

		bool valid() const { return this->width > 0 && this->height > 0 && !this->pixels.empty(); }
	};

	/* @fit_mode:
		fit    - optically centers the artwork inside the canvas, matching the reference scaling rules
		stretch- maps the document view box straight onto the canvas
	*/
	enum class fit_mode : int
	{
		fit = 0,
		stretch
	};

	/* @rasterize_source:
		renders svg markup into a pure white mask whose alpha carries the antialiased coverage
	*/
	bool rasterize_source(const std::string& source, int canvas, bitmap_t& out,
		fit_mode mode = fit_mode::fit, bool flip_horizontal = false);

	/* @rasterize_file:
		same as above, reading the markup from disk
	*/
	bool rasterize_file(const std::string& path, int canvas, bitmap_t& out,
		fit_mode mode = fit_mode::fit, bool flip_horizontal = false);
}
