#include "../../includes.hh"

#include "intro.hh"
#include "../../../modules/render/arraylist/font_pool.h"

namespace vanta::intro
{
	namespace
	{
		/* ------------------------------------------------------------------ */
		/* timing                                                              */
		/* ------------------------------------------------------------------ */

		/* How long we will wait for the JVM to hand over the account name before
		   greeting whoever it is anyway.

		   Generous, because the wait costs nothing to look at — nothing is drawn
		   while we are arming, the game just plays. It matters when the client is
		   injected during a launcher's auth screen, where the session does not
		   exist yet and the name can be seconds away. */
		constexpr float k_arm_timeout = 3.00f;

		constexpr float k_welcome_in = 0.80f;
		constexpr float k_welcome_hold = 1.05f;
		constexpr float k_welcome_out = 0.55f;
		constexpr float k_welcome_total = k_welcome_in + k_welcome_hold + k_welcome_out;

		constexpr float k_flood = 1.35f;

		constexpr float k_logo_in = 0.70f;
		constexpr float k_word_delay = 0.34f;
		constexpr float k_word_in = 0.72f;
		constexpr float k_mark_total = k_word_delay + k_word_in;

		constexpr float k_hold = 1.25f;

		constexpr float k_word_out = 0.50f;
		constexpr float k_logo_out_delay = 0.22f;
		constexpr float k_logo_out = 0.52f;
		constexpr float k_mark_out_total = k_logo_out_delay + k_logo_out;

		constexpr float k_drain = 1.70f;

		/* ------------------------------------------------------------------ */
		/* palette                                                             */
		/* ------------------------------------------------------------------ */

		/* Deliberately NOT theme::accent_violet. The flood is branding, not
		   chrome: it has to look the same on every machine, including one whose
		   owner has recoloured the whole client mint green from the account panel. */
		/* the artwork's own violet. The mark is shipped as a coverage mask so the
		   gui can recolour it; the splash puts its original colour back, because
		   branding should not change shape depending on someone's accent. */
		constexpr hue::c_color k_mark{ 165, 10, 239, 255 };

		constexpr hue::c_color k_crest{ 196, 132, 252, 255 };
		constexpr hue::c_color k_surface{ 154, 71, 245, 255 };
		constexpr hue::c_color k_deep{ 52, 12, 92, 255 };

		/* ------------------------------------------------------------------ */
		/* state                                                               */
		/* ------------------------------------------------------------------ */

		enum class stage
		{
			idle,
			arming,     /* waiting for the name                    */
			welcome,    /* "Welcome Back, <name>"                  */
			flood,      /* the violet rises                        */
			mark,       /* logo, then the wordmark                 */
			hold,
			mark_out,   /* the letters leave, logo behind them     */
			drain,      /* the violet comes apart and runs off     */
			done
		};

		stage g_stage = stage::idle;
		bool g_finished_edge = false;

		float g_clock = 0.f;       /* time inside the current stage */
		float g_wave = 0.f;        /* free-running clock for the surface + glints */
		std::string g_name{};

		/* ------------------------------------------------------------------ */
		/* helpers                                                             */
		/* ------------------------------------------------------------------ */

		void advance(stage next)
		{
			g_stage = next;
			g_clock = 0.f;
		}

		/* @surface_at:
			the liquid's top edge, as an offset in pixels from its resting height.

			Two waves at different lengths and speeds, deliberately not harmonics of
			each other, so the crest never repeats within the time anyone watches it.
			A single sine reads as a machine; two that beat against each other read
			as liquid.
		*/
		float surface_at(float t, float amplitude)
		{
			return amplitude * (std::sin(t * 6.1f + g_wave * 2.35f) * 0.62f
				+ std::sin(t * 13.7f - g_wave * 1.55f) * 0.38f);
		}

		/* @lobes_at:
			COHERENT noise across the width, 0..1.

			The point is the word coherent. An earlier cut hashed each column index
			straight into a number, which is white noise: neighbouring columns got
			unrelated values, so the draining edge became a 13px sawtooth — visually
			static, not liquid. Interpolating between hashed lattice points with a
			smoothstep gives neighbours values that belong together, which is what
			turns the drain into a handful of broad tongues letting go at their own
			pace instead of a comb of teeth.

			Two octaves: big lobes decide where the tongues form, the finer one puts
			lips on their edges so no two are the same shape.
		*/
		float hash_at(float slot)
		{
			const float value = std::sin(slot * 127.1f) * 43758.5453f;
			return value - std::floor(value);
		}

		float value_noise(float x)
		{
			const float cell = std::floor(x);
			const float t = x - cell;

			// smoothstep, so the slope matches at every lattice point and the
			// surface has no visible creases where the cells meet
			const float blend = t * t * (3.f - 2.f * t);

			const float low = hash_at(cell);
			const float high = hash_at(cell + 1.f);

			return low + (high - low) * blend;
		}

