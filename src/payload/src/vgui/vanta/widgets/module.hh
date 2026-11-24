#pragma once
#include "../../../util/StringScrub.h"

/* @vanta::module:
	a module is one row in the grid plus the settings drawer that opens beside it.

	the card owns its own spring physics (visual_y / visual_w) so a category switch or the sidecar
	compressing the list reads as movement rather than a jump cut.
*/
namespace vanta
{
	/* ------------------------------------------------------------------ */
	/* settings                                                            */
	/* ------------------------------------------------------------------ */

	class c_setting
	{
	public:
		explicit c_setting(std::string name) : m_name(std::move(name)) { }
		virtual ~c_setting()
		{
			vanta::util::scrubString(this->m_name);
            vanta::util::scrubString(this->m_label);
			vanta::util::scrubString(this->m_description);
		}

		/* @update:
			pulls the animation values towards their targets
		*/
		virtual void update(float delta) = 0;

		/* @height:
			vertical space this row consumes inside the drawer
		*/
		virtual float height() const = 0;

		/* @render:
			draws the row and consumes its own input
		*/
		virtual void render(float x, float y, float w, const frame_input_t& input, float alpha) = 0;

		/* @propagate_if_changed:
			called by c_module::render_settings after render() every frame.
			Each derived setting compares its current value against the last
			one pushed to the real vanta::Setting and, if different, forwards
			it through vgui::bridge::apply* — the same plumbing that already
			exists but that no widget was calling. Default is no-op so a
			setting without a mirror still compiles.
		*/
		virtual void propagate_if_changed(const std::string& /*module_name*/) { }

		const std::string& name() const { return this->m_name; }
        const std::string& label() const { return m_label.empty() ? m_name : m_label; }
        void set_label(std::string text) { m_label = std::move(text); }

		/* @visibility:
			mirrors vanta::Setting::setVisibleIf. A row whose predicate returns
			false is skipped entirely by the drawer — it consumes no height and
			receives no input, so a setting that only applies to one mode stops
			existing when that mode is not selected.

			Evaluated EVERY frame, so switching a mode enum re-lays-out the
			drawer immediately.
		*/
		void set_visible_if(std::function<bool()> pred) { this->m_visible = std::move(pred); }
		bool visible() const { return !this->m_visible || this->m_visible(); }

		/* @description:
			mirrors vanta::Setting::description. When non-empty the drawer shows it
			as a tooltip beside the cursor after the row has been hovered for a
			moment -- see c_module::render_settings. Empty rows show nothing.
		*/
		void set_description(std::string text) { this->m_description = std::move(text); }
		const std::string& description() const { return this->m_description; }

	protected:
		std::string m_name{};
        std::string m_label{};
		std::string m_description{};
		std::function<bool()> m_visible{};
	};

	class c_slider_setting final : public c_setting
	{
	public:
		c_slider_setting(std::string name, double minimum, double maximum, double value, double step, std::string suffix);

		void update(float delta) override;
		float height() const override { return 46.f; }
		void render(float x, float y, float w, const frame_input_t& input, float alpha) override;
		void propagate_if_changed(const std::string& module_name) override;

		double value() const { return this->m_value; }

		/* @set_value:
			adopt an externally owned value (e.g. a setting whose real source of
			truth lives outside the gui). Snaps the fill animation so the row
			does not sweep across on the frame it is adopted.
		*/
		void set_value(double v)
		{
			this->m_value = std::clamp(v, this->m_minimum, this->m_maximum);
			this->m_visual_value = static_cast<float>(
				(this->m_value - this->m_minimum) / (this->m_maximum - this->m_minimum));
		}

		bool dragging() const { return this->m_dragging; }

		/* @set_screen_anchored_drag:
			opt in to resolving the drag in DESKTOP pixels instead of the gui's
			logical space.

			Needed by any slider whose value changes the ui scale: resizing the
			panel re-maps logical -> screen, so a logical-space drag feeds its
			own output back into its input and the handle crawls away from the
			cursor. Anchoring to the desktop makes the value track the cursor's
			absolute motion, which is stable no matter how the panel resizes.
		*/
		void set_screen_anchored_drag(bool enabled) { this->m_screen_anchored = enabled; }

