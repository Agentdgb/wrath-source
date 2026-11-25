#pragma once

/* @dt::theme:
	the single source of truth for the client palette and typography.
	every hex below is the exact value the design was signed off with, do not drift them.
*/
namespace dt
{
	/* @font_face:
		families the gui draws with. every face is baked at the sizes listed in text.cc.

		script is a joined handwriting face, used only for the second half of the wordmark.
	*/
	enum class font_face : int
	{
		regular = 0,
		bold,
		script,

		/* @italic:
			a REAL italic, borrowed from the system, baked at gui sizes.

			It exists because shearing the roman does not work down here. At 15px
			the x-height is about seven pixels, so a 13-degree lean displaces the
			tops of the letters by under two — mathematically an italic, visually
			nothing. The letterforms have to actually differ, and for that the face
			has to actually be one.

			Falls back to bold when the machine has no italic installed, in which
			case draw_text shears it as a last resort.
		*/
		italic,
		count
	};

	/* @text_align:
		bit flags, mirrors the alignment contract the gui was written against.
		horizontal: left / center / right. vertical: middle centers on the given y.
	*/
	enum text_align : int
	{
		align_left = 1 << 0,
		align_center = 1 << 1,
		align_right = 1 << 2,

		/* y is the optical centre of the line */
		align_middle = 1 << 3,

		/* y is the baseline. two faces of different sizes only sit level on a shared baseline,
		   which is what a wordmark made of two typefaces needs. */
		align_baseline = 1 << 4
	};

	namespace theme
	{
		/* @defaults:
			the palette the design was signed off with. these never move — they are what
			"Reset" restores, and what every live value below is seeded from.
		*/
		inline constexpr hue::c_color k_bg_window{ 10, 10, 10, 255 };
		inline constexpr hue::c_color k_bg_sidebar{ 12, 12, 12, 255 };
		inline constexpr hue::c_color k_bg_search{ 15, 15, 15, 255 };
		inline constexpr hue::c_color k_accent{ 168, 85, 247, 255 };
        inline constexpr hue::c_color k_intro{ 154, 71, 245, 255 };

		/* @live palette:
			the four surfaces the profile panel lets the user restyle are ORDINARY
			variables, not constants.

			That is the whole trick: every call site in the client already reads
			`theme::accent_violet` / `theme::bg_sidebar`, so making the storage mutable
			turns fifty existing draw calls into customisable ones without touching a
			single one of them. A parallel "custom colour" lookup would have had to be
			threaded through each of those instead, and any site that was missed would
			have kept painting the old violet.
		*/

		/* main background behind everything: #0A0A0A */
		inline hue::c_color bg_window{ k_bg_window };

		/* cards (sidebar, content panel): #0C0C0C */
		inline hue::c_color bg_sidebar{ k_bg_sidebar };
		inline hue::c_color bg_card{ 12, 12, 12, 255 };

		/* interactive elements (profile, buttons): #0F0F0F */
		inline hue::c_color bg_element{ 15, 15, 15, 255 };

		/* the search field, customisable on its own so it can be lifted off the card */
		inline hue::c_color bg_search{ k_bg_search };
        inline hue::c_color intro_colour{ k_intro };
		inline hue::c_color bg_selected{ 20, 20, 20, 255 };

		/* borders & outlines: #101010, static, never illuminated on hover */
		inline hue::c_color border{ 16, 16, 16, 255 };
		inline hue::c_color border_light{ 16, 16, 16, 255 };
		inline hue::c_color divider{ 16, 16, 16, 255 };

		/* text & details */
		inline constexpr hue::c_color text_white{ 255, 255, 255, 255 };
		inline constexpr hue::c_color text_primary{ 255, 255, 255, 255 };
		inline constexpr hue::c_color text_secondary{ 160, 160, 160, 255 };
		inline constexpr hue::c_color text_muted{ 110, 110, 120, 255 };

		/* accent: electric violet */
		inline hue::c_color accent_violet{ k_accent };
		inline hue::c_color accent_violet_deep{ 147, 51, 234, 255 };
		inline hue::c_color accent_violet_glow{ 168, 85, 247, 60 };