		float lobes_at(float t)
		{
			return anim::saturate(value_noise(t * 5.5f + 11.3f) * 0.68f
				+ value_noise(t * 12.0f + 41.7f) * 0.32f);
		}

		/* @tongues_at:
			the same idea at a much coarser scale, and the only thing the drain uses.

			lobes_at() carries a fine octave, which is right for a surface — it puts
			lips on the waves. It is wrong for deciding WHEN a stretch of the sheet
			lets go: the fine detail makes neighbouring stretches disagree over short
			distances, so the survivors end up as narrow towers standing on the floor
			rather than as broad tongues hanging back. One slow octave, about three
			lobes across the frame, is what makes the sheet tear into a few big
			pieces instead of a row of spikes.
		*/
		float tongues_at(float t)
		{
			return anim::saturate(value_noise(t * 4.5f + 7.7f) * 0.75f
				+ value_noise(t * 8.5f + 23.1f) * 0.25f);
		}

		/* ------------------------------------------------------------------ */
		/* type                                                                */
		/* ------------------------------------------------------------------ */

		ImFont* display_font(float size)
		{
			auto& pool = vanta::modules::arraylist::shared_pool();
			if (!pool.ensure())
				return nullptr;

			return pool.get(size);
		}

		/* @display_italic:
			the wordmark's cut.

			Prefers a REAL italic off the system. A shear turns a roman into a roman
			that leans, and that is not what an italic is — the lowercase is drawn
			differently, most visibly the single-storey 'a'. It also fixes a second
			problem nobody could have named by looking at it: the roman ladder tops
			out at 96 px and the wordmark asks for 160+ on a 1080p frame, so it was
			being upscaled almost 2x and arriving soft.

			`synthesized` comes back true when the machine had no italic to lend, and
			the caller shears the roman as before.
		*/
		ImFont* display_italic(float size, bool& synthesized)
		{
			using pool_t = vanta::modules::arraylist::font_pool;

			auto& pool = vanta::modules::arraylist::shared_pool();

			if (!pool.ensure())
			{
				synthesized = true;
				return nullptr;
			}

			if (pool.has_italic())
			{
				ImFont* italic = pool.get(size, pool_t::face::italic);

				/* @and then check that it can actually draw.

				   Having the face is not the same as having its glyphs. The pool
				   shares one atlas with the HUD's whole size ladder, and if the
				   packer runs out of room the font is still there with nothing in
				   it — AddText would then draw a blank where the wordmark should be,
				   or silently fall back. Asking the font for one of the letters we
				   are about to set is the only honest way to know, and it costs a
				   hash lookup. */
				if (italic != nullptr && italic->FindGlyph(static_cast<ImWchar>('a')) != nullptr)
				{
					synthesized = false;
					return italic;
				}
			}

			/* no italic, or one that cannot draw: shear the roman instead */
			synthesized = true;
			return pool.get(size, pool_t::face::roman);
		}

		float measure(ImFont* font, float size, const char* text)
		{
			if (font == nullptr)
				return 0.f;

			return font->CalcTextSizeA(size, FLT_MAX, -1.f, text).x;
		}

		/* @cap_height:
			the height of a capital, in pixels, for this font at this size.

			Needed because a wordmark set beside a logo has to be optically centred
			on it, and the line box is the wrong thing to centre — it carries the
			descender space of letters this word does not contain. ImGui gives glyph
			Y0 relative to the top of the line and puts the baseline at Ascent, so
			the cap band is exactly the distance between them.
		*/
		float cap_height(ImFont* font, float size)
		{
			if (font == nullptr || font->FontSize <= 0.f)
				return size * 0.7f;

			const float scale = size / font->FontSize;

			if (const ImFontGlyph* glyph = font->FindGlyph(static_cast<ImWchar>('V')))
				return (font->Ascent - glyph->Y0) * scale;

			return size * 0.7f;
		}

		/* @put_text:
			one run of display type, optionally sheared into an italic.

			The shear is applied to the vertices after the fact rather than asked of
			the font, because the client ships one roman cut and no italic — and an
			oblique of a roman is the same glyphs leaning, which is what a
			synthesised italic is. It pivots on the BASELINE so the feet stay planted
			and only the shoulders move; pivoting on the middle would slide the whole
			word sideways as it leaned.
		*/
		void put_text(ImDrawList* draw_list, ImFont* font, float size, const char* text,
			float x, float baseline, hue::c_color colour, float slant)
		{
			if (font == nullptr || colour.a <= 0)
				return;

			const int first = draw_list->VtxBuffer.Size;

			draw_list->AddText(font, size, ImVec2(x, baseline - font->Ascent * (size / font->FontSize)),
				colour.transform(), text);

			if (slant == 0.f)
				return;

			for (int index = first; index < draw_list->VtxBuffer.Size; ++index)
			{
				ImDrawVert& vertex = draw_list->VtxBuffer[index];
				vertex.pos.x += (baseline - vertex.pos.y) * slant;
			}
		}