		/* ------------------------------------------------------------------ */
		/* typing a value                                                      */
		/* ------------------------------------------------------------------ */

		/* @value_focus:
			the chip currently accepting keystrokes, or nullptr.

			A single static, for the same reason the hex field has one: only one
			caret can exist at a time, and the shell needs to answer "is anyone
			typing?" without walking every module's settings to find out. Cleared by
			the destructor so a drawer torn down mid-edit cannot leave it dangling.
		*/
		static c_slider_setting* value_focus() { return s_value_focus; }
		static void clear_value_focus();

		~c_slider_setting() override;

		void on_character(char character);
		void on_backspace();
		void submit_value();

	private:
		void begin_value_edit();

		/* @apply_typed:
			parses the buffer and adopts it, CLAMPED to the slider's own range.

			Clamped rather than rejected: someone typing 500 into a field that stops
			at 100 means "as high as it goes", and refusing the whole entry because
			its last digit went too far would be pedantry. The step is applied too,
			so a typed value lands on the same grid a dragged one does.
		*/
		bool apply_typed(const std::string& text);

		static inline c_slider_setting* s_value_focus{ nullptr };

		std::string m_typed{};
		float m_chip_hover{ 0.f };
		float m_chip_focus{ 0.f };

		double m_minimum{};
		double m_maximum{};
		double m_value{};
		double m_step{};
		std::string m_suffix{};

		bool m_dragging{ false };
		float m_hover{ 0.f };
		float m_visual_value{ 0.f };
		anim::pulse_t m_grab{};

		/* screen anchored drag state (see set_screen_anchored_drag) */
		bool m_screen_anchored{ false };
		POINT m_anchor_cursor{};
		double m_anchor_value{};
		float m_anchor_track_px{ 1.f };

		/* last value forwarded to the real vanta::Setting through the bridge.
		   NaN sentinel forces the first propagate_if_changed to always push
		   the current value even if the widget spawned already matching. */
		double m_last_pushed{
			std::numeric_limits<double>::quiet_NaN()
		};
	};

	/* @c_range_slider_setting:
		a two-thumb slider. Both handles ride the same capsule track and their
		fill is the segment between them. The min thumb cannot pass the max
		thumb (and vice-versa); the row shows a "min – max" chip on the right.

		Backed by vanta::RangeSetting through vgui::bridge::applyRange.
	*/
	class c_range_slider_setting final : public c_setting
	{
	public:
		c_range_slider_setting(std::string name,
			double absolute_min, double absolute_max,
			double value_min, double value_max,
			double step, std::string suffix);

		void  update(float delta) override;
		float height() const override { return 46.f; }
		void  render(float x, float y, float w, const frame_input_t& input, float alpha) override;
		void  propagate_if_changed(const std::string& module_name) override;

		double min_value() const { return this->m_min; }
		double max_value() const { return this->m_max; }

	private:
		double m_absolute_min{};
		double m_absolute_max{};
		double m_min{};
		double m_max{};
		double m_step{};
		std::string m_suffix{};

		enum class thumb { none, low, high } m_dragging{ thumb::none };
		float m_hover{ 0.f };
		float m_visual_min{ 0.f };
		float m_visual_max{ 1.f };
		anim::pulse_t m_grab_low{};
		anim::pulse_t m_grab_high{};

		/* last (min,max) pushed through the bridge — NaN sentinels force the
		   first propagate_if_changed to always emit, matching c_slider_setting. */
		double m_last_pushed_min{ std::numeric_limits<double>::quiet_NaN() };
		double m_last_pushed_max{ std::numeric_limits<double>::quiet_NaN() };
	};

	class c_boolean_setting final : public c_setting
	{
	public:
		c_boolean_setting(std::string name, bool value);

		void update(float delta) override;
		float height() const override { return 34.f; }
		void render(float x, float y, float w, const frame_input_t& input, float alpha) override;
		void propagate_if_changed(const std::string& module_name) override;

		bool enabled() const { return this->m_enabled; }

