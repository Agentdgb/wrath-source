#include "../../includes.hh"

#include "../../bridge.h"

namespace vanta
{
	namespace
	{
		bool equals_ignore_case(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;

			for (std::size_t index = 0; index < a.size(); ++index)
			{
				if (std::tolower(static_cast<unsigned char>(a[index])) != std::tolower(static_cast<unsigned char>(b[index])))
					return false;
			}

			return true;
		}

		std::string to_lower(std::string_view value)
		{
			std::string result(value);
			std::transform(result.begin(), result.end(), result.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });

			return result;
		}

		std::string trim(const std::string& value)
		{
			const auto first = value.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return {};

			const auto last = value.find_last_not_of(" \t\r\n");
			return value.substr(first, last - first + 1);
		}

		std::shared_ptr<c_slider_setting> slider(std::string name, double minimum, double maximum, double value, double step, std::string suffix)
		{
			return std::make_shared<c_slider_setting>(std::move(name), minimum, maximum, value, step, std::move(suffix));
		}

		std::shared_ptr<c_boolean_setting> boolean(std::string name, bool value)
		{
			return std::make_shared<c_boolean_setting>(std::move(name), value);
		}

		std::shared_ptr<c_mode_setting> mode(std::string name, std::vector<std::string> modes, int index)
		{
			return std::make_shared<c_mode_setting>(std::move(name), std::move(modes), index);
		}
	}

	c_module_manager::c_module_manager()
	{
		this->build_default_modules();
	}

	void c_module_manager::reload_cards()
	{
		this->m_sidecar = nullptr;
		this->m_last_open = nullptr;
		this->build_default_modules();
	}

	void c_module_manager::build_default_modules()
	{
		/* Post-port: the cards come from the REAL client module registry
		   (src/core/module/ModuleManager) rather than the demo list that
		   shipped with the standalone GUI. Category strings, setting rows and
		   initial values all mirror the live modules. */
		this->m_modules = vgui::bridge::buildCardsFromRealModules();

		if (!this->m_modules.empty())
			return;

		/* Fallback: the real registry was empty (bootstrap has not run yet, or
		   we are running the GUI standalone for design work). Keep the demo
		   set so the layout is still exercised. */
		const auto add = [this](std::string name, std::string description, std::string category)
			{
				auto created = std::make_shared<c_module>(std::move(name), std::move(description), std::move(category));
				this->m_modules.push_back(created);

				return created;
			};

		/* --- combat --- */
		{
			auto auto_clicker = add("ClickAssist", "Automatically clicks when holding down attack button", "Combat");
			auto_clicker->set_enabled(true)->set_keybind("R")
				->add_setting(slider("Left CPS", 1.0, 20.0, 14.5, 0.5, ""))
				->add_setting(slider("Right CPS", 1.0, 20.0, 12.0, 0.5, ""))
				->add_setting(boolean("Break Blocks", true))
				->add_setting(boolean("Right Click", false))
				->add_setting(boolean("Inventory Fill", false))
				->add_setting(mode("Mode", { "Hold", "Toggle", "Dynamic" }, 0));

			add("TargetGuide", "Smoothly guides crosshair towards target players", "Combat")
				->add_setting(slider("Speed", 1.0, 10.0, 4.5, 0.5, ""))
				->add_setting(slider("Range", 2.0, 6.0, 4.2, 0.1, "m"))
				->add_setting(boolean("Click Aim", true))
				->add_setting(mode("Target", { "Players", "Mobs", "All" }, 0));

			add("Reach", "Extends attack reach distance", "Combat")
				->add_setting(slider("Reach Distance", 3.0, 5.0, 3.8, 0.05, "m"))
				->add_setting(boolean("Only While Moving", true));

			add("Velocity", "Reduces horizontal and vertical knockback taken", "Combat")
				->add_setting(slider("Horizontal", 0.0, 100.0, 90.0, 1.0, "%"))
				->add_setting(slider("Vertical", 0.0, 100.0, 100.0, 1.0, "%"))
				->add_setting(slider("Chance", 0.0, 100.0, 100.0, 1.0, "%"));

			add("HitArea", "Expands player hitbox boundaries for easier hits", "Combat")
				->add_setting(slider("Expand Size", 0.1, 1.0, 0.35, 0.05, "m"));
		}

		/* --- movement --- */
		{
			add("Sprint", "Automatically keeps your player sprinting constantly", "Movement")
				->set_enabled(true)
				->add_setting(boolean("Omni-Directional", true));

			add("Fly", "Allows flying across survival environments", "Movement")
				->add_setting(slider("Speed", 0.5, 5.0, 1.8, 0.1, "x"))
				->add_setting(mode("Type", { "Vanilla", "Glide", "Packet" }, 0));

			add("Speed", "Increases movement speed with custom bypass algorithms", "Movement")
				->add_setting(slider("Speed Factor", 1.0, 3.0, 1.4, 0.1, "x"))
				->add_setting(mode("Mode", { "Hop", "Ground", "Strafe" }, 0));

			add("NoSlow", "Prevents item consumption and bow slowdowns", "Movement")
				->add_setting(slider("Forward Multiplier", 0.2, 1.0, 1.0, 0.05, "x"))
				->add_setting(boolean("Bow Slowdown", true));
		}

		/* --- visuals --- */
		{
			add("ESP", "Highlights players and entities through solid walls", "Visuals")
				->set_enabled(true)
				->add_setting(mode("Box Style", { "2D Box", "Corner", "Glow" }, 0))
				->add_setting(boolean("Show Health", true))
				->add_setting(boolean("Show Distance", true));

			add("Tracers", "Draws lines connecting your crosshair to nearby players", "Visuals")
				->add_setting(slider("Line Width", 1.0, 4.0, 1.5, 0.5, "px"))
				->add_setting(boolean("Show Inivisibles", true));

			add("Chams", "Renders glowing colored silhouettes on player models", "Visuals")
				->add_setting(slider("Opacity", 10.0, 100.0, 80.0, 5.0, "%"))
				->add_setting(boolean("Rainbow Mode", false));

			add("Fullbright", "Brightens dark caves and nighttime lighting completely", "Visuals")
				->set_enabled(true)
				->add_setting(slider("Brightness", 1.0, 10.0, 10.0, 1.0, "x"));
		}

		/* --- utilities --- */
		{
			add("QuickPlace", "Removes building block placement delay", "Utilities")
				->add_setting(slider("Delay Ticks", 0.0, 4.0, 0.0, 1.0, ""));

			add("ChestLooter", "Automatically loots items from opened storage chests", "Utilities")
				->add_setting(slider("Loot Delay", 0.0, 200.0, 60.0, 10.0, "ms"))
				->add_setting(boolean("Auto Close", true));

			add("InvSorter", "Drops useless trash items and sorts your inventory", "Utilities")
				->add_setting(slider("Delay", 10.0, 150.0, 50.0, 5.0, "ms"))
				->add_setting(boolean("Open Inv Only", true));
		}

		/* --- misc --- */
		{
			add("AntiAFK", "Prevents server idle kicks with random subtle movements", "Misc")
				->add_setting(slider("Interval", 5.0, 60.0, 20.0, 5.0, "s"));

			add("SelfClean", "Instantly closes and unhooks all client trace footprints", "Misc")
				->set_keybind("DELETE");
		}
	}

	int c_module_manager::count_matching(const std::string& query) const
	{
		const std::string needle = to_lower(trim(query));
		if (needle.empty())
			return 0;

		int matches = 0;

		for (const auto& entry : this->m_modules)
		{
			if (to_lower(entry->name()).find(needle) != std::string::npos
				|| to_lower(entry->description()).find(needle) != std::string::npos)
				++matches;
		}

		return matches;
	}

	void c_module_manager::wipe_category(const std::string& category)
	{
		for (auto& entry : this->m_modules)
		{
			if (!equals_ignore_case(entry->category(), category))
				continue;

			if (entry->enabled())
			{
				entry->set_enabled(false);

				/* the card animating off is not the module turning off — the real
				   Module has to be told, or the engine keeps running it */
				vanta::vgui::bridge::applyToggle(entry->name(), false);
			}

			/* The clear has to reach the real Module too, exactly like the
			   toggle two lines up — otherwise the card reads NONE while the
			   engine still fires the old key. */
			entry->set_keybind_vk(0);
			entry->set_binding(false);

			vanta::vgui::bridge::applyKeybind(entry->name(), 0);
		}
	}

	void c_module_manager::open_sidecar(const std::shared_ptr<c_module>& module_)
	{
		this->m_sidecar = module_;
		this->m_last_open = module_;
		/* Fresh scroll for each opened module — otherwise a scrolled-down
		   drawer keeps that offset when the user opens a shorter module,
		   revealing an empty top strip. */
		this->m_sidecar_scroll = 0.f;
		this->m_target_sidecar_scroll = 0.f;
	}

	void c_module_manager::close_sidecar()
	{
		this->m_sidecar.reset();
		this->m_sidecar_rect_w = 0.f;   // stop routing wheel to the closed drawer
		this->m_sidecar_rect_h = 0.f;
	}

	void c_module_manager::on_category_changed(const std::string& category)
	{
		if (equals_ignore_case(this->m_category, category))
			return;

		this->m_category = category;
		this->m_category_transition = 0.f;
		this->m_target_scroll = 0.f;
		this->m_scroll = 0.f;

		// re-arm the spring so the incoming cards glide up instead of appearing in place
		for (auto& entry : this->m_modules)
		{
			if (equals_ignore_case(entry->category(), category))
				entry->m_visual_y = -1.f;
		}

		if (this->m_sidecar != nullptr && !equals_ignore_case(this->m_sidecar->category(), category))
			this->close_sidecar();
	}

	/* on_key is gone. It broadcast every ImGuiKey press to every card so that
	   whichever one was arming could capture it, and that route was wrong twice
	   over: ImGuiKey is not the Win32 virtual key the engine matches against,
	   and its "named key" range holds the mouse buttons and the scroll wheel.
	   The pill reads the keyboard itself now — c_module::poll_bind_capture. */

	void c_module_manager::on_scroll(float wheel)
	{
		/* If the cursor is over the settings drawer, route the wheel there
		   instead of the grid. The rect is filled by render_sidecar every
		   frame the drawer is open, and stays zero-sized otherwise so the
		   cursor never accidentally matches. */
		if (this->m_sidecar != nullptr && this->m_sidecar_rect_w > 1.f)
		{
			const auto cursor = g_input->get_mouse_position();
			if (cursor.x >= this->m_sidecar_rect_x
				&& cursor.y >= this->m_sidecar_rect_y
				&& cursor.x <  this->m_sidecar_rect_x + this->m_sidecar_rect_w
				&& cursor.y <  this->m_sidecar_rect_y + this->m_sidecar_rect_h)
			{
				this->m_target_sidecar_scroll += wheel * 40.f;
				// Clamp against the measured overflow. Top is 0 (no scroll),
				// bottom is negative up to content-overflow.
				const float overflow = std::max(0.f,
					this->m_sidecar_content_height - this->m_sidecar_body_height);
				if (this->m_target_sidecar_scroll > 0.f)
					this->m_target_sidecar_scroll = 0.f;
				if (this->m_target_sidecar_scroll < -overflow)
					this->m_target_sidecar_scroll = -overflow;
				return;
			}
		}

		/* @a bottom stop, which this never had.

		   The wheel was clamped at the top and left completely open below, so a
		   category with four modules scrolled just as far as one with forty --
		   straight off the end of the list and into the empty card. The sidecar
		   a few lines above has always clamped against its measured overflow;
		   the grid simply never published the two numbers that made that
		   possible. It does now, from its draw.

		   Both extents are zero until the first render, which makes the overflow
		   zero and pins the scroll at the top -- correct, because before a draw
		   there is nothing on screen to move. */
		this->m_target_scroll += wheel * 40.f;

		const float overflow = std::max(0.f,
			this->m_grid_content_height - this->m_grid_body_height);

		if (this->m_target_scroll > 0.f)
			this->m_target_scroll = 0.f;
		if (this->m_target_scroll < -overflow)
			this->m_target_scroll = -overflow;
	}

	namespace
	{
		/* @draw_grip:
			six dots on the card's left edge — the handle a list gets when its order
			is something the user owns. Two columns of three, which is the shape a
			drag handle has had since toolbars had them, and the only thing on the
			card that does not already mean something else.
		*/
		void draw_grip(float centre_x, float centre_y, float lit, float alpha)
		{
			const hue::c_color dot = hue::c_color(92, 92, 104)
				.lerp(theme::text_white, lit)
				.modulate(alpha * (0.55f + 0.45f * lit));

			for (int column = 0; column < 2; ++column)
			{
				for (int row = 0; row < 3; ++row)
				{
					draw::circle(centre_x + static_cast<float>(column) * 4.f,
						centre_y - 4.f + static_cast<float>(row) * 4.f,
						1.35f, dot);
				}
			}
		}
	}

	void c_module_manager::render(const std::string& category, const std::string& search,
		float x, float y, float w, float h, const frame_input_t& input)
	{
		if (!equals_ignore_case(this->m_category, category))
			this->on_category_changed(category);

		this->m_scroll = anim::lerp(this->m_scroll, this->m_target_scroll, 14.f, input.m_delta);
		this->m_category_transition = anim::lerp(this->m_category_transition, 1.f, 8.5f, input.m_delta);

		// the drawer is the animation the user triggers most, so it rides a spring: it arrives
		// carrying momentum and settles, instead of creeping the last few pixels
		this->m_sidecar_anim.update(this->m_sidecar != nullptr ? 1.f : 0.f, 170.f, 21.f, input.m_delta);

		if (this->m_sidecar != nullptr)
			this->m_last_open = this->m_sidecar;

		/* layout only ever sees 0..1, the raw value keeps its overshoot for the panel travel */
		const float travel = this->m_sidecar_anim.m_value;
		const float ease = anim::saturate(travel);

		/* filter by category and search text */
		std::vector<std::shared_ptr<c_module>> filtered{};
		const std::string query = to_lower(trim(search));

		for (auto& entry : this->m_modules)
		{
			if (!this->m_category.empty() && !equals_ignore_case(entry->category(), this->m_category))
				continue;

			if (!query.empty())
			{
				if (to_lower(entry->name()).find(query) == std::string::npos &&
					to_lower(entry->description()).find(query) == std::string::npos)
					continue;
			}

			filtered.push_back(entry);
		}

		/* the grid compresses to the left as the sidecar slides in */
		const float sidecar_width = std::min(340.f, w * 0.44f);
		const float sidecar_gap = 12.f * ease;
		const float main_width = w - (sidecar_width + sidecar_gap) * ease;

		constexpr float padding_x = 16.f;
		constexpr float card_height = 52.f;

		const float card_width = main_width - padding_x * 2.f;

		/* @what the wheel is allowed to reach, measured here and read by
		   on_scroll, which cannot see any of it.

		   n cards carry n-1 gaps, not n: counting a trailing gap would hand the
		   scroll a strip of nothing to stop in. The 8 above the first card is
		   mirrored below the last so the end of the list has the same margin as
		   its start rather than sitting flush on the card's edge. */
		{
			constexpr float k_stride = 52.f + 11.f;
			constexpr float k_inset  = 8.f;

			const std::size_t n = filtered.size();

			this->m_grid_body_height    = h;
			this->m_grid_content_height = n == 0
				? 0.f
				: k_inset + static_cast<float>(n - 1) * k_stride + 52.f + k_inset;
		}

		// the grid scrolls, so nothing it draws is allowed to escape the content card
		draw::push_clip(x, y, w, h);

		if (filtered.empty())
		{
			draw_text("No modules found in " + this->m_category, x + main_width * 0.5f, y + 60.f,
				font_face::regular, 13.5f, theme::text_muted, align_center | align_middle);
		}
		else
		{
			constexpr float stride = card_height + 11.f;
			constexpr float grip_width = 15.f;

			const float list_top = y + 8.f + this->m_scroll;
			const float card_x = x + padding_x;

			/* ---------------- the grip, and the gesture it starts ------------ */
			frame_input_t card_input = input;

			if (!input.m_left_down)
			{
				/* released: whatever order is on screen is the order that stays */
				if (this->m_drag != nullptr)
					this->m_drag.reset();

				this->m_drag_slot = -1;
			}
			else if (this->m_drag == nullptr && input.m_left_clicked)
			{
				for (std::size_t index = 0; index < filtered.size(); ++index)
				{
					const float top = list_top + static_cast<float>(index) * stride;

					if (!input.hovered(card_x, top, grip_width, card_height))
						continue;

					this->m_drag = filtered[index];
					this->m_drag_offset = input.m_cursor.y - top;
					this->m_drag_slot = static_cast<int>(index);

					/* the press belongs to the grip; the card underneath must not
					   also read it as "toggle me" */
					card_input.m_left_clicked = false;
					break;
				}
			}

			/* ---------------- provisional order ------------------------------ */
			std::vector<std::shared_ptr<c_module>> order = filtered;

			if (this->m_drag != nullptr)
			{
				/* Where the carried card would land, from where it is being held.
				   Rounded rather than floored so a card swaps once it is more than
				   halfway over its neighbour, which is where the eye expects the
				   list to give way. */
				const float loose_y = input.m_cursor.y - this->m_drag_offset;
				const int slots = static_cast<int>(order.size()) - 1;

				this->m_drag_slot = std::clamp(
					static_cast<int>(std::lround((loose_y - list_top) / stride)), 0, std::max(0, slots));

				const auto held = std::find(order.begin(), order.end(), this->m_drag);

				if (held != order.end())
				{
					order.erase(held);
					order.insert(order.begin() + this->m_drag_slot, this->m_drag);
				}

				/* the list is being rearranged, not operated */
				card_input.m_left_clicked = false;
				card_input.m_right_clicked = false;

				this->commit_reorder(order);
			}

			/* ---------------- the cards -------------------------------------- */
			for (std::size_t index = 0; index < order.size(); ++index)
			{
				auto& entry = order[index];

				/* Staggered entrance, each card trails the one above it while a
				   category settles. The delay is folded into the transition as
				   (T*(1+k) - k), the same normalisation the settings drawer uses,
				   so that at T == 1 every card reaches progress 1 no matter how
				   long the list is. The old (T - k)/0.65 form never let a card
				   past index 14 cross the 0.35 click gate in render_card (index
				   15 settled at alpha 0.11), which is what made the last card of
				   Visuals (WorldTweaks) impossible to toggle or open. */
				const float k = static_cast<float>(index) * 0.065f;
				const float progress = anim::saturate((this->m_category_transition * (1.f + k) - k) / 0.65f);
				const float alpha = anim::ease_out_cubic(progress);

				const float slot_y = list_top + static_cast<float>(index) * stride;
				const bool carried = (entry == this->m_drag);

				if (carried)
				{
					/* The one card that does NOT settle into its slot: it is pinned
					   to the cursor, so m_visual_y is written rather than eased. Its
					   slot is still reserved above — that gap IS the drop preview,
					   and every other card springs towards it on its own. */
					entry->m_visual_y = input.m_cursor.y - this->m_drag_offset;

					continue;   // drawn last, on top of the list it is moving through
				}

				if (entry->render_card(card_x, slot_y, card_width, card_height,
					this->m_sidecar == entry, card_input, alpha))
				{
					if (this->m_sidecar == entry)
						this->close_sidecar();
					else
						this->open_sidecar(entry);
				}

				/* the grip, on whichever card the cursor is over */
				if (this->m_drag == nullptr
					&& input.hovered(card_x, entry->m_visual_y, card_width, card_height))
				{
					draw_grip(card_x + 6.f, entry->m_visual_y + card_height * 0.5f,
						input.hovered(card_x, entry->m_visual_y, grip_width, card_height) ? 1.f : 0.45f,
						alpha);
				}
			}

			/* the carried card, over everything it is moving through */
			if (this->m_drag != nullptr)
			{
				const float carried_y = input.m_cursor.y - this->m_drag_offset;

				draw::shadow(card_x, carried_y, card_width, card_height, 10.f, 22.f, 0.f, 7.f,
					hue::c_color(0, 0, 0, 190));

				this->m_drag->render_card(card_x, carried_y, card_width, card_height,
					this->m_sidecar == this->m_drag, card_input, 1.f);

				draw_grip(card_x + 6.f, carried_y + card_height * 0.5f, 1.f, 1.f);
			}
		}

		/* ambient depth dim over the grid while the drawer is open */
		if (ease > 0.01f)
		{
			draw::rounded_rect(x + 6.f, y + 4.f, main_width - 6.f, h - 8.f, 10.f,
				hue::c_color(0, 0, 0, static_cast<int>(110.f * ease)));
		}

		if (ease > 0.002f && this->m_last_open != nullptr)
			this->render_sidecar(this->m_last_open, x, y, w, h, sidecar_width, input, ease, travel);

		draw::pop_clip();
	}

	/* @commit_reorder:
		writes a rearranged view back into the real list.

		`order` is only the cards this category and this search are showing, so it
		cannot simply replace m_modules. What it can do is permute the SLOTS those
		cards occupy: collect the indices the filtered set holds in m_modules, then
		deal the new order back into exactly those indices. Everything the filter
		hid keeps the position it had, which is what stops reordering inside one
		category from shuffling every other category as a side effect.
	*/
	void c_module_manager::commit_reorder(const std::vector<std::shared_ptr<c_module>>& order)
	{
		std::vector<std::size_t> slots{};
		slots.reserve(order.size());

		for (std::size_t index = 0; index < this->m_modules.size(); ++index)
		{
			if (std::find(order.begin(), order.end(), this->m_modules[index]) != order.end())
				slots.push_back(index);
		}

		if (slots.size() != order.size())
			return;   // the view and the list disagree; leave the list alone

		for (std::size_t k = 0; k < order.size(); ++k)
			this->m_modules[slots[k]] = order[k];
	}

	void c_module_manager::render_sidecar(const std::shared_ptr<c_module>& module_, float x, float y, float w, float h,
		float sidecar_width, const frame_input_t& input, float ease, float travel)
	{
		const float panel_width = sidecar_width;

		/* the panel rides the raw spring, so it can slide a few pixels past its mark and come back */
		const float slide = (1.f - travel) * (panel_width + 60.f);
		const float panel_x = (x + w - panel_width - 12.f) + slide;
		const float panel_y = y + 8.f;
		const float panel_height = h - 16.f;
		constexpr float radius = 12.f;

		const float alpha = std::min(1.f, ease * 1.2f);

		/* layered ambient depth: a violet halo over a plain black shadow */
		draw::shadow(panel_x, panel_y, panel_width, panel_height, radius, 30.f * ease, 0.f, 2.f,
			theme::accent_violet_deep.alpha(static_cast<int>(35.f * ease)));
		draw::shadow(panel_x, panel_y, panel_width, panel_height, radius, 22.f * ease, 0.f, 4.f,
			hue::c_color(0, 0, 0, static_cast<int>(180.f * ease)));

		draw::rounded_rect(panel_x, panel_y, panel_width, panel_height, radius, theme::bg_card.modulate(alpha));
		draw::border(panel_x, panel_y, panel_width, panel_height, radius, 1.f, theme::border.modulate(alpha));

		/* header row: back button, title, keybind */
		constexpr float header_height = 50.f;
		const float center_y = panel_y + header_height * 0.5f;

		constexpr float back_size = 22.f;
		const float back_x = panel_x + 12.f;
		const float back_y = center_y - back_size * 0.5f;

		const bool back_hovered = input.hovered(back_x, back_y, back_size, back_size);
		this->m_back_hover = anim::lerp(this->m_back_hover, back_hovered ? 1.f : 0.f, 16.f, input.m_delta);

		if (back_hovered && input.m_left_clicked)
			this->close_sidecar();

		if (this->m_back_hover > 0.01f)
		{
			draw::rounded_rect(back_x, back_y, back_size, back_size, 5.f,
				hue::c_color(24, 24, 28, static_cast<int>(255.f * this->m_back_hover * ease)));
		}

		const hue::c_color back_color = this->m_back_hover > 0.1f ? theme::text_white : theme::text_secondary;
		draw::chevron(back_x + back_size * 0.5f, center_y, 9.f, -1, 1.6f, back_color.modulate(ease));

		const float title_x = back_x + back_size + 8.f;
		draw_text(module_->name(), title_x, center_y - 6.f, font_face::bold, 14.f, theme::text_white.modulate(ease), align_left | align_middle);
		draw_text(module_->category(), title_x, center_y + 8.f, font_face::regular, 10.f, theme::text_muted.modulate(ease), align_left | align_middle);

		module_->render_keybind_button(panel_x + panel_width - 14.f, center_y, input, alpha, 28.f, 8.f);

		/* hairline divider then the settings body */
		const float divider_y = panel_y + header_height;
		draw::gradient_line(panel_x + 8.f, divider_y, panel_width - 16.f, 1.f,
			hue::c_color(140, 140, 155, static_cast<int>(60.f * ease)));

		/* Publish the sidecar rect so on_scroll can decide whether a wheel
		   event should scroll the drawer instead of the grid. */
		this->m_sidecar_rect_x = panel_x;
		this->m_sidecar_rect_y = panel_y;
		this->m_sidecar_rect_w = panel_width;
		this->m_sidecar_rect_h = panel_height;

		const float body_top    = divider_y + 14.f;
		const float body_bottom = panel_y + panel_height - 8.f;
		const float body_height = std::max(0.f, body_bottom - body_top);
		const float content_height = module_->settings_height();

		this->m_sidecar_body_height    = body_height;
		this->m_sidecar_content_height = content_height;

		// Re-clamp any leftover target scroll to the new overflow — content
		// height can shrink when a color picker collapses etc.
		const float overflow = std::max(0.f, content_height - body_height);
		if (this->m_target_sidecar_scroll < -overflow)
			this->m_target_sidecar_scroll = -overflow;
		if (this->m_target_sidecar_scroll > 0.f)
			this->m_target_sidecar_scroll = 0.f;
		this->m_sidecar_scroll = anim::lerp(this->m_sidecar_scroll,
			this->m_target_sidecar_scroll, 16.f, input.m_delta);

		/* Clip so long content is bounded to the panel's body, and
		   translate by the scroll offset. */
		draw::push_clip(panel_x + 4.f, body_top - 2.f, panel_width - 8.f, body_height + 4.f);
		module_->render_settings(panel_x + 18.f, body_top + this->m_sidecar_scroll,
			panel_width - 36.f, input, ease);
		draw::pop_clip();
	}
}