		/* @tracked_width / @put_tracked:
			the wordmark, set letter by letter with air between the letters.

			Two things a sheared roman needs that a plain AddText will not give it.
			First, tracking: leaning glyphs bring their shoulders towards the letter
			in front, so a slant set at the metrics of an upright face reads cramped
			and the counters close up. Second, the shear has to happen per glyph
			about the SHARED baseline — which is the same transform as shearing the
			whole run, but it lets the advance stay upright so the letters do not
			also drift apart as they lean.
		*/
		float tracked_width(ImFont* font, float size, const char* text, float tracking)
		{
			if (font == nullptr || *text == '\0')
				return 0.f;

			float total = 0.f;

			for (const char* character = text; *character != '\0'; ++character)
			{
				const char glyph[2]{ *character, '\0' };
				total += measure(font, size, glyph) + tracking;
			}

			return total - tracking;
		}

		void put_tracked(ImDrawList* draw_list, ImFont* font, float size, const char* text,
			float x, float baseline, hue::c_color colour, float slant, float tracking)
		{
			if (font == nullptr)
				return;

			float cursor = x;

			for (const char* character = text; *character != '\0'; ++character)
			{
				const char glyph[2]{ *character, '\0' };

				put_text(draw_list, font, size, glyph, cursor, baseline, colour, slant);
				cursor += measure(font, size, glyph) + tracking;
			}
		}

		/* ------------------------------------------------------------------ */
		/* the flood                                                           */
		/* ------------------------------------------------------------------ */

