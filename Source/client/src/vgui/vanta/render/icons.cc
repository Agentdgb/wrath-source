#include "../../includes.hh"
#include "../../framework/render/stb/stb_image.hh"

namespace vanta
{
	namespace
	{
		/* how much of its tile a cropped bitmap icon occupies. the category glyphs sit inside the
		   same optical box as the vector set; the wordmark is a logotype and gets the whole tile. */
		constexpr float k_glyph_fit = 0.78f;

		constexpr const char* k_combat_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">)"
			/* blade running bottom-left to top-right */
			R"(<path d="M50 8 L56 8 L56 14 L30 40 L24 34 Z" fill="#FFFFFF"/>)"
			R"(<path d="M22 36 L28 42 L20 50 L14 44 Z" fill="#FFFFFF"/>)"
			R"(<path d="M9 47 L17 55 L13 59 L5 51 Z" fill="#FFFFFF"/>)"
			/* and its mirror, bottom-right to top-left */
			R"(<path d="M14 8 L8 8 L8 14 L34 40 L40 34 Z" fill="#FFFFFF"/>)"
			R"(<path d="M42 36 L36 42 L44 50 L50 44 Z" fill="#FFFFFF"/>)"
			R"(<path d="M55 47 L47 55 L51 59 L59 51 Z" fill="#FFFFFF"/>)"
			R"(</svg>)";
		/* @the Spotify HUD's transport marks.

			Authored as vector and rasterised at the exact size they are drawn,
			which is the reason this atlas exists at all -- see the note at the
			top of icons.hh. They were hand-placed triangles and AddLine
			segments before, built at twelve pixels inside a thirty pixel
			button, and at that size the seams between segments and the
			crossing of the shuffle turned to mush.

			A 64-unit box throughout, so a glyph's optical weight is set here
			rather than at every call site. */
		/* A head and a pair of shoulders, on the same 64-unit box as the rest so its optical
		   weight matches them without tuning at the call site. Deliberately not the two-person
		   "friends" mark: that one is already the sidebar's Friends category, and a header
		   button wearing a nav icon reads as a shortcut to that page. */
		constexpr const char* k_user_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">)"
			R"(<circle cx="32" cy="21" r="13" fill="#FFFFFF"/>)"
			R"(<path d="M32 37 C18.7 37 8 46.6 8 58.4 L8 60 L56 60 L56 58.4)"
			R"( C56 46.6 45.3 37 32 37 Z" fill="#FFFFFF"/>)"
			R"(</svg>)";

		/* see the registration below for why the notch is there */
		constexpr const char* k_user_range_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">)"
			R"(<circle cx="26" cy="20" r="13" fill="#FFFFFF"/>)"
			R"(<path d="M26 36 C13 36 4 45 4 55 L4 58 L36 58 L36 48 L30 48)"
			R"( A8 8 0 0 1 30 40 L44 40 C44 37 42 36 40 36 Z" fill="#FFFFFF"/>)"
			R"(<rect x="42" y="34" width="6" height="26" rx="3" fill="#FFFFFF"/>)"
			R"(<rect x="32" y="44" width="26" height="6" rx="3" fill="#FFFFFF"/>)"
			R"(</svg>)";

		constexpr const char* k_tr_play_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">)"
			/* Centroid on the centre line, not the bounding box: a triangle
			   centred on its box reads as leaning left, because its mass is
			   not where its box is. */
			R"(<path d="M22 13 L52 32 L22 51 Z" fill="#FFFFFF"/>)"
			R"(</svg>)";

		constexpr const char* k_tr_pause_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">)"
			R"(<rect x="20" y="13" width="9" height="38" rx="4" fill="#FFFFFF"/>)"
			R"(<rect x="35" y="13" width="9" height="38" rx="4" fill="#FFFFFF"/>)"
			R"(</svg>)";

		/* Two triangles and no stop bar: the bar makes it "skip to the end of
		   the queue", the pair alone is the mark every player uses. */
		constexpr const char* k_tr_next_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">)"
			R"(<path d="M13 14 L33 32 L13 50 Z" fill="#FFFFFF"/>)"
			R"(<path d="M31 14 L51 32 L31 50 Z" fill="#FFFFFF"/>)"
			R"(</svg>)";

		constexpr const char* k_tr_prev_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">)"
			R"(<path d="M51 14 L31 32 L51 50 Z" fill="#FFFFFF"/>)"
			R"(<path d="M33 14 L13 32 L33 50 Z" fill="#FFFFFF"/>)"
			R"(</svg>)";

		/* @shuffle: two lanes that SWAP, not an X.

			Each path enters FLAT on the left, crosses, and leaves FLAT on the
			right under an arrow head. Those flat runs are the whole difference
			between a shuffle mark and a close button, and they are what the
			hand-drawn version lost once its segments were three pixels long. */
		constexpr const char* k_tr_shuffle_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg" )"
			R"(fill="none" stroke="#FFFFFF" stroke-width="7">)"
			R"(<path d="M4 44 L17 44 L41 20 L46 20"/>)"
			R"(<path d="M4 20 L17 20 L41 44 L46 44"/>)"
			R"(<path d="M43 11 L62 20 L43 29 Z" fill="#FFFFFF" stroke="none"/>)"
			R"(<path d="M43 35 L62 44 L43 53 Z" fill="#FFFFFF" stroke="none"/>)"
			R"(</svg>)";

		/* Two opposite corner brackets -- the arrows-out mark without the
		   arrows, which do not survive this size. */
		constexpr const char* k_tr_expand_markup =
			R"(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg" )"
			R"(fill="none" stroke="#FFFFFF" stroke-width="7">)"
			R"(<path d="M23 8 L8 8 L8 23"/>)"
			R"(<path d="M41 56 L56 56 L56 41"/>)"
			R"(</svg>)";

		constexpr float k_logo_fit = 1.f;

		std::string variant_key(const std::string& name, int pixel_size)
		{
			return name + "#" + std::to_string(pixel_size);
		}

		/* @crop_to_content:
			trims fully transparent margins so differently padded source art reads at the same weight
		*/
		void crop_to_content(const std::vector<std::uint8_t>& source, int width, int height,
			int& out_x, int& out_y, int& out_width, int& out_height)
		{
			int min_x = width, min_y = height, max_x = -1, max_y = -1;

			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					if (source[(static_cast<std::size_t>(y) * width + x) * 4 + 3] > 20)
					{
						min_x = std::min(min_x, x);
						min_y = std::min(min_y, y);
						max_x = std::max(max_x, x);
						max_y = std::max(max_y, y);
					}
				}
			}

			if (max_x < min_x || max_y < min_y)
			{
				out_x = 0; out_y = 0; out_width = width; out_height = height;
				return;
			}

			out_x = min_x;
			out_y = min_y;
			out_width = max_x - min_x + 1;
			out_height = max_y - min_y + 1;
		}

		/* @resample:
			separable triangle filter whose radius follows the scale factor, so a 1254px logo landing
			on a 30px tile is properly area averaged instead of point sampled.
			alpha is premultiplied while filtering, otherwise transparent texels bleed their color in.
		*/
		void resample(const std::vector<std::uint8_t>& source, int source_width, int source_height,
			int region_x, int region_y, int region_width, int region_height,
			std::vector<float>& destination, int target_width, int target_height)
		{
			const auto build_taps = [](int source_extent, int target_extent, std::vector<std::vector<std::pair<int, float>>>& taps)
				{
					const float scale = static_cast<float>(source_extent) / static_cast<float>(target_extent);
					const float radius = std::max(1.f, scale);

					taps.assign(static_cast<std::size_t>(target_extent), {});

					for (int index = 0; index < target_extent; ++index)
					{
						const float center = (static_cast<float>(index) + 0.5f) * scale - 0.5f;

						const int first = static_cast<int>(std::floor(center - radius));
						const int last = static_cast<int>(std::ceil(center + radius));

						float total = 0.f;
						for (int sample = first; sample <= last; ++sample)
						{
							const float weight = 1.f - std::fabs((static_cast<float>(sample) - center) / radius);
							if (weight <= 0.f)
								continue;

							taps[index].emplace_back(std::clamp(sample, 0, source_extent - 1), weight);
							total += weight;
						}

						if (total <= 0.f)
						{
							taps[index].clear();
							taps[index].emplace_back(std::clamp(static_cast<int>(center), 0, source_extent - 1), 1.f);
							total = 1.f;
						}

						for (auto& tap : taps[index])
							tap.second /= total;
					}
				};

			std::vector<std::vector<std::pair<int, float>>> horizontal{}, vertical{};
			build_taps(region_width, target_width, horizontal);
			build_taps(region_height, target_height, vertical);

			/* pass 1: horizontal, into a premultiplied intermediate */
			std::vector<float> intermediate(static_cast<std::size_t>(target_width) * region_height * 4u, 0.f);

			for (int y = 0; y < region_height; ++y)
			{
				for (int x = 0; x < target_width; ++x)
				{
					float accumulated[4]{};

					for (const auto& [sample, weight] : horizontal[static_cast<std::size_t>(x)])
					{
						const std::size_t index = (static_cast<std::size_t>(region_y + y) * source_width + (region_x + sample)) * 4u;
						const float alpha = source[index + 3] / 255.f;

						accumulated[0] += source[index + 0] * alpha * weight;
						accumulated[1] += source[index + 1] * alpha * weight;
						accumulated[2] += source[index + 2] * alpha * weight;
						accumulated[3] += source[index + 3] * weight;
					}

					const std::size_t target = (static_cast<std::size_t>(y) * target_width + x) * 4u;
					for (int channel = 0; channel < 4; ++channel)
						intermediate[target + channel] = accumulated[channel];
				}
			}

			/* pass 2: vertical */
			destination.assign(static_cast<std::size_t>(target_width) * target_height * 4u, 0.f);

			for (int y = 0; y < target_height; ++y)
			{
				for (int x = 0; x < target_width; ++x)
				{
					float accumulated[4]{};

					for (const auto& [sample, weight] : vertical[static_cast<std::size_t>(y)])
					{
						const std::size_t index = (static_cast<std::size_t>(sample) * target_width + x) * 4u;

						for (int channel = 0; channel < 4; ++channel)
							accumulated[channel] += intermediate[index + channel] * weight;
					}

					const std::size_t target = (static_cast<std::size_t>(y) * target_width + x) * 4u;
					for (int channel = 0; channel < 4; ++channel)
						destination[target + channel] = accumulated[channel];
				}
			}
		}

		/* @compose:
			un-premultiplies a filtered buffer back into rgba8 at an offset inside a canvas
		*/
		void compose(const std::vector<float>& filtered, int filtered_width, int filtered_height,
			std::vector<std::uint8_t>& canvas, int canvas_width, int offset_x, int offset_y)
		{
			for (int y = 0; y < filtered_height; ++y)
			{
				for (int x = 0; x < filtered_width; ++x)
				{
					const std::size_t source = (static_cast<std::size_t>(y) * filtered_width + x) * 4u;
					const std::size_t target = (static_cast<std::size_t>(offset_y + y) * canvas_width + (offset_x + x)) * 4u;

					if (target + 3 >= canvas.size())
						continue;

					const float alpha = filtered[source + 3] / 255.f;

					if (alpha > 0.0001f)
					{
						for (int channel = 0; channel < 3; ++channel)
							canvas[target + channel] = static_cast<std::uint8_t>(std::clamp(filtered[source + channel] / alpha, 0.f, 255.f));
					}

					canvas[target + 3] = static_cast<std::uint8_t>(std::clamp(filtered[source + 3], 0.f, 255.f));
				}
			}
		}

		std::optional<std::string> read_file(const std::string& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream.is_open())
				return std::nullopt;

			return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		}
	}

	GLuint c_icon_atlas::upload(const std::uint8_t* pixels, int width, int height) const
	{
		if (pixels == nullptr || width <= 0 || height <= 0)
			return 0;

		/* Baked at its display size, so no mips. CLAMP_TO_EDGE stops the atlas
		   bleeding neighbouring icons at the seams when the panel is scaled. */
		GLuint tex = 0;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F /*GL_CLAMP_TO_EDGE*/);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glBindTexture(GL_TEXTURE_2D, 0);

		return tex;
	}

	GLuint c_icon_atlas::bake(const source_t& source, int pixel_size) const
	{
		if (pixel_size <= 0)
			return 0;

		if (source.m_vector)
		{
			svg::bitmap_t bitmap{};
			if (!svg::rasterize_source(source.m_markup, pixel_size, bitmap, source.m_fit, source.m_flip) || !bitmap.valid())
				return 0;

			return this->upload(bitmap.pixels.data(), bitmap.width, bitmap.height);
		}

		int region_x = 0, region_y = 0;
		int region_width = source.m_width, region_height = source.m_height;

		if (source.m_fit_ratio > 0.f)
			crop_to_content(source.m_pixels, source.m_width, source.m_height, region_x, region_y, region_width, region_height);

		/* how big the artwork itself is allowed to be inside the tile */
		const float budget = static_cast<float>(pixel_size) * (source.m_fit_ratio > 0.f ? source.m_fit_ratio : 1.f);
		const float largest = static_cast<float>(std::max(region_width, region_height));

		const int target_width = std::clamp(static_cast<int>(std::lround(region_width * budget / std::max(1.f, largest))), 1, pixel_size);
		const int target_height = std::clamp(static_cast<int>(std::lround(region_height * budget / std::max(1.f, largest))), 1, pixel_size);

		std::vector<float> filtered{};
		resample(source.m_pixels, source.m_width, source.m_height,
			region_x, region_y, region_width, region_height, filtered, target_width, target_height);

		std::vector<std::uint8_t> canvas(static_cast<std::size_t>(pixel_size) * pixel_size * 4u, 0u);
		compose(filtered, target_width, target_height, canvas, pixel_size,
			(pixel_size - target_width) / 2, (pixel_size - target_height) / 2);

		return this->upload(canvas.data(), pixel_size, pixel_size);
	}

	bool c_icon_atlas::add_vector(const std::string& name, const std::string& path, bool flip_horizontal)
	{
		std::optional<std::string> markup{};
		if (const auto embedded = core::find_embedded_asset(path); embedded.valid())
		{
			markup.emplace(reinterpret_cast<const char*>(embedded.data), embedded.size);
		}
		else
		{
			markup = read_file(core::asset_path(path));
		}

		if (!markup.has_value())
		{
			slog::log::error("icons: could not open {}", path);
			return false;
		}

		source_t source{};
		source.m_vector = true;
		source.m_markup = std::move(*markup);
		source.m_flip = flip_horizontal;
		source.m_fit = svg::fit_mode::fit;

		this->m_sources[name] = std::move(source);
		return true;
	}

	bool c_icon_atlas::add_markup(const std::string& name, std::string markup, svg::fit_mode fit)
	{
		source_t source{};
		source.m_vector = true;
		source.m_markup = std::move(markup);
		source.m_fit = fit;

		this->m_sources[name] = std::move(source);
		return true;
	}

	bool c_icon_atlas::add_image(const std::string& name, const std::string& path, bool as_mask, float fit)
	{
		int width{}, height{}, channels{};
		std::uint8_t* raw = nullptr;

		if (const auto embedded = core::find_embedded_asset(path); embedded.valid())
		{
			raw = stbi_load_from_memory(
				embedded.data, static_cast<int>(embedded.size), &width, &height, &channels, 4);
		}
		else
		{
			const std::string full_path = core::asset_path(path);
			raw = stbi_load(full_path.c_str(), &width, &height, &channels, 4);
		}

		if (raw == nullptr)
		{
			slog::log::error("icons: failed to load {}", path);
			return false;
		}

		source_t source{};
		source.m_pixels.assign(raw, raw + static_cast<std::size_t>(width) * height * 4u);
		source.m_width = width;
		source.m_height = height;
		source.m_fit_ratio = fit;

		stbi_image_free(raw);

		if (as_mask)
		{
			// keep only the coverage, the gui tints the icon at draw time
			for (std::size_t index = 0; index < source.m_pixels.size(); index += 4)
			{
				source.m_pixels[index + 0] = 255;
				source.m_pixels[index + 1] = 255;
				source.m_pixels[index + 2] = 255;
			}
		}

		this->m_sources[name] = std::move(source);
		return true;
	}

	void c_icon_atlas::initialize()
	{
		slog::log::info("[>] loading the icon sources");

		/* @combat, authored here rather than shipped as a file.

		   Two crossed blades: a hilt and a guard at the bottom of each, a taper to
		   the point, and the two laid over one another so the crossing reads at
		   16px — which is the whole difficulty with a combat glyph. A single sword
		   at this size is a diagonal line, and a diagonal line is not a symbol. */
		this->add_markup("combat", k_combat_markup, svg::fit_mode::fit);
		this->add_image("movement", "icons/movement.png", true, k_glyph_fit);
		this->add_vector("visuals", "icons/render.svg");
		this->add_image("utilities", "icons/utilities.png", true, k_glyph_fit);
		this->add_vector("misc", "icons/misc.svg");

		/* secondary category icons */
		this->add_vector("chat", "icons/cht.svg");
		this->add_vector("globe", "icons/globe.svg");
		this->add_vector("configs", "icons/configs.svg");
		this->add_vector("friends", "icons/friend.svg");
		this->add_vector("hud", "icons/hud.svg");
		this->add_vector("settings", "icons/settings.svg");

		/* action & control icons */
		this->add_vector("search", "icons/search.svg");
		this->add_vector("mark", "icons/mark.svg");
		this->add_vector("eraser", "icons/eraser.svg");
		this->add_vector("sidebar_collapse", "icons/sidebarcollapse.svg");
		this->add_vector("sidebar_expand", "icons/sidebarcollapse.svg", true);

		/* @the player list's range filter.

		   A person and a plus. Drawn on the same 64-unit box as the transport
		   glyphs so its optical weight matches them without tuning at the call
		   site: head r=13 at y=20, shoulders as a capsule, and the plus sitting
		   in a notch cut out of the shoulder's lower right so the two shapes
		   read as separate at sixteen pixels instead of merging into a blob. */
		this->add_markup("user_range", k_user_range_markup, svg::fit_mode::fit);

		/* the header's player-list switch */
		this->add_markup("user", k_user_markup, svg::fit_mode::fit);

		/* Spotify HUD transport. */
		this->add_markup("tr_play",    k_tr_play_markup,    svg::fit_mode::fit);
		this->add_markup("tr_pause",   k_tr_pause_markup,   svg::fit_mode::fit);
		this->add_markup("tr_next",    k_tr_next_markup,    svg::fit_mode::fit);
		this->add_markup("tr_prev",    k_tr_prev_markup,    svg::fit_mode::fit);
		this->add_markup("tr_shuffle", k_tr_shuffle_markup, svg::fit_mode::fit);
		this->add_markup("tr_expand",  k_tr_expand_markup,  svg::fit_mode::fit);

		/* branding & user artwork, these keep their own colors */
		this->add_image("avatar", "images/avatar.png", false, 0.f);

		/* @loaded as a MASK, so the mark can be recoloured.

		   as_mask throws the artwork's own violet away and keeps only its coverage,
		   which is then tinted at draw time — the sidebar tints it with the live
		   accent so the logo follows the client's colour, and the inject sequence
		   tints it with the fixed brand violet so the splash looks the same on
		   every machine.

		   Nothing is lost by discarding the colour: the artwork is flat violet with
		   about 5% variation across it, so the depth people read in the ribbon is
		   its outline and its alpha, not its shading. */
		this->add_image("logo", "images/logo.png", true, k_logo_fit);

		slog::log::success("icon sources ready, {} entries", this->m_sources.size());
	}

	void c_icon_atlas::destroy()
	{
		for (auto& [key, view] : this->m_variants)
		{
			if (view != 0)
				glDeleteTextures(1, &view);
		}

		for (auto& [key, src] : this->m_sources)
		{
			vanta::util::scrubString(src.m_markup);
			if (!src.m_pixels.empty())
			{
				SecureZeroMemory(src.m_pixels.data(), src.m_pixels.size());
				src.m_pixels.clear();
				src.m_pixels.shrink_to_fit();
			}
		}

		this->m_variants.clear();
		this->m_sources.clear();
	}

	ImTextureID c_icon_atlas::get(const std::string& name, int pixel_size)
	{
		// artwork handed over by adopt() is stored under its bare name, at whatever size it arrived
		if (const auto adopted = this->m_variants.find(name); adopted != this->m_variants.end())
			return static_cast<ImTextureID>(static_cast<std::uintptr_t>(adopted->second));

		if (pixel_size <= 0)
			return static_cast<ImTextureID>(0);

		const std::string key = variant_key(name, pixel_size);

		if (const auto cached = this->m_variants.find(key); cached != this->m_variants.end())
			return static_cast<ImTextureID>(static_cast<std::uintptr_t>(cached->second));

		const auto source = this->m_sources.find(name);
		if (source == this->m_sources.end())
			return static_cast<ImTextureID>(0);

		GLuint view = this->bake(source->second, pixel_size);

		// cached even when it failed, so a broken source is not retried on every frame
		this->m_variants[key] = view;

		if (view == 0)
			slog::log::error("icons: failed to bake {} at {}px", name, pixel_size);

		return static_cast<ImTextureID>(static_cast<std::uintptr_t>(view));
	}

	math::c_vector_2d c_icon_atlas::dimensions(const std::string& name) const
	{
		const auto source = this->m_sources.find(name);
		if (source == this->m_sources.end())
			return math::c_vector_2d();

		return math::c_vector_2d(static_cast<float>(source->second.m_width), static_cast<float>(source->second.m_height));
	}

	ImTextureID c_icon_atlas::adopt(const std::string& name, const std::uint8_t* pixels, int width, int height)
	{
		if (const auto existing = this->m_variants.find(name); existing != this->m_variants.end())
		{
			if (existing->second != 0)
				glDeleteTextures(1, &existing->second);

			this->m_variants.erase(existing);
		}

		GLuint view = this->upload(pixels, width, height);
		this->m_variants[name] = view;

		return static_cast<ImTextureID>(static_cast<std::uintptr_t>(view));
	}

	namespace image
	{
		void resize(const std::vector<std::uint8_t>& source, int source_width, int source_height,
			std::vector<std::uint8_t>& destination, int target_width, int target_height)
		{
			destination.assign(static_cast<std::size_t>(target_width) * target_height * 4u, 0u);

			if (source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0)
				return;

			std::vector<float> filtered{};
			resample(source, source_width, source_height, 0, 0, source_width, source_height,
				filtered, target_width, target_height);

			compose(filtered, target_width, target_height, destination, target_width, 0, 0);
		}
	}
}