		/* @set_enabled:
			adopt a value from outside. Does NOT snap the knob — the switch is on
			screen when Reset is pressed, and a knob that teleports says nothing
			about what just happened.
		*/
		void set_enabled(bool on) { this->m_enabled = on; }

	private:
		bool m_enabled{ false };
		float m_toggle{ 0.f };
		float m_hover{ 0.f };

		/* the knob carries momentum into its stop, a lerp would just creep there */
		anim::spring_t m_knob{};
		bool m_knob_ready{ false };
		anim::pulse_t m_flip{};

		/* -1 = "never pushed"; forces the first frame to sync to real Setting */
		int m_last_pushed{ -1 };
	};

	/* @c_color_setting:
		one RGBA colour, edited through a full picker: a saturation / value
		square, a hue rail, an alpha rail, and a hex field that can be read,
		copied and typed into.

		Every handle carries its own eased position rather than being drawn
		straight from the value, so dragging one rail moves the others' markers
		instead of teleporting them.
	*/
	class c_color_setting final : public c_setting
	{
	public:
		c_color_setting(std::string name, float r, float g, float b, float a);
		~c_color_setting() override;

		/* @editor_height:
			how tall the picker itself is, wherever it is drawn.

			Not height(). height() is what this setting costs its OWNER — one
			compact row — because the picker no longer lives inside the list that
			owns it; it opens in a window of its own (see vanta::colour_popup).
		*/
		static constexpr float editor_height = 214.f;

		void update(float delta) override;
		float height() const override { return 34.f; }

		/* the compact row: label, swatch, and a control that opens the editor */
		void render(float x, float y, float w, const frame_input_t& input, float alpha) override;

		/* @render_editor:
			the picker proper — square, rails, hex field. Drawn by the popup, never
			by the row.
		*/
		void render_editor(float x, float y, float w, const frame_input_t& input, float alpha);

		void propagate_if_changed(const std::string& module_name) override;

		float r() const { return this->m_r; }
		float g() const { return this->m_g; }
		float b() const { return this->m_b; }
		float a() const { return this->m_a; }

		void set_rgba(float r, float g, float b, float a);

		/* @hex:
			the current colour as "RRGGBB", uppercase, no leading hash
		*/
		std::string hex() const;

		/* ------------------------------------------------------------------ */
		/* hex entry                                                           */
		/* ------------------------------------------------------------------ */

		/* @hex_focus:
			the field currently accepting typed characters, or nullptr.

			A single static rather than a flag per widget: only one caret can
			exist at a time, and the shell needs to answer "is anyone typing?"
			without walking every module's settings to find out. Cleared by the
			destructor, so a drawer that is torn down while focused cannot leave
			this dangling.
		*/
		static c_color_setting* hex_focus() { return s_hex_focus; }
		static void clear_hex_focus();

		/* fed by the shell's keyboard dispatch while this field owns the caret */
		void on_character(char character);
		void on_backspace();
		void submit_hex();

		/* @paste_clipboard:
			adopts a colour off the clipboard, if that is what is on it.

			Deliberately all-or-nothing: the whole string has to BE a colour, once a
			leading hash and any surrounding space are taken off. Scraping hex digits
			out of arbitrary text sounds friendlier and is not — "hello" contains an
			'e', and half the words anyone might have copied would silently repaint
			the client. Returns false and changes nothing when the clipboard is not a
			colour.
		*/
		bool paste_clipboard();

		/* @copy_clipboard:
			puts "#RRGGBB" on the clipboard. Shared by the copy button and Ctrl+C.
		*/
		void copy_clipboard();

	private:
		void refresh_hsv_from_rgb();
		void refresh_rgb_from_hsv();

		/* @apply_hex:
			parses the buffer as #RGB or #RRGGBB and adopts it. Returns false and
			changes nothing when the buffer is not yet a complete colour, which is
			what lets the field update live as the sixth digit is typed without
			flickering through the partial ones.
		*/
		bool apply_hex(const std::string& text);

		void begin_hex_edit();