		/* @paint_liquid:
			the violet itself.

			`fill` is how much of the frame it covers, 0..1 and a little past 1 so the
			crest can travel off the top rather than flattening against it. `unmake`
			is the drain, 0 while the sheet is whole. `motion` is how alive the
			surface still is: 1 while it is climbing, 0 once it has settled.

			Drawn as a column mesh instead of a rectangle for one reason: the drain.
			A rectangle can only shrink evenly, and coming apart means each part
			leaving at its own pace. Once the surface is per-column the waves come
			free — and so does the tearing at the end, which is nothing more than the
			same columns each given their own head start.

			Every column shares ONE opacity. An earlier cut faded each column on its
			own clock, which put a hard alpha step on every seam and turned the
			whole sheet into vertical banding — the geometry is what should come
			apart, never the colour.
		*/
		void paint_liquid(ImDrawList* draw_list, float width, float height,
			float fill, float unmake, float dim, float motion)
		{
			if (fill <= 0.f)
				return;

			/* The sheet dissolves at the end, and GLOBALLY — one opacity for every
			   column, never one each. Overlapped with the tail of the tearing on
			   purpose: the last tongues are still letting go while it fades, so the
			   violet goes out by coming apart and thinning at the same time rather
			   than shrinking to a stub and then switching off.

			   It starts where the tearing starts, not after it. By the time a tongue
			   has shrunk to the point where it would read as a mound on the floor it
			   is already most of the way transparent, so that shape is never
			   something anyone actually sees. */
			const float sheet = 1.f - anim::ease_in_out_sine(anim::saturate((unmake - 0.48f) / 0.50f));

			if (sheet <= 0.004f)
				return;

			constexpr int k_columns = 200;

			const float step = width / static_cast<float>(k_columns);
			const ImVec2 uv = draw_list->_Data->TexUvWhitePixel;

			/* the crest is tall while the liquid is moving and settles once it has
			   arrived — still water, roughly */
			const float amplitude = height * 0.030f * (0.35f + 0.65f * motion);

			const hue::c_color surface = k_surface.lerp(k_deep, dim * 0.45f).modulate(sheet);
			const hue::c_color deep = k_deep.lerp(hue::c_color(24, 5, 46, 255), dim).modulate(sheet);

			const ImU32 top_colour = surface.transform();
			const ImU32 bottom_colour = deep.transform();

			/* @two passes:
				solved first, drawn second.

				PrimReserve hands out a fixed run of vertex and index slots and every
				one of them is rendered whether or not it was written — a column that
				decided mid-loop to skip itself would leave garbage triangles behind.
				So the surface is worked out for every column up front, the drained
				ones are counted out, and only then is the exact space they need
				reserved. */
			std::array<float, k_columns> tops{};
			std::array<bool, k_columns> alive{};

			int drawn = 0;

			for (int index = 0; index < k_columns; ++index)
			{
				const float centre = step * (static_cast<float>(index) + 0.5f);
				const float t = centre / std::max(1.f, width);
				const float lobe = lobes_at(t);

				/* Rising, the sheet climbs unevenly — some stretches run ahead of the
				   rest, the way water does up an uneven surface. Scaled by motion, so
				   the lead flattens out as it arrives instead of leaving the settled
				   surface permanently lopsided.

				   Only while it is climbing. lobes_at() carries a fine octave, which
				   is what a surface wants and the opposite of what a draining edge
				   wants — left switched on it would put back exactly the comb the
				   coarse tongue field below exists to avoid. Both are zero at the
				   moment the drain begins, so the gate costs no continuity. */
				const float climb = (unmake > 0.f) ? 0.f : (lobe - 0.5f) * 0.10f * motion;

				float column_fill = fill + climb;

				if (unmake > 0.f)
				{
					/* Draining, each tongue lets go on its own clock. `spread` is how
					   far apart those clocks are — it is what tears one sheet into
					   several — and the cube is gravity: a tongue barely moves, then
					   goes all at once. A linear fall reads as a wipe.

					   The two numbers are not free. Solving the curve for its ends:
					   the first tongue finishes at fall/(1+spread) and the last at
					   (fall+spread)/(1+spread) — here 0.55 and 0.95 of the drain.

					   That window is deliberately LATE and narrow, and it overlaps the
					   dissolve below rather than running before it. Three earlier
					   settings each failed a different way, and all three were the
					   same mistake — letting the tearing finish the job on its own:

					     0.48 / 0.62 emptied the frame with a third of the drain still
					     to run, and the sequence ended staring at nothing.

					     0.72 / 2.30 let the spread swamp the noise, so the sheet left
					     in one left-to-right wipe with a lone column at the edge.

					     0.70 / 1.00 tore cleanly but ran to completion, and what is
					     left at the end of a tear is small isolated mounds sitting on
					     the floor — hills, not liquid.

					   Tearing gets it ragged; the dissolve is what takes it away. Each
					   does half, at the same time, and neither is watched alone. */
					constexpr float spread = 0.67f;
					constexpr float fall = 0.92f;

					const float slot = tongues_at(t);
					const float local = anim::saturate((unmake * (1.f + spread) - slot * spread) / fall);

					column_fill *= 1.f - local * local * local;
				}

				if (column_fill <= 0.002f)
				{
					alive[static_cast<std::size_t>(index)] = false;
					continue;
				}

				tops[static_cast<std::size_t>(index)] =
					height * (1.f - column_fill) + surface_at(t, amplitude);

				alive[static_cast<std::size_t>(index)] = true;
				++drawn;
			}

			if (drawn == 0)
				return;

			/* --- the haze the crest pushes ahead of itself -------------------- */
			/* Light scatters off a bright surface into whatever is above it, and
			   without that the rising edge is a hard line with a game behind it —
			   a wipe, not a flood. Drawn first so the body lands on top of it. */
			if (motion > 0.02f && fill < 1.04f)
			{
				const float reach = height * 0.10f * motion;

				draw_list->PrimReserve(drawn * 6, drawn * 4);

				const ImU32 clear = k_crest.alpha(0).transform();
				const ImU32 lit = k_crest.alpha(static_cast<int>(70.f * motion * sheet)).transform();

				for (int index = 0; index < k_columns; ++index)
				{
					if (!alive[static_cast<std::size_t>(index)])
						continue;

					const float left = step * static_cast<float>(index);
					const float top = tops[static_cast<std::size_t>(index)];

					const ImDrawIdx base = static_cast<ImDrawIdx>(draw_list->_VtxCurrentIdx);

					draw_list->PrimWriteVtx(ImVec2(left, top - reach), uv, clear);
					draw_list->PrimWriteVtx(ImVec2(left + step, top - reach), uv, clear);
					draw_list->PrimWriteVtx(ImVec2(left + step, top), uv, lit);
					draw_list->PrimWriteVtx(ImVec2(left, top), uv, lit);

					draw_list->PrimWriteIdx(base);
					draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
					draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
					draw_list->PrimWriteIdx(base);
					draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
					draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
				}
			}

			/* --- the body ----------------------------------------------------- */
			draw_list->PrimReserve(drawn * 6, drawn * 4);

			for (int index = 0; index < k_columns; ++index)
			{
				if (!alive[static_cast<std::size_t>(index)])
					continue;

				const float left = step * static_cast<float>(index);
				const float top = tops[static_cast<std::size_t>(index)];

				const ImDrawIdx base = static_cast<ImDrawIdx>(draw_list->_VtxCurrentIdx);

				draw_list->PrimWriteVtx(ImVec2(left, top), uv, top_colour);
				draw_list->PrimWriteVtx(ImVec2(left + step, top), uv, top_colour);
				draw_list->PrimWriteVtx(ImVec2(left + step, height), uv, bottom_colour);
				draw_list->PrimWriteVtx(ImVec2(left, height), uv, bottom_colour);

				draw_list->PrimWriteIdx(base);
				draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
				draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
				draw_list->PrimWriteIdx(base);
				draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
				draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
			}

			/* --- the crest ---------------------------------------------------- */
			/* A bright hairline where the liquid meets what it has not taken yet.
			   Without it the fill has no edge and reads as a wash being turned up;
			   with it there is a surface, and the frame below it is under something.

			   Drawn on the way out as well as on the way in — the drain is when the
			   surface has the most to say, since every tongue still hanging carries
			   its own lit edge. */
			const float lit = anim::saturate(1.f - dim) * sheet;

			if (lit <= 0.02f)
				return;

			for (int pass = 0; pass < 3; ++pass)
			{
				const float thickness = 1.6f + 5.5f * static_cast<float>(pass);
				const float weight = (pass == 0) ? 0.95f : (pass == 1 ? 0.30f : 0.12f);

				const ImU32 colour = k_crest.alpha(static_cast<int>(255.f * weight * lit)).transform();

				draw_list->PathClear();

				for (int index = 0; index < k_columns; ++index)
				{
					const std::size_t slot = static_cast<std::size_t>(index);
					const float top = tops[slot];

					/* A column is on the line only while it is present AND its edge is
					   actually on screen. Once the fill is over the top of the frame
					   its crest is above y=0, and joining an off-screen point to an
					   on-screen one would draw a diagonal across the whole picture —
					   which is what the breaks below prevent. */
					if (alive[slot] && top > 1.f && top < height - 1.f)
					{
						draw_list->PathLineTo(ImVec2(step * (static_cast<float>(index) + 0.5f), top));
						continue;
					}

					/* a tongue that has let go BREAKS the line rather than dragging it
					   down to the floor — the gaps are the tearing */
					if (draw_list->_Path.Size > 1)
						draw_list->PathStroke(colour, ImDrawFlags_None, thickness);

					draw_list->PathClear();
				}

				if (draw_list->_Path.Size > 1)
					draw_list->PathStroke(colour, ImDrawFlags_None, thickness);

				draw_list->PathClear();
			}
		}

