#pragma once

/* @vanta::text:
	typography layer.

	the framework binds one font handle per pixel size, so the registry bakes every
	(face, size) pair the gui asks for up front and hands them back on demand.

	font_face::script is a joined handwriting face taken from the system. when the machine has
	none of the candidates, it degrades to the bold atlas sheared into an oblique, so the wordmark
	still reads as something other than the roman half.
	draw_text() reproduces the alignment contract the whole gui is laid out against:
	x is the left / center / right anchor, and with align_middle y is the optical center of the line.
*/
namespace vanta
{
	class c_font_registry
	{
	public:
		/* @initialize:
			bakes every face at every size the gui uses. must run before the first frame,
			the atlas cannot grow once it has been uploaded.
		*/
		void initialize();

		/* @get:
			returns the handle for a face at a size, falling back onto the closest baked size
		*/
		c_font* get(font_face face, float size);

		/* @script_is_synthesized:
			true when no handwriting face was found and the script slot is a sheared bold
		*/
		bool script_is_synthesized() const { return this->m_script_synthesized; }

		/* @italic_is_synthesized:
			true when no italic was found and the italic slot is a sheared bold
		*/
		bool italic_is_synthesized() const { return this->m_italic_synthesized; }

		/* @atlas_scale:
			supersampling factor the atlas is currently baked at. every glyph carries FontSize
			== nominal * atlas_scale, so callers must feed the nominal (canvas) size back to
			AddText / CalcTextSizeA and derive canvas metrics as `atlas_metric * (size / FontSize)`.
		*/
		float atlas_scale() const { return this->m_atlas_scale; }

		/* @set_pending_atlas_scale:
			stores the scale the atlas should be baked at. cheap: does not touch the atlas,
			so it is safe to call from the middle of a frame. the frame loop calls this after
			it settles panel_scale, then gl_hook calls apply_pending_atlas_scale before the
			next NewFrame — that is the only moment the atlas can be safely rebuilt.
		*/
		void set_pending_atlas_scale(float desired) { this->m_pending_scale = desired; }

		/* @apply_pending_atlas_scale:
			rebakes the atlas at the pending scale if it drifted enough from the current one.
			returns true if the atlas was rebuilt. must run OUTSIDE NewFrame → Render (atlas is
			locked in that window and Clear() would blow through the lock).
		*/
		bool apply_pending_atlas_scale();

		/* @clear_cap_cache:
			cap-centre memoisation keyed on ImFont* is invalidated when the atlas is rebuilt.
		*/
		void clear_cap_cache();

		/* @cap_centre_of:
			cap-band centre in ATLAS pixels for the passed font, memoised across frames.
		*/
		float cap_centre_of(ImFont* font);

	private:
		struct baked_t
		{
			float m_size{};    // nominal / canvas size the caller asked for
			c_font m_font{};   // ImFont* baked at m_size * m_atlas_scale
		};

		void bake_all(float atlas_scale);

		std::array<std::vector<baked_t>, static_cast<std::size_t>(font_face::count)> m_faces{};
		std::unordered_map<ImFont*, float> m_cap_cache{};
		bool  m_script_synthesized{ true };
		bool  m_italic_synthesized{ true };
		float m_atlas_scale{ 1.f };
		float m_pending_scale{ 0.f };  // 0 == nothing requested yet
	};
	inline auto g_fonts = std::make_unique<c_font_registry>();

	/* @draw_text:
		aligned string rendering, the single entry point the whole gui draws text through
	*/
	void draw_text(const std::string& text, float x, float y, font_face face, float size,
		hue::c_color color, int align = align_left);

	/* @draw_slanted_text:
		the same run, leaning.

		The gui atlas ships one roman cut and no italic, so a lean here is
		synthesised: the glyphs are laid down and then sheared about their
		BASELINE, so the feet stay planted and only the shoulders move. Pivoting
		anywhere else would slide the whole word sideways as it leaned.

		At gui sizes this is indistinguishable from a real italic — the letterform
		differences that give a true italic away (the single-storey 'a' above all)
		are a few pixels across at 14px. The inject sequence, which sets the same
		word at ten times the size, does go and find a real one.
	*/
	void draw_slanted_text(const std::string& text, float x, float y, font_face face, float size,
		hue::c_color color, int align, float slant);

	/* @text_width:
		advance width of a string for a face at a size
	*/
	float text_width(const std::string& text, font_face face, float size);

	/* @text_baseline:
		where the baseline of a middle aligned line ends up. feed it back with align_baseline to
		set a second face level with the first.
	*/
	float text_baseline(font_face face, float size, float center_y);

	/* @truncate_text:
		clips a string to a pixel budget, appending an ellipsis when it had to cut
	*/
	std::string truncate_text(const std::string& text, float max_width, font_face face, float size);

	/* ------------------------------------------------------------------ */
	/* rich text                                                           */
	/* ------------------------------------------------------------------ */

	/* @rich_run_t:
		one piece of a laid out line: a stretch of glyphs, or a single emoji picture
	*/
	struct rich_run_t
	{
		std::string m_text{};
		bool m_emoji{ false };
		float m_x{ 0.f };
		float m_width{ 0.f };
	};

	struct rich_line_t
	{
		std::vector<rich_run_t> m_runs{};
		float m_width{ 0.f };
	};

	/* @rich_text_t:
		a wrapped paragraph that mixes glyphs and colour emoji. measured once, then drawn as many
		times as needed, so a chat can size its bubbles before painting them.
	*/
	struct rich_text_t
	{
		std::vector<rich_line_t> m_lines{};
		float m_width{ 0.f };
		float m_height{ 0.f };
		float m_line_height{ 0.f };
	};

	/* @layout_rich_text:
		wraps on word boundaries, treating every emoji sequence as one unbreakable unit
	*/
	rich_text_t layout_rich_text(const std::string& text, font_face face, float size, float max_width);

	/* @draw_rich_text:
		paints a layout produced above. y is the top of the first line.
	*/
	void draw_rich_text(const rich_text_t& layout, float x, float y, font_face face, float size, hue::c_color color);
}