		/* @render_hex_field:
			the readable / typable / copyable row under the rails. Split out of
			render() because it owns four animations and two hit rects of its own,
			and folding that into the picker's body made neither readable.
		*/
		void render_hex_field(float x, float y, float w, float h,
			const frame_input_t& input, float alpha);

		float m_r{ 1.f }, m_g{ 1.f }, m_b{ 1.f }, m_a{ 1.f };

		// Cached HSV so the picker doesn't lose hue at s=0 or v=0 (the two
		// axes where the RGB round-trip collapses to a single number).
		float m_hue{ 0.f };
		float m_sat{ 0.f };
		float m_val{ 1.f };

		/* eased mirrors of the three above, and the only thing the handles are
		   ever drawn from */
		float m_visual_hue{ 0.f };
		float m_visual_sat{ 0.f };
		float m_visual_val{ 1.f };
		float m_visual_alpha{ 1.f };
		bool  m_visual_ready{ false };

		enum class drag_target { none, hue, sv, alpha };
		drag_target m_drag{ drag_target::none };

		float m_hover{ 0.f };
		float m_row_hover{ 0.f };

		/* @m_anim_frame:
			the frame update() last ran on.

			The picker is now ticked from two places — the list that owns the row,
			and the popup that owns the editor — and on any frame where both are on
			screen it would otherwise advance twice, running every eased handle at
			double speed exactly when the editor is open.
		*/
		int m_anim_frame{ -1 };

		/* hex field */
		std::string m_hex_buffer{};
		float m_hex_hover{ 0.f };
		float m_hex_focus_anim{ 0.f };
		float m_hex_caret_x{ -1.f };

		/* copy button: hover, plus the "done" state it holds for a moment after
		   the value actually reaches the clipboard */
		float m_copy_hover{ 0.f };
		float m_copied{ 0.f };
		float m_copied_hold{ 0.f };
		anim::pulse_t m_copy_pulse{};

		static inline c_color_setting* s_hex_focus{ nullptr };

		// Sentinel triple that forces the first propagate_if_changed after
		// construction to push whatever the widget spawned with.
		float m_pushed_r{ std::numeric_limits<float>::quiet_NaN() };
		float m_pushed_g{ 0.f }, m_pushed_b{ 0.f }, m_pushed_a{ 0.f };
	};

	/* @c_slot_map_setting:
		nine squares laid out like the hotbar itself, one assignment each.

		Clicking a square advances it to the next SELECTABLE option and wraps
		out through empty. A cycle rather than a popup because the option list
		is short and, more usefully, because a cycle cannot land on something
		that is not selectable — the constraint is enforced by the walk itself
		instead of by a menu that has to remember to grey entries out.

		Which options are selectable is not the widget's business: it asks
		through m_allowed every frame, so ticking a kind off upstream empties
		the squares that used it on the very next frame with nothing having to
		notify anyone.
	*/
	class c_slot_map_setting final : public c_setting
	{
	public:
		static constexpr int SLOTS = 9;

		c_slot_map_setting(std::string name, std::vector<std::string> options);
		~c_slot_map_setting() override { vanta::util::scrubStringVector(this->m_options); }

		void update(float delta) override;
		float height() const override { return 66.f; }
		void render(float x, float y, float w, const frame_input_t& input, float alpha) override;
		void propagate_if_changed(const std::string& module_name) override;

		/* Source of truth for the values and for what may be picked. Both read
		   straight from the real setting, so the widget holds no stale copy. */
		void set_slot_provider(std::function<int(int)> p)       { this->m_slot = std::move(p); }
		void set_allowed_provider(std::function<std::uint32_t()> p) { this->m_allowed = std::move(p); }

	private:
		std::vector<std::string> m_options{};
		std::function<int(int)>  m_slot{};
		std::function<std::uint32_t()> m_allowed{};

		/* Which square the user clicked this frame; -1 when none. Consumed by
		   propagate_if_changed so the cycle happens on the real setting and the
		   widget never owns a second copy of the value. */
		int   m_pending_cycle{ -1 };
		float m_hover[SLOTS]{};