		/* status */
		inline constexpr hue::c_color online_green{ 34, 197, 94, 255 };
		inline constexpr hue::c_color danger_red{ 239, 68, 68, 255 };

		/* branding lilac used by the italic half of the wordmark */
		inline hue::c_color brand_lilac{ 192, 132, 252, 255 };

		/* the body a module card takes once it is on, and the opacity its ring is stroked with */
		inline hue::c_color card_enabled{ 22, 18, 28, 255 };
		inline constexpr float accent_ring_opacity = 200.f / 255.f;

		/* @accent_ring:
			the violet an active module ring actually lands on screen.

			the ring is stroked at 200/255, so what the eye sees is the accent mixed into the card
			beneath it, several shades deeper than the raw constant. anything that has to match that
			ring wears this instead of accent_violet, and the match survives a change to either half.
		*/
		inline hue::c_color accent_ring()
		{
			return card_enabled.lerp(accent_violet, accent_ring_opacity);
		}

		/* ------------------------------------------------------------------ */
		/* glass                                                               */
		/* ------------------------------------------------------------------ */

		/* @glass:
			whether the client's own surfaces are panes rather than cards.

			Off, every surface is opaque and the client is a solid object sitting on
			the game. On, the backdrop is blurred and everything above it is sheer
			enough to let that blur through — which is only possible because the
			backdrop is the FIRST thing the client draws, so at that moment the
			framebuffer still holds nothing but the game.
		*/
		inline bool glass_enabled = false;

		/* @pane:
			a surface's colour, given the mode the client is in.

			One function rather than a branch at every draw site: there are three
			backgrounds and they have to agree, and "which alpha does the sidebar use
			again" is exactly the sort of question that gets answered differently in
			two places six months apart.
		*/
		inline hue::c_color pane(const hue::c_color& solid, float sheer)
		{
			if (!glass_enabled)
				return solid;

			return solid.alpha(static_cast<int>(solid.a * std::clamp(sheer, 0.f, 1.f)));
		}

		/* ------------------------------------------------------------------ */
		/* customisation                                                       */
		/* ------------------------------------------------------------------ */

		/* @set_accent:
			re-tints the whole client from one colour.

			The accent is not one value but three — the raw violet, a deeper one the
			chips and pills sit on, and the lilac the wordmark's second half wears.
			Deriving the other two here rather than exposing three pickers is what
			keeps a user-chosen accent looking like a designed palette instead of
			three unrelated colours: the relationships between them are preserved,
			only the hue they are built from moves.
		*/
		inline void set_accent(hue::c_color colour)
		{
			colour.a = 255;

			accent_violet = colour;

			/* The deep shade is a half-strength MULTIPLY of the accent against
			   itself, not a lerp towards black. A lerp darkens every channel by
			   the same amount and washes the colour out; multiplying darkens the
			   weak channels hardest, which deepens and saturates at once — run on
			   the reference violet it reproduces #9333EA to within a shade, and
			   it holds that relationship for any hue the user picks.

			   The lilac is the plain 30% lift towards white the wordmark uses. */
			const auto deepen = [](int channel)
				{
					const float value = static_cast<float>(channel);
					return static_cast<int>(value + (value * value / 255.f - value) * 0.5f);
				};

			accent_violet_deep = hue::c_color(deepen(colour.r), deepen(colour.g), deepen(colour.b), 255);
			brand_lilac = colour.lerp(hue::c_color(255, 255, 255, 255), 0.30f);
			accent_violet_glow = colour.alpha(60);

			/* Dynamically derive ambient surface tones from the accent so that the
			   whole client harmonises with the chosen color (cards, tracks, borders, background) */
			bg_window = hue::c_color(10, 10, 12).lerp(colour, 0.07f);
			bg_sidebar = hue::c_color(10, 10, 12).lerp(colour, 0.05f);
			bg_card = hue::c_color(13, 13, 16).lerp(colour, 0.09f);
			bg_element = hue::c_color(16, 16, 20).lerp(colour, 0.13f);
			bg_search = hue::c_color(14, 14, 18).lerp(colour, 0.10f);
			intro_colour = colour;
			card_enabled = bg_element.lerp(colour, 0.18f);
			border = hue::c_color(22, 22, 26).lerp(colour, 0.12f);
		}

