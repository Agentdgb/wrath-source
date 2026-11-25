#include "../includes.hh"
#include "../../util/StringScrub.h"
#include "../../modules/render/targethud/fonts/hud_font_data.inl"
#include "embedded_assets.inl"
#include "wrath_loader_logo.inl"

namespace core
{
	namespace
	{
		std::string g_asset_root{};
		std::string g_asset_directory{};

		std::string normalize_asset_key(std::string_view raw)
		{
			std::string key(raw);
			for (char& c : key)
			{
				if (c == '\\') c = '/';
			}
			for (const std::string_view prefix : { "gl_assets/", "assets/", "./" })
			{
				if (key.size() >= prefix.size() &&
					key.compare(0, prefix.size(), prefix) == 0)
				{
					key.erase(0, prefix.size());
				}
			}
			return key;
		}
	}

	embedded_asset_view_t find_embedded_asset(std::string_view relative) noexcept
	{
		const std::string key = normalize_asset_key(relative);

		if (key == "images/gl-loader.png") return { embedded::k_wrath_loader_logo, sizeof(embedded::k_wrath_loader_logo) };

		// 1) Shared fonts already embedded in hud_font_data.inl
		if (key == "fonts/proximabd.ttf")
			return { k_font_proxima_bold, k_font_proxima_bold_size };
		if (key == "fonts/proxima.ttf")
			return { k_font_proxima, k_font_proxima_size };
		if (key == "fonts/comfortaa_bold.ttf")
			return { k_font_comfortaa, k_font_comfortaa_size };

		// 2) Remaining fonts, icons, and images in embedded_assets.inl
		using namespace embedded;
		struct entry_t { const char* path; const std::uint8_t* data; std::size_t size; };
		static constexpr entry_t k_table[] = {
			{ "fonts/anime.ttf",              k_asset_fonts_anime_ttf,              k_asset_fonts_anime_ttf_size },
			{ "fonts/badaboom.ttf",           k_asset_fonts_badaboom_ttf,           k_asset_fonts_badaboom_ttf_size },
			{ "icons/cht.svg",                k_asset_icons_cht_svg,                k_asset_icons_cht_svg_size },
			{ "icons/combat.png",             k_asset_icons_combat_png,             k_asset_icons_combat_png_size },
			{ "icons/configs.svg",            k_asset_icons_configs_svg,            k_asset_icons_configs_svg_size },
			{ "icons/eraser.svg",             k_asset_icons_eraser_svg,             k_asset_icons_eraser_svg_size },
			{ "icons/friend.svg",             k_asset_icons_friend_svg,             k_asset_icons_friend_svg_size },
			{ "icons/globe.svg",              k_asset_icons_globe_svg,              k_asset_icons_globe_svg_size },
			{ "icons/hud.svg",                k_asset_icons_hud_svg,                k_asset_icons_hud_svg_size },
			{ "icons/mark.svg",               k_asset_icons_mark_svg,               k_asset_icons_mark_svg_size },
			{ "icons/misc.svg",               k_asset_icons_misc_svg,               k_asset_icons_misc_svg_size },
			{ "icons/movement.png",           k_asset_icons_movement_png,           k_asset_icons_movement_png_size },
			{ "icons/render.svg",             k_asset_icons_render_svg,             k_asset_icons_render_svg_size },
			{ "icons/search.svg",             k_asset_icons_search_svg,             k_asset_icons_search_svg_size },
			{ "icons/settings.svg",           k_asset_icons_settings_svg,           k_asset_icons_settings_svg_size },
			{ "icons/sidebarcollapse.svg",    k_asset_icons_sidebarcollapse_svg,    k_asset_icons_sidebarcollapse_svg_size },
			{ "icons/utilities.png",          k_asset_icons_utilities_png,          k_asset_icons_utilities_png_size },
			{ "images/avatar.png",            k_asset_images_avatar_png,            k_asset_images_avatar_png_size },
			{ "images/logo.png",              k_asset_images_logo_png,              k_asset_images_logo_png_size },
		};

		for (const auto& e : k_table)
		{
			if (key == e.path)
				return { e.data, e.size };
		}

		return {};
	}

	bool locate_asset_root()
	{
		// All Dt assets are now embedded in memory; no disk directory lookup is required.
		return true;
	}

	const std::string& asset_root()
	{
		return g_asset_root;
	}

	std::string asset_path(std::string_view relative)
	{
		return normalize_asset_key(relative);
	}

	void scrub_paths()
	{
		dt::util::scrubString(g_asset_root);
		dt::util::scrubString(g_asset_directory);
	}
}

