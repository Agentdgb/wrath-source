#pragma once

/* @dt::c_icon_atlas:
	owns every texture the gui draws: the vector icon set and the branding artwork.

	textures are baked per display size rather than once at a high resolution: an icon asked for
	at 17px is rasterized at exactly 17x17, so its texels land one to one on screen pixels and the
	antialiasing comes from the rasterizer instead of from a minification filter. that is what
	keeps small geometric icons (the hud grid, the misc grid) from breaking up.

	vector sources are kept in memory as markup, bitmap sources as decoded rgba, so a new size can
	be produced at any point without touching the disk again.
*/
namespace dt
{
	class c_icon_atlas
	{
	public:
		/* @initialize:
			reads every source off disk. no gpu work happens here, variants are built on demand.
		*/
		void initialize();

		/* @destroy:
			releases every shader resource view we created
		*/
		void destroy();

		/* @get:
			texture for a name at an exact pixel size, baked on first use and cached afterwards
		*/
		ImTextureID get(const std::string& name, int pixel_size);

		/* @dimensions:
			resolution of the source artwork, before any resampling
		*/
		math::c_vector_2d dimensions(const std::string& name) const;

		/* @adopt:
			registers an already decoded rgba buffer under a name, at its own resolution.
			used for artwork that arrives at runtime and is not square, such as chat attachments.
		*/
		ImTextureID adopt(const std::string& name, const std::uint8_t* pixels, int width, int height);

	private:
		struct source_t
		{
			bool m_vector{ false };

			/* vector */
			std::string m_markup{};
			bool m_flip{ false };
			svg::fit_mode m_fit{ svg::fit_mode::fit };

			/* bitmap */
			std::vector<std::uint8_t> m_pixels{};
			int m_width{};
			int m_height{};

			/* 0 uses the source as authored. anything above crops the transparent margin away and
			   scales the artwork so its longest side takes that fraction of the tile, which is how
			   differently padded sources end up reading at the same weight. */
			float m_fit_ratio{ 0.f };
		};

		bool add_vector(const std::string& name, const std::string& path, bool flip_horizontal = false);
		bool add_markup(const std::string& name, std::string markup, svg::fit_mode fit);
		bool add_image(const std::string& name, const std::string& path, bool as_mask, float fit);

		GLuint bake(const source_t& source, int pixel_size) const;
		GLuint upload(const std::uint8_t* pixels, int width, int height) const;

		std::unordered_map<std::string, source_t> m_sources{};
		std::unordered_map<std::string, GLuint> m_variants{};
	};
	inline auto g_icons = std::make_unique<c_icon_atlas>();

	/* @dt::image:
		rgba8 helpers shared by the atlas and anything else that has to scale artwork
	*/
	namespace image
	{
		/* @resize:
			high quality resample of a whole rgba buffer into another size
		*/
		void resize(const std::vector<std::uint8_t>& source, int source_width, int source_height,
			std::vector<std::uint8_t>& destination, int target_width, int target_height);
	}
}