		/* ------------------------------------------------------------------ */
		/* preset theme cards system                                           */
		/* ------------------------------------------------------------------ */
		struct theme_preset_t
		{
			const char* id;
			const char* name;
			hue::c_color accent;
			hue::c_color secondary_accent;
			hue::c_color bg_window;
			hue::c_color bg_sidebar;
			hue::c_color bg_card;
			hue::c_color bg_element;
			hue::c_color bg_search;
			hue::c_color intro;
		};

		inline const theme_preset_t k_presets[] = {
			{
				"indigo",
				"Indigo",
				hue::c_color(112, 107, 234, 255),
				hue::c_color(79, 70, 229, 255),
				hue::c_color(18, 19, 26, 255),
				hue::c_color(14, 15, 20, 255),
				hue::c_color(18, 19, 26, 255),
				hue::c_color(26, 27, 36, 255),
				hue::c_color(22, 23, 32, 255),
				hue::c_color(91, 88, 236, 255)
			},
			{
				"crimson",
				"Crimson",
				hue::c_color(217, 119, 6, 255),
				hue::c_color(136, 34, 34, 255),
				hue::c_color(30, 21, 21, 255),
				hue::c_color(21, 14, 14, 255),
				hue::c_color(30, 21, 21, 255),
				hue::c_color(40, 29, 29, 255),
				hue::c_color(32, 20, 20, 255),
				hue::c_color(158, 42, 43, 255)
			},
			{
				"violet",
				"Violet",
				hue::c_color(140, 50, 155, 255),
				hue::c_color(90, 32, 102, 255),
				hue::c_color(11, 4, 13, 255),
				hue::c_color(9, 3, 11, 255),
				hue::c_color(16, 7, 19, 255),
				hue::c_color(24, 10, 28, 255),
				hue::c_color(19, 8, 23, 255),
				hue::c_color(140, 50, 155, 255)
			},
			{
				"obsidian",
				"Obsidian",
				hue::c_color(168, 85, 247, 255),
				hue::c_color(147, 51, 234, 255),
				hue::c_color(10, 10, 10, 255),
				hue::c_color(12, 12, 12, 255),
				hue::c_color(12, 12, 12, 255),
				hue::c_color(15, 15, 15, 255),
				hue::c_color(15, 15, 15, 255),
				hue::c_color(154, 71, 245, 255)
			},
			{
				"emerald",
				"Emerald",
				hue::c_color(16, 185, 129, 255),
				hue::c_color(4, 120, 87, 255),
				hue::c_color(15, 23, 20, 255),
				hue::c_color(10, 17, 14, 255),
				hue::c_color(15, 23, 20, 255),
				hue::c_color(23, 36, 31, 255),
				hue::c_color(18, 28, 24, 255),
				hue::c_color(5, 150, 105, 255)
			}
		};

		inline constexpr std::size_t k_preset_count = sizeof(k_presets) / sizeof(k_presets[0]);
		inline std::string current_preset = "indigo";

		inline void apply_preset(const theme_preset_t& p)
		{
			current_preset = p.id;
			set_accent(p.accent);
			bg_window = p.bg_window;
			bg_sidebar = p.bg_sidebar;
			bg_card = p.bg_card;
			bg_element = p.bg_element;
			bg_search = p.bg_search;
			intro_colour = p.intro;
			card_enabled = p.bg_element.lerp(p.accent, 0.12f);
		}

		inline bool apply_preset_by_id(const std::string& id)
		{
			for (std::size_t i = 0; i < k_preset_count; ++i)
			{
				if (id == k_presets[i].id)
				{
					apply_preset(k_presets[i]);
					return true;
				}
			}
			return false;
		}

		/* @reset_customisation:
			puts every live value back to the signed-off palette
		*/
		inline void reset_customisation()
		{
			bg_window = k_bg_window;
			bg_sidebar = k_bg_sidebar;
			bg_card = hue::c_color(12, 12, 12, 255);
			bg_element = hue::c_color(15, 15, 15, 255);
			bg_search = k_bg_search;
            intro_colour = k_intro;
			current_preset = "obsidian";

			set_accent(k_accent);
		}
	}
}