		/* Assignment animation. m_last only ever exists to spot the frame the
		   value changed -- the value itself is still read from the provider, so
		   this is a change detector, not a second copy. */
		int   m_last[SLOTS]{};
		int   m_art[SLOTS]{};    /* last drawable option, so a cleared slot can
		                            shrink its item away instead of blinking out */
		float m_pop[SLOTS]{};    /* 1 on change, decays: drives scale + ring   */
		float m_fill[SLOTS]{};   /* eases 0..1 so the accent fades in and out  */
		bool  m_seeded{ false };

		/* Vector item art, drawn at the real Minecraft colours. */
		static void draw_potion(float cx, float cy, float s, hue::c_color liquid, float alpha);
		static void draw_totem(float cx, float cy, float s, float alpha);
		void draw_item(int option, float cx, float cy, float s, float alpha) const;
	};

	class c_mode_setting final : public c_setting
	{
	public:
		c_mode_setting(std::string name, std::vector<std::string> modes, int index);
		~c_mode_setting() override { vanta::util::scrubStringVector(this->m_modes); }

		void update(float delta) override;
		float height() const override;
		void render(float x, float y, float w, const frame_input_t& input, float alpha) override;
		void propagate_if_changed(const std::string& module_name) override;

		const std::string& selected() const { return this->m_modes[static_cast<std::size_t>(this->m_index)]; }

	private:
		/* @when the capsule stops being a capsule, measured rather than counted.

		   The segmented control splits a fixed 210px evenly, so the question is
		   whether the words fit in 210/n. That was approximated by a count — more
		   than four options, use a dropdown — and the approximation broke the
		   moment a row had four LONG options: the Target HUD's font list is
		   exactly four, so it kept the capsule, and "Proxima Bold" was drawn
		   straight through its neighbour.

		   The count was always a proxy for the width. This asks the width. */
		static constexpr float k_track_width = 210.f;
		static constexpr float k_option_font = 10.5f;

		/* Room for the glyphs plus the breathing space either side of them; below
		   this the words touch, which is the thing being avoided. */
		static constexpr float k_option_padding = 10.f;

		bool uses_dropdown() const;

		/* @measured once, not every frame.

		   The options are fixed at construction and so are the face and the
		   size, so the answer cannot change — but height() is asked for it twice
		   a frame for every row in an open drawer, and measuring a string is not
		   free. Resolved lazily rather than in the constructor because the font
		   atlas is not up yet when the cards are built. */
		mutable int m_dropdown{ -1 };   /* -1 unknown, 0 capsule, 1 dropdown */

		void render_segmented(float x, float y, float w, const frame_input_t& input, float alpha);
		void render_dropdown(float x, float y, float w, const frame_input_t& input, float alpha);

		std::vector<std::string> m_modes{};
		int m_index{ 0 };
		float m_hover{ 0.f };
		float m_slide{ 0.f };
		anim::pulse_t m_pick{};

		/* @the list opens IN FLOW, pushing the rows below it down.

		   The obvious build is a popup floating over the drawer, and it is the
		   wrong one here: the settings drawer scrolls inside its own clip rect, so
		   a floating list is cut off at the drawer's edge, and it would need the
		   same publish-a-rect-and-shield dance the account panel uses to stop the
		   rows underneath answering its clicks. Growing the row instead costs one
		   virtual height() that is already called per frame, and the list cannot
		   be clipped by a region it is inside of. */
		bool m_open{ false };
		anim::spring_t m_expand{};
		std::vector<float> m_option_hover{};

		/* INT_MIN sentinel so first propagate_if_changed always fires */
		int m_last_pushed{ std::numeric_limits<int>::min() };
	};

	/* ------------------------------------------------------------------ */
	/* block picker                                                        */
	/* ------------------------------------------------------------------ */