		/* ------------------------------------------------------------------ */
		/* the mark                                                            */
		/* ------------------------------------------------------------------ */

		/* the logo is baked ONCE at this size and scaled by the blit.

		   draw::icon() bakes a texture per requested pixel size and caches it for
		   ever; asking it for a size that animates would bake a new one on every
		   frame of the entrance and never let go of any of them. */
		constexpr int k_logo_bake = 256;

		/* @the lockup.

		   The mark IS the V of the word — so what is set beside it is "anta", not
		   "Vanta", and the two are one wordmark rather than a logo with its name
		   written out next to it. Everything below is a ratio of the V's height,
		   because that is the only measurement in the lockup that means anything:
		   point sizes chosen independently would drift out of proportion the moment
		   the mark was resized.

		   The gap is NEGATIVE. A V leaves a wide empty triangle under its right
		   arm, and at the baseline — which is exactly where "anta" sits — that
		   space is entirely unused. Setting the word at a positive gap leaves a
		   hole in the middle of the word; tucking it back under the arm is what
		   makes the two read as one thing. Chosen by eye against the artwork: past
		   about -0.10 the 'a' starts crowding the arm.
		*/
		constexpr float k_word_ratio = 0.72f;

		/* @the tuck, measured rather than guessed.

		   The mark is 1.34 times as wide as it is tall, but almost none of that
		   width is down at the baseline: the arms splay upward, so the ribbon's
		   rightmost ink recedes from 0.99 of the width at the top to 0.64 at the
		   bottom. "anta" lives in the lowest ~36% of the mark's height, and across
		   that band the ink stops at about 0.91 of the mark's HEIGHT.

		   Setting the word against the mark's bounding box therefore leaves a hole
		   the size of the empty triangle under the right arm. This pulls it back to
		   just clear of the ink that is actually there. */
		constexpr float k_gap_ratio = -0.36f;
		constexpr float k_tracking_ratio = 0.048f;

		/* Only used when the machine had no italic to lend and the roman has to be
		   sheared into one. ~13 degrees: beside a mark this heavy a shy lean does
		   not register as an italic at all, it registers as type that is very
		   slightly crooked. */
		constexpr float k_slant = 0.235f;