	/* @a searchable multi-select for Minecraft block identifiers.

	   The row collapsed is a compact bar showing "N selected" plus a chevron.
	   Expanded, it grows the drawer in-flow (same spring as c_mode_setting) and
	   reveals: a chip strip of currently-picked blocks (click to un-pick), a
	   search field that filters by identifier substring, and a scrollable list
	   of matches — click a row to toggle it in / out of the selection.

	   The list of ALL known blocks is fetched every frame via
	   vgui::bridge::blockPickerRows(), which the engine populates from
	   Registries.BLOCK at world-load. The widget never touches JNI itself. */
	class c_block_picker_setting final : public c_setting
	{
	public:
		c_block_picker_setting(std::string name, std::vector<std::string> initial);
        ~c_block_picker_setting() override {
            if (s_search_focus == this) s_search_focus = nullptr;
            vanta::util::scrubStringVector(this->m_selected);
            vanta::util::scrubString(this->m_search);
        }

		void update(float delta) override;
		float height() const override;
		void render(float x, float y, float w, const frame_input_t& input, float alpha) override;
		void propagate_if_changed(const std::string& module_name) override;

		/* @search text input focus — same static-owner pattern as
		   c_slider_setting::value_focus and c_color_setting::hex_focus. Only
		   one picker's search field owns the caret at a time; app.cc routes
		   character events to it. */
		static c_block_picker_setting* search_focus() { return s_search_focus; }
		static void clear_search_focus() { s_search_focus = nullptr; }

		void on_character(char character);
		void on_backspace();

	private:
		void render_collapsed(float x, float y, float w, const frame_input_t& input, float alpha);
		void render_expanded(float x, float y, float w, const frame_input_t& input, float alpha);

		std::vector<std::string> m_selected{};
		std::string m_search{};
		float m_scroll{ 0.f };
		float m_scroll_target{ 0.f };
		bool  m_open{ false };
		anim::spring_t m_expand{};
		float m_hover_header{ 0.f };
		int   m_hover_row{ -1 };

		/* @propagate seam — mirror the picker's selection into the real
		   BlockSetSetting when it changes. Compares the size + hash of ids
		   against the last push so a click that turns out to be a no-op does
		   not fire an apply. */
		std::size_t m_last_pushed_hash{ 0 };
		std::size_t m_last_pushed_size{ static_cast<std::size_t>(-1) };

		static inline c_block_picker_setting* s_search_focus{ nullptr };

		static std::size_t hash_ids(const std::vector<std::string>& ids);
	};

	/* ------------------------------------------------------------------ */
	/* module                                                              */
	/* ------------------------------------------------------------------ */

	class c_module
	{
	public:
		c_module(std::string name, std::string description, std::string category);
		~c_module()
		{
			if (s_binding == this) s_binding = nullptr;
			this->m_settings.clear();
			vanta::util::scrubString(this->m_name);
			vanta::util::scrubString(this->m_description);
			vanta::util::scrubString(this->m_category);
			vanta::util::scrubString(this->m_keybind);
		}

		/* @add_setting:
			appends a row to the settings drawer, returns the module so calls can chain
		*/
		c_module* add_setting(std::shared_ptr<c_setting> setting);

		/* @settings:
			read access to the rows, used by the bridge to attach a visibility
			predicate to whichever row it just appended
		*/
		const std::vector<std::shared_ptr<c_setting>>& settings() const { return this->m_settings; }

		c_module* set_enabled(bool enabled);
		c_module* set_keybind(std::string keybind);

		/* @set_keybind_vk:
			adopt a real Win32 virtual key. The label is DERIVED from it.

			The card used to hold only the label, and that was the bug: the label
			came out of ImGui's key-name table, which its own header calls debug
			output not meant to be saved or compared, and nothing in the engine
			speaks it. The number is the value; the string is what the number
			looks like. 0 means unbound and shows as "NONE".
		*/
		void set_keybind_vk(int vk);
		int  keybind_vk() const { return this->m_keybind_vk; }

		/* @cancel_binding:
			disarms whichever pill is waiting, if any.

			Hiding the panel has to call this. Once hidden, the shell returns
			before it dispatches anything, so an armed pill left behind would sit
			there silently eating the first key pressed after the panel comes
			back.
		*/
		static void cancel_binding();
		static bool binding_active() { return s_binding != nullptr || s_auxiliary_binding; }
        static bool auxiliary_binding_active() { return s_auxiliary_binding; }
        static void set_auxiliary_binding(bool active) { s_auxiliary_binding = active; }