		void paint_mark(ImDrawList* draw_list, float width, float height,
			float logo_progress, float word_slide, float logo_alpha, float word_alpha)
		{
			const float scale = std::max(0.55f, height / 720.f);

			/* the mark's height on screen — the one real measurement */
			const float mark_h = 150.f * scale;

			/* The artwork is wider than it is tall, and the atlas bakes it into a
			   SQUARE tile with the shape letterboxed inside. So the blit has to be
			   square, and the mark is the band across the middle of it. Read from
			   the source rather than hardcoded: re-crop the asset and this follows. */
			const math::c_vector_2d art = g_icons->dimensions("logo");
			const float aspect = (art.y > 0.f) ? (art.x / art.y) : 1.f;

			const float mark_w = mark_h * std::max(aspect, 0.01f);
			const float tile = std::max(mark_w, mark_h);

			const float word_size = mark_h * k_word_ratio;
			const float gap = mark_h * k_gap_ratio;

			bool synthesized = true;
			ImFont* font = display_italic(word_size, synthesized);

			/* a real italic needs no shear; a borrowed roman does */
			const float slant = synthesized ? k_slant : 0.f;

			const float tracking = word_size * k_tracking_ratio;
			const float word_width = tracked_width(font, word_size, "anta", tracking);

			/* a shear pushes the tops of the letters right, so a sheared word's ink
			   reaches further than its advance width says. A real italic already
			   carries that in its own metrics, so there is nothing to add. */
			const float lean = cap_height(font, word_size) * slant;

			const float total = mark_w + gap + word_width + lean;

			const float origin_x = (width - total) * 0.5f;
			const float centre_y = height * 0.5f;

			const float mark_top = centre_y - mark_h * 0.5f;
			const float baseline = mark_top + mark_h;

			/* @nothing behind it.

			   There was a wide soft darkening here, to buy the violet mark contrast
			   against the violet frame. On paper it worked; on screen it read as a
			   grey box parked in the middle of the picture, and the hard-edged
			   contrast step around it was itself most of what looked like a glow on
			   the mark. The frame is already dark by this point in the sequence
			   (see `dim`), which is contrast enough — so the mark sits on the flood
			   with nothing between them. */

			/* --- the mark ---------------------------------------------------- */
			if (logo_alpha > 0.004f)
			{
				/* eased on its own curve so it settles a beat before the word starts,
				   which is what makes the two read as a sequence and not a pair */
				const float grown = 0.72f + 0.28f * anim::ease_out_back(logo_progress);

				/* scaled about its own centre, so the entrance never moves the
				   lockup the word has already been laid out against */
				const float drawn = tile * grown;

				const float centre_x = origin_x + mark_w * 0.5f;
				const float blit_x = centre_x - drawn * 0.5f;
				const float blit_y = centre_y - drawn * 0.5f;

				/* No bloom behind it. The mark is a neon shape and it was tried both
				   ways: light thrown onto violet from a violet mark has nothing to be
				   brighter than, so it reads as a smudge with the ribbon dissolving
				   inside it. The darkened pool above is what gives it its contrast. */
				const ImTextureID texture = g_icons->get("logo", k_logo_bake);

				if (texture != 0)
				{
					draw_list->AddImage(texture,
						ImVec2(blit_x, blit_y), ImVec2(blit_x + drawn, blit_y + drawn),
						ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
						k_mark.modulate(logo_alpha).transform());
				}
				else
				{
					/* Packaging fallback: keep the inject sequence legible even when
					   the bitmap is absent. The dedicated vanta_assets deployment
					   should make this exceptional rather than the normal path. */
					const ImU32 color = k_mark.modulate(logo_alpha).transform();
					const float stroke = std::max(4.f, drawn * 0.09f);
					draw_list->AddLine(ImVec2(blit_x + drawn * 0.22f, blit_y + drawn * 0.18f),
						ImVec2(blit_x + drawn * 0.50f, blit_y + drawn * 0.82f), color, stroke);
					draw_list->AddLine(ImVec2(blit_x + drawn * 0.50f, blit_y + drawn * 0.82f),
						ImVec2(blit_x + drawn * 0.78f, blit_y + drawn * 0.18f), color, stroke);
				}
			}

			/* --- the rest of the word ----------------------------------------- */
			if (word_alpha > 0.004f && font != nullptr)
			{
				/* Slides the last few pixels out from behind the mark, so it reads as
				   having come OUT of the V rather than as having faded in beside it —
				   and, on the way out, keeps going the same way. The caller hands us a
				   signed offset rather than a 0..1 progress precisely so the exit is
				   not forced to run the entrance backwards. */
				const float word_x = origin_x + mark_w + gap + word_slide * scale;

				draw_list->PushTextureID(vanta::modules::arraylist::shared_pool().texture_id());

				/* Plain white, no halo. The letters are legible because the frame was
				   darkened under them, which is a cleaner way to win contrast than
				   lighting the type — a glow at this size fattens the strokes and the
				   counters silt up. */
				put_tracked(draw_list, font, word_size, "anta", word_x, baseline,
					theme::text_white.modulate(word_alpha), slant, tracking);

				draw_list->PopTextureID();
			}
		}

		/* ------------------------------------------------------------------ */
		/* the greeting                                                        */
		/* ------------------------------------------------------------------ */

		/* @paint_welcome:
			"Welcome Back, <name>", on the untouched game.

			The greeting and the name are set differently on purpose. They are not
			the same kind of thing — one is a fixed phrase and the other is the
			only word on screen that is about the person reading it — so the phrase
			is smaller and stepped back in a muted lilac while the name carries full
			size and full white. That is the whole design: no rule, no box, no
			second colour beyond those two.

			What it deliberately does NOT do any more is glow. It used to be stamped
			twenty times around itself to fake a halo, and at this size the stamps
			do not blur together into light — they stay legible as twenty offset
			copies, and every letter came out with a violet fringe crawling around
			it. A single soft darkening underneath does the one job the glow was
			hired for, which is keeping white type readable over a bright sky.
		*/
		void paint_welcome(ImDrawList* draw_list, float width, float height, float progress, float opacity)
		{
			if (opacity <= 0.004f)
				return;

			const float scale = std::max(0.55f, height / 720.f);
			const float size = 38.f * scale;

			/* the pool only carries the bold cut, so the step down to the phrase is
			   made with size and colour rather than with weight */
			const float lead_size = size * 0.82f;

			ImFont* font = display_font(size);
			ImFont* lead_font = display_font(lead_size);

			if (font == nullptr || lead_font == nullptr)
				return;

			const std::string lead = "Welcome Back, ";
			const std::string name = g_name.empty() ? std::string{ "Player" } : g_name;

			const float tracking = size * 0.02f;

			const float lead_width = tracked_width(lead_font, lead_size, lead.c_str(), tracking);
			const float name_width = tracked_width(font, size, name.c_str(), tracking);

			const float total = lead_width + name_width;

			/* rises the last few pixels into place, and keeps rising as it leaves */
			const float lift = (1.f - progress) * 22.f * scale;

			const float x = (width - total) * 0.5f;
			const float baseline = height * 0.5f + size * 0.32f + lift;

			draw_list->PushTextureID(vanta::modules::arraylist::shared_pool().texture_id());

			/* @nothing behind the letters.

			   This has had a full-width dark bloom, then a four-way black stamp
			   around every glyph, and each version was the same mistake at a smaller
			   size: anything opaque drawn behind type at this scale stops being a
			   backing and becomes a shape of its own. The bloom read as a cloud; the
			   stamps read as an outline, because a ring of black on all four sides
			   of a letter IS an outline.

			   So there is nothing behind it. The greeting is white on whatever it
			   lands on, and it lands for about two seconds — long enough to read,
			   short enough that an unlucky background is a moment rather than a
			   problem. */

			/* both runs sit on ONE baseline, so the size difference reads as
			   hierarchy rather than as two lines that failed to line up */
			put_tracked(draw_list, lead_font, lead_size, lead.c_str(), x, baseline,
				hue::c_color(198, 184, 224).modulate(opacity * 0.92f), 0.f, tracking);

			put_tracked(draw_list, font, size, name.c_str(), x + lead_width, baseline,
				theme::text_white.modulate(opacity), 0.f, tracking);

			draw_list->PopTextureID();
		}
	}

	/* ---------------------------------------------------------------------- */
	/* interface                                                               */
	/* ---------------------------------------------------------------------- */

	void begin()
	{
		g_stage = stage::arming;
		g_clock = 0.f;
		g_wave = 0.f;
		g_name.clear();
	}

	bool active()
	{
		return g_stage != stage::idle && g_stage != stage::done;
	}

	bool just_finished()
	{
		const bool edge = g_finished_edge;
		g_finished_edge = false;
		return edge;
	}

	void tick(float delta)
	{
		if (!active())
			return;

		/* a long stall — the first frames after an inject are the worst ones the
		   client will ever see — must not fast-forward the whole sequence */
		delta = std::clamp(delta, 0.f, 0.05f);

		g_clock += delta;
		g_wave += delta;

		switch (g_stage)
		{
		case stage::arming:
		{
			/* The greeting is addressed to someone, so it waits for the name. It
			   waits a bounded amount: an offline session, or one where the JVM
			   never answers, still gets its inject animation. */
			g_name = profile::name();

			if (!g_name.empty() || g_clock >= k_arm_timeout)
				advance(stage::welcome);

			break;
		}

		case stage::welcome:
			/* The name may land a frame or two after we gave up waiting, and while
			   the line is still fading in there is no harm in taking it. Once it is
			   fully up we stop looking: the line is centred, so adopting a name
			   mid-hold would shunt the whole greeting sideways. */
			if (g_name.empty() && g_clock < k_welcome_in)
				g_name = profile::name();

			if (g_clock >= k_welcome_total)
				advance(stage::flood);
			break;

		case stage::flood:
			if (g_clock >= k_flood)
				advance(stage::mark);
			break;

		case stage::mark:
			if (g_clock >= k_mark_total)
				advance(stage::hold);
			break;

		case stage::hold:
			if (g_clock >= k_hold)
				advance(stage::mark_out);
			break;

		case stage::mark_out:
			if (g_clock >= k_mark_out_total)
				advance(stage::drain);
			break;

		case stage::drain:
			if (g_clock >= k_drain)
			{
				advance(stage::done);
				g_finished_edge = true;
			}
			break;

		default:
			break;
		}
	}