		/* @update:
			advances every animation the module owns
		*/
		void update(float delta);

		/* @render_card:
			draws the module row. returns true when the row asked for its drawer to be toggled.
		*/
		bool render_card(float target_x, float target_y, float target_w, float h,
			bool selected_in_sidecar, const frame_input_t& input, float alpha);

		/* @render_settings:
			draws every setting row with a staggered spring entrance
		*/
		void render_settings(float x, float y, float w, const frame_input_t& input, float alpha);

		/* @settings_height:
			total vertical space every setting row would consume — used by
			the module manager to clamp the drawer's scroll offset.
		*/
		float settings_height() const;

		/* @keybind_hit_t:
			where the keybind pill landed, so the caller can lay text out next to it
		*/
		struct keybind_hit_t
		{
			bool m_clicked{ false };
			float m_x{ 0.f };
			float m_width{ 0.f };
		};

		/* @render_keybind_button:
			the [ NONE / R ] pill, shared by the card and the drawer header
		*/
		keybind_hit_t render_keybind_button(float right_edge, float center_y,
			const frame_input_t& input, float alpha, float minimum_width, float horizontal_padding);

		const std::string& name() const { return this->m_name; }
		const std::string& description() const { return this->m_description; }
		const std::string& category() const { return this->m_category; }
		const std::string& keybind() const { return this->m_keybind; }

		bool enabled() const;
		bool binding() const { return this->m_binding; }

		/* @only one pill may be armed at a time.

		   The old flag was per-module with nothing coordinating them, so two
		   armed pills both captured the same press and both claimed the key. The
		   static below is the arbiter: arming one disarms the other, exactly the
		   way the hex field and the value chip already share a caret.
		*/
		void set_binding(bool binding);

		/* spring physics, reset by the manager when the category changes */
		float m_visual_y{ -1.f };
		float m_visual_w{ -1.f };

	private:
		std::string m_name{};
		std::string m_description{};
		std::string m_category{};

		bool m_enabled{ false };

		/* The value, and its label. Never the other way round — see
		   set_keybind_vk. */
		int m_keybind_vk{ 0 };
		std::string m_keybind{ "NONE" };

		bool m_binding{ false };

		/* @who is waiting, and what was already held when they started waiting.

		   The capture reads the real keyboard rather than ImGui's key queue, so
		   a modifier the user happened to be holding when they clicked the pill
		   would otherwise be captured instantly. The mask records what was
		   already down at arming time and those keys are ignored until released.

		   Both are static because only one pill can be armed — see set_binding.
		*/
		static inline c_module* s_binding{ nullptr };
        static inline bool s_auxiliary_binding{ false };
		static inline bool s_bind_held[256]{};

		/* @poll_bind_capture:
			reads the keyboard while this pill is armed. Once per frame.
		*/
		void poll_bind_capture();

		float m_toggle{ 0.f };
		float m_hover{ 0.f };
		float m_bind_hover{ 0.f };

		/* @waiting-for-a-key state.

		   m_bind_mix crossfades the pill between its label and the three dots — the
		   pill's width, its violet and its contents all ride that one value, so
		   nothing about the transition is a jump cut. m_bind_phase is the clock the
		   dots march to.

		   m_bind_frame exists because the pill is drawn TWICE on any frame where the
		   settings drawer is open beside its own card, and both copies share this
		   state. Stamping the frame the animation was last advanced on makes the
		   second draw read the values rather than advance them again — otherwise the
		   dots march at double speed exactly when the drawer is open. */
		float m_bind_mix{ 0.f };
		float m_bind_phase{ 0.f };
		float m_bind_glow{ 0.f };
		int   m_bind_frame{ -1 };

		/* @advance_bind_animation:
			ticks the pill's animation once per frame, whoever asks first
		*/
		void advance_bind_animation(float delta);

		/* one shot feedback for the two things a card can be told to do */
		anim::pulse_t m_toggled{};
		anim::pulse_t m_bind_pulse{};

		std::vector<std::shared_ptr<c_setting>> m_settings{};
	};
}