	void render(int fb_w, int fb_h)
	{
		if (!active() || fb_w <= 0 || fb_h <= 0)
			return;

		ImDrawList* draw_list = ImGui::GetForegroundDrawList();
		if (draw_list == nullptr)
			return;

		const float width = static_cast<float>(fb_w);
		const float height = static_cast<float>(fb_h);

		/* gl_hook seeds io.DisplaySize to the CANVAS, so every draw list opens the
		   frame clipped to 1120x700. We are painting the whole framebuffer, which
		   on anything larger sits well outside that — so the clip is REPLACED
		   (non-intersecting) rather than pushed inside it. */
		draw_list->PushClipRect(ImVec2(0.f, 0.f), ImVec2(width, height), false);

		ImDrawList* previous = g_render->draw_list();
		g_render->set_draw_list(draw_list);

		/* --- how far the violet has come ---------------------------------- */
		float fill = 0.f;
		float unmake = 0.f;
		float dim = 0.f;
		float motion = 0.f;

		switch (g_stage)
		{
		case stage::flood:
		{
			const float rise = g_clock / k_flood;

			/* Eased in and out: it starts as a surge and arrives as a settle, which
			   is what water does and what a linear rise does not. It is taken a
			   little past the top of the frame so the crest leaves the picture
			   instead of flattening against it. */
			fill = anim::ease_in_out_sine(rise) * 1.10f;
			dim = anim::saturate((g_clock - k_flood * 0.55f) / (k_flood * 0.45f)) * 0.35f;

			/* how alive the surface still is. It drives the crest's height, the
			   uneven lead of the rising edge and the haze thrown ahead of it — all
			   three belong to the climb, and all three have to be gone by the time
			   the sheet is standing still, or the settled frame keeps rippling. */
			motion = 1.f - anim::ease_in_out_sine(anim::saturate((rise - 0.45f) / 0.55f));
			break;
		}

		case stage::mark:
		case stage::hold:
		case stage::mark_out:
			fill = 1.10f;
			dim = 0.35f + 0.25f * anim::saturate(g_clock * 1.2f);
			break;

		case stage::drain:
		{
			fill = 1.10f;
			unmake = anim::saturate(g_clock / k_drain);
			dim = 0.60f * (1.f - unmake * 0.55f);

			/* the surface wakes back up as it lets go — a sheet that is falling is
			   not a sheet that is still */
			motion = anim::saturate(unmake * 1.6f) * 0.7f;
			break;
		}

		default:
			break;
		}

		if (fill > 0.f)
			paint_liquid(draw_list, width, height, fill, unmake, dim, motion);

		/* --- the greeting -------------------------------------------------- */
		if (g_stage == stage::welcome)
		{
			float opacity = 1.f;
			float progress = 1.f;

			if (g_clock < k_welcome_in)
			{
				progress = anim::ease_out_cubic(g_clock / k_welcome_in);
				opacity = progress;
			}
			else if (g_clock > k_welcome_in + k_welcome_hold)
			{
				const float out = (g_clock - k_welcome_in - k_welcome_hold) / k_welcome_out;

				opacity = 1.f - anim::ease_in_out_sine(out);

				/* keeps drifting up on the way out, so it leaves rather than
				   switching off */
				progress = 1.f + out * 0.8f;
			}

			paint_welcome(draw_list, width, height, progress, opacity);
		}

		/* --- the mark ------------------------------------------------------ */
		if (g_stage == stage::mark || g_stage == stage::hold || g_stage == stage::mark_out)
		{
			float logo_progress = 1.f, logo_alpha = 1.f;
			float word_slide = 0.f, word_alpha = 1.f;

			if (g_stage == stage::mark)
			{
				logo_progress = anim::saturate(g_clock / k_logo_in);
				logo_alpha = anim::ease_out_cubic(anim::saturate(g_clock / (k_logo_in * 0.7f)));

				const float word_clock = g_clock - k_word_delay;

				word_slide = -18.f * (1.f - anim::ease_out_cubic(anim::saturate(word_clock / k_word_in)));
				word_alpha = anim::ease_out_cubic(anim::saturate(word_clock / (k_word_in * 0.65f)));
			}
			else if (g_stage == stage::mark_out)
			{
				/* THE LETTERS FIRST. The wordmark is the message and the logo is
				   the object it belongs to; taking the object away first would
				   leave the message floating unattached for a fifth of a second,
				   and that reads as a glitch rather than as an exit. */
				word_alpha = 1.f - anim::ease_in_out_sine(anim::saturate(g_clock / k_word_out));
				word_slide = 22.f * (1.f - word_alpha);

				const float logo_clock = g_clock - k_logo_out_delay;

				logo_alpha = 1.f - anim::ease_in_out_sine(anim::saturate(logo_clock / k_logo_out));
			}

			paint_mark(draw_list, width, height, logo_progress, word_slide, logo_alpha, word_alpha);
		}

		g_render->set_draw_list(previous);
		draw_list->PopClipRect();
	}
}
