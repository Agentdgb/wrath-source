#include "../../includes.hh"

#include "modules/render/targethud/head_cache.h"

#include "core/profile/PlayerAppearance.h"
#include "../render/player_preview.hh"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vanta
{
	namespace
	{
		constexpr float k_pad      = 16.f;
		constexpr float k_header   = 42.f;
		/* @the search field sits on the CARD. There is no second card.

		   The reference has exactly one container in this region — the player
		   list card itself, whose padding sets the field's left edge and under
		   whose bottom padding the cards begin — and the field lives in that
		   padding, defined by a visible hairline rather than by a lighter fill.

		   What was here instead was a 44px strip with a 32px field inset 6px
		   inside it: two hairlines six px apart, neither of which read as the
		   control. They were not even concentric — an outer radius of 11 inset
		   by 6 wants an inner radius of 5, not 8 — so the gap opened from 6.0 on
		   the straights to 7.2 across the corners and the inner box floated
		   loose in all four of them. The strip is gone; the field takes its rect.

		   44 x r8 is the reference's field at canvas scale (it shows 88 x r14 at
		   2x), 15px of lead matches its 30, and the 16px gap beneath is the
		   card's own padding unit — the same 16 that sets the left edge. */
		constexpr float k_search_h    = 36.f;
		constexpr float k_search_r    = 8.f;
		constexpr float k_search_lead = 15.f;
		constexpr float k_search_gap  = 12.f;

		/* The focus band is BIGGER than the field and is never drawn. It is all
		   that survives of the strip, and it is the half worth keeping: a press
		   on the slack around the field must not drop the caret you were aiming
		   at. Six px still clears the grid below by ten. */
		constexpr float k_search_hit  = 6.f;

		/* @the hairline is what defines this field, not the fill.

		   theme::bg_search is the client's OWN search colour — stock 15,15,15,
		   five levels over the card, which is the reference's "barely
		   distinguishable" — and it is customisable from the profile panel as
		   "Search bar", which is why this is not a literal: the module search in
		   the rail is drawn out of the same value and the two must not drift.

		   The border is brighter than the card's own 30,30,34 so the field is
		   unambiguously the foreground, but well short of the 56 the first draft
		   wanted, which would have been the brightest outline in the file by a
		   wide margin. */
		constexpr hue::c_color k_search_edge{ 46, 46, 52, 255 };

		/* @the grid is sized in CARDS, and the detail column gets the remainder.

		   The old split was the other way round — a fixed 200px detail column and whatever fell
		   out for the grid — and what fell out was 312px, which holds three 78px cards and 62px
		   of nothing. A fifth of the grid was empty gutter. Deriving the split from the card
		   pitch instead makes the leftover impossible: the grid is exactly n cards wide and the
		   62px goes to the column that can use it.

		   The card is 112 x 92 because the NAME has to fit. At 78 wide the usable text budget
		   was 62px and "@xXShadowSlayerXx" is 93px, so it overhung both edges and the clip rect
		   ate the overhang; and the name sat at y+68 with the head running to y+70, so four
		   fifths of its ink was printed over a Minecraft face. Both of those are the reported
		   "no se ve nada".  */
		constexpr float k_card_w      = 112.f;
		constexpr float k_card_h      = 92.f;
		constexpr float k_card_gap    = 12.f;
		constexpr float k_detail_min  = 164.f;
		constexpr float k_rule_gap    = 16.f;   /* grid | gap | rule | gap | detail */


		std::string lowered(const std::string& s)
		{
			std::string out;
			out.reserve(s.size());
			for (const char c : s)
				out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			return out;
		}

		/* @Neutral, or Friend in green.

		   The word is the status, not a second thing beside it — a card that says both "Neutral"
		   and carries a friend tick is saying the same fact twice and can contradict itself. */
		hue::c_color status_colour(bool is_friend)
		{
			return is_friend ? hue::c_color{ 90, 210, 130, 255 }
			                 : hue::c_color{ 130, 130, 140, 255 };
		}

		/* @health, coloured by what fraction of it is left.

		   Three stops, not a hue sweep: a straight rotation through HSV would run green ->
		   yellow -> red by way of a lime that reads as "fine" at 60% and an orange that reads
		   as "fine" again at 40%. Anchoring the middle at a real amber and mixing towards each
		   end keeps the change monotonic, so the colour alone answers the question the panel is
		   open to answer.

		   The ratio, not the absolute. A server that hands out forty max health would otherwise
		   paint a player at twenty — dead level in vanilla terms, half a bar in that world — in
		   the same red as one about to fall over. */
		hue::c_color health_colour(float fraction)
		{
			const float f = std::clamp(fraction, 0.f, 1.f);

			const hue::c_color low { 236,  84,  84, 255 };
			const hue::c_color mid { 240, 190,  74, 255 };
			const hue::c_color high{  92, 214, 128, 255 };

			return f >= 0.5f ? mid.lerp(high, (f - 0.5f) * 2.f)
			                 : low.lerp(mid, f * 2.f);
		}

		/* Half hearts exist and 20.0 does not read as twenty, so the decimal is printed only
		   when there is one. Rounded before the test, or 19.999996 prints as "20" from one
		   branch and "20.0" from the other depending on the float. */
		std::string hp_text(float value)
		{
			const float v = std::round(std::max(0.f, value) * 10.f) / 10.f;

			char buffer[24];
			if (std::fabs(v - std::round(v)) < 0.05f)
				std::snprintf(buffer, sizeof(buffer), "%d HP", static_cast<int>(std::round(v)));
			else
				std::snprintf(buffer, sizeof(buffer), "%.1f HP", v);

			return buffer;
		}

		/* @every surface here is a PANE, not a colour.

		   With glass on, the panel is backed by a real blur of the world and each
		   surface above it has to be sheer enough to let that through — otherwise
		   the blur is queued, composited, and then painted over by an opaque card,
		   which is exactly as expensive and completely invisible. The sheer values
		   match the client's own: 0.34 for the backdrop, 0.42 for a card, 0.5 for
		   a control. With glass off theme::pane hands the solid colour straight
		   back, so there is one code path either way. */
		hue::c_color pane(const hue::c_color& solid, float sheer)
		{
			return theme::pane(solid, sheer);
		}
	}

	c_player_list::card_state_t& c_player_list::state_for(const std::string& name)
	{
		return this->m_cards[name];
	}

	bool c_player_list::hovered(const frame_input_t& input) const
	{
		return this->m_rect[2] > 0.f
			&& input.hovered(this->m_rect[0], this->m_rect[1], this->m_rect[2], this->m_rect[3]);
	}

	void c_player_list::on_scroll(float wheel)
	{
		if(m_detail_hovered && m_preview_tab==1){m_name_history_scroll=std::max(0,m_name_history_scroll-static_cast<int>(wheel*3));return;}
        this->m_target_scroll -= wheel * 48.f;
	}

	void c_player_list::on_character(char character)
	{
		if (!this->m_search_focused) return;
		if (this->m_search.size() < 32u && character >= 32 && character < 127)
			this->m_search.push_back(character);
	}

	void c_player_list::on_backspace()
	{
		if (!this->m_search_focused) return;
		if (!this->m_search.empty()) this->m_search.pop_back();
	}

	/* ------------------------------------------------------------------------ */

	void c_player_list::render_search(float x, float y, float w, float h, const frame_input_t& input)
	{
		/* Focus is decided by the caller, against a band a few px larger than the
		   field rather than against the field itself. */
		const float glow = this->m_search_glow.update(this->m_search_focused ? 1.f : 0.f,
			420.f, 34.f, input.m_delta);

		/* @the fill goes through pane(), the hairline never does.

		   With glass on the fill has to let the card's blur through, so it is a
		   pane at the same 0.5 every other control here takes. The border is
		   drawn opaque in BOTH modes on purpose: what defines a control must not
		   depend on which mode the client is in, and a hairline is one pixel of
		   ink either way.

		   lerp rather than the channel arithmetic this used to do by hand. The
		   spring is 420/34 and critical damping there is 2*sqrt(420) = 41, so it
		   overshoots slightly; c_color::lerp clamps its step, the raw arithmetic
		   did not. */
		draw::rounded_rect(x, y, w, h, k_search_r, pane(theme::bg_search, 0.5f));
		draw::border(x, y, w, h, k_search_r, 1.f,
			k_search_edge.lerp(theme::accent_violet, glow));

		/* 13px, not 12: the field is 44 tall now and 12 read as a caption lost in
		   the middle of it. 13 is one of the sizes text.cc bakes, so it is a real
		   atlas entry rather than a silent fall back to the nearest one.

		   Clipped to the field, because nothing else would clip it. The query is
		   capped at 32 characters, which at 13px can run past the 330px of inner
		   width — and the grid's clip is not pushed until after this, while the
		   shell's outer clip is wider than the card, so an overlong query used to
		   paint straight across the divider and into the detail column. */
		constexpr float k_search_text = 13.f;

		draw::push_clip(x + 2.f, y, w - 4.f, h);

		const bool empty = this->m_search.empty();
		draw_text(empty ? "Search player..." : this->m_search,
			x + k_search_lead, y + h * 0.5f, font_face::regular, k_search_text,
			empty ? theme::text_muted : theme::text_primary, align_left | align_middle);

		/* A caret only while focused, and only on the beat — a static bar reads as a glyph. */
		if (this->m_search_focused)
		{
			const float caret = text_width(this->m_search, font_face::regular, k_search_text);
			const float beat  = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.f);
			draw::rounded_rect(x + k_search_lead + caret + 1.f, y + h * 0.5f - 7.5f, 1.f, 15.f, 0.5f,
				hue::c_color{ 255, 255, 255, static_cast<int>(200.f * beat) });
		}

		draw::pop_clip();
	}

	bool c_player_list::render_card(const roster_entry_t& person, float x, float y, float w, float h,
		bool selected, const frame_input_t& input)
	{
		const bool is_friend = g_social->is_friend(person.m_name);
		card_state_t& state = this->state_for(person.m_name);

		const bool hot = input.hovered(x, y, w, h);
		state.m_hover = anim::lerp(state.m_hover, hot ? 1.f : 0.f, 16.f, input.m_delta);

		if (!state.m_ready) { state.m_appear.snap(1.f); state.m_ready = true; }

		const int lift = static_cast<int>(7.f * state.m_hover);
		draw::rounded_rect(x, y, w, h, 10.f,
			pane(hue::c_color{ 19 + lift, 19 + lift, 22 + lift, 255 }, 0.42f));

		/* @the selected card is outlined in the client's own accent.

		   1px, not 1.6. draw::border strokes about the rect's edge rather than inside it
		   (draw.cc: inset = thickness * 0.5), so at 1.6 the ink lands from -1.3 to +0.5 of the
		   edge and half of it is on the wrong side of a fractional panel scale — which is the
		   blur that got reported. At 1.0 the AA path is thin enough to stay on one pixel. */
		if (selected)
			draw::border(x, y, w, h, 10.f, 1.f, theme::accent_violet);

		/* @the card is a badge, a face, a name and a reading, in that order.

		   The status is at the TOP RIGHT and the health under the name, which is what was
		   asked for and also what the card wants: the two are different kinds of fact. The
		   status is a label — something you decided about this person, true until you change
		   it — so it belongs in a corner, out of the reading order. The health is live data
		   about them, so it sits with the name, under it, where you read down to it.

		   The whole column has to close inside 92px, and it does, exactly:

		       4 .. 18   status pill      (14 tall, right aligned, 6 in from the edge)
		      21 .. 61   head             (40 square, centred)
		      65 .. 75   @name            (10px, centred on 70)
		      79 .. 89   health           (10px, centred on 84)

		   with 3-4px between every pair and 3 left at the foot. The head came down from 44 to
		   40 to pay for the fourth line. It could not stay at 44: the pill's band and the head
		   would have had to overlap, and a badge printed over the top right of somebody's skin
		   is exactly what this card looked like two revisions ago.

		   The pill sits ABOVE the head's band rather than beside it because there is no beside:
		   a centred 40px face leaves 36px either side and "Neutral" alone is thirty of them. */
		const char* word    = person.m_self ? "Client" : is_friend ? "Friend" : "Neutral";
		const float word_w  = text_width(word, font_face::regular, 9.f);
		const float pill_w  = word_w + 12.f;
		const float pill_h  = 14.f;
		const float pill_x  = x + w - 6.f - pill_w;
		const float pill_y  = y + 4.f;

		/* @your own card says "Client", in the accent.

		   "Neutral" on yourself is a category error, and the grid gives no other way to tell
		   which face is you. The pill is tinted from the accent so it follows a restyled theme
		   rather than a hardcoded violet. */
		const hue::c_color pill_bg = person.m_self
			? hue::c_color{ theme::accent_violet.r / 5 + 14, theme::accent_violet.g / 5 + 14,
			                theme::accent_violet.b / 5 + 14, 255 }
			: is_friend ? hue::c_color{ 26, 44, 33, 255 } : hue::c_color{ 28, 28, 32, 255 };
		const hue::c_color pill_fg = person.m_self ? theme::accent_violet : status_colour(is_friend);

		draw::rounded_rect(pill_x, pill_y, pill_w, pill_h, pill_h * 0.5f, pane(pill_bg, 0.62f));
		if (person.m_self)
			draw::border(pill_x, pill_y, pill_w, pill_h, pill_h * 0.5f, 1.f,
				theme::accent_violet.alpha(110));
		draw_text(word, pill_x + pill_w * 0.5f, pill_y + pill_h * 0.5f, font_face::regular, 9.f,
			pill_fg, align_center | align_middle);

		/* @the head. square, centred, and it shares pixels with nothing. */
		constexpr float k_face = 40.f;
		const float face_x = x + (w - k_face) * 0.5f;
		const ImTextureID head = modules::targethud::heads::texture_for(person.m_name);
		if (head)
			draw::texture(head, face_x, y + 21.f, k_face, k_face, 7.f, theme::text_primary);
		else
			draw::rounded_rect(face_x, y + 21.f, k_face, k_face, 7.f,
				pane(hue::c_color{ 32, 32, 36, 255 }, 0.55f));

		/* @ONE name, not two.

		   The reference is Roblox, where a display name and an @handle are different strings.
		   Minecraft has one name, so printing it bold and again with an @ under it was the same
		   word twice. The @ form is the one that was asked for.

		   Ellipsised to the card's own width so a long name can neither overhang into its
		   neighbour nor be cut dead by the grid's clip rect. */
		const std::string handle = truncate_text("@" + person.m_name, w - 14.f,
			font_face::regular, 10.f);
		draw_text(handle, x + w * 0.5f, y + 70.f, font_face::regular, 10.f,
			selected ? theme::accent_violet : theme::text_secondary,
			align_center | align_middle);

		/* @the health, on its own line under the name.

		   Nothing at all when it is unknown, rather than a placeholder: on a full server most
		   of the tab list is outside render distance, and a column of dashes says nothing that
		   the empty line does not already say. The name stays put either way — the line is at
		   a fixed y, not stacked under a variable-height block — so cards with and without a
		   reading still line up across a row. */
		if (person.m_max_health > 0.f)
			draw_text(hp_text(person.m_health), x + w * 0.5f, y + 84.f, font_face::bold, 10.f,
				health_colour(person.m_health / person.m_max_health),
				align_center | align_middle);

		return hot && input.m_left_clicked;
	}

	void c_player_list::render_detail(float x, float y, float w, float h, const frame_input_t& input)
	{
        const roster_entry_t* person = nullptr;
        for (const auto& entry : g_roster->entries()) if (entry.m_name == m_selected) { person = &entry; break; }
        if (!person) {
            m_detail_hovered = false;
            appearance::select({});
            draw_text("Select a player", x+w*.5f,y+h*.5f,font_face::regular,11,theme::text_muted,align_center|align_middle);
            return;
        }
        if (m_detail_for != person->m_name) {
            m_detail_for = person->m_name; m_name_choice = 0;
            m_preview_drag = false; m_copy_until = 0; m_history_cape.clear();m_cape_index=0;m_name_history_scroll=0;m_history_copy.clear();
        }
        m_detail_hovered=input.hovered(x,y,w,h);
        appearance::select(person->m_name);
        const auto look = appearance::get(person->m_name,person->m_uuid,person->m_textures,person->m_details_ready,m_history_cape);
        const float cx=x+w*.5f, name_y=y+h-10;
        const float preview_bottom=name_y-14, options_y=preview_bottom-24;
        auto button = [&](const char* label,float bx,float by,float bw,bool selected,bool enabled=true) {
            const bool hot=enabled && input.hovered(bx,by,bw,24);
            draw::rounded_rect(bx,by,bw,24,6,selected?theme::accent_violet_deep:theme::pane(theme::bg_element,.65f));
            if (hot || selected) draw::border(bx,by,bw,24,6,1,theme::accent_violet.alpha(selected?180:100));
            draw_text(label,bx+bw*.5f,by+12,font_face::regular,10,enabled?theme::text_primary:theme::text_muted,align_center|align_middle);
            return hot && input.m_left_clicked;
        };
        const char* tabs[] = {"Capes","Nicks"};
        const float tab_w=(w-4)/2;
        for(int tab=0;tab<2;++tab) if(button(tabs[tab],x+tab*(tab_w+4),y+3,tab_w,m_preview_tab==tab)) {
            m_preview_tab=tab; m_preview_drag=false;
        }
        if (m_preview_tab == 0) {
            const float py=y+36, ph=std::max(30.f,options_y-py-7);
            draw::rounded_rect(x,py,w,ph,9,theme::pane(theme::bg_element,.4f));
            if(!look.capes.empty() && m_history_cape.empty()) {
                m_cape_index=std::clamp(m_cape_index,0,static_cast<int>(look.capes.size())-1);
                m_history_cape=look.capes[m_cape_index].id;
            }
            if(!m_history_cape.empty() && m_history_cape!="none") {
                const float side=std::max(16.f,std::min(w,ph-27));
                if(look.historicalCapeTexture)draw::texture(static_cast<ImTextureID>(look.historicalCapeTexture),cx-side*.5f,py+(ph-side)*.5f-6,side,side,0,hue::c_color(255,255,255));
                else draw_text(look.historicalCapeLoading?"Loading cape...":"Cape preview unavailable",cx,py+ph*.5f,font_face::regular,10,theme::text_muted,align_center|align_middle);
                if(!look.capes.empty()) {
                    const auto& cape=look.capes[std::clamp(m_cape_index,0,static_cast<int>(look.capes.size())-1)];
                    const std::string caption=(cape.active?"Active / ":"Seen / ")+cape.lastSeen.substr(0,10);
                    draw_text(caption,cx,py+ph-9,font_face::regular,9,theme::text_muted,align_center|align_middle);
                }
            } else if (look.skin) {
                draw_player_preview(g_render->draw_list(),look,x,py,w,ph-18,m_preview_yaw,true,true);
                if (input.m_left_clicked && input.hovered(x,py,w,ph)) { m_preview_drag=true; m_preview_last_x=input.m_cursor.x; }
                if (!input.m_left_down) m_preview_drag=false;
                if (m_preview_drag) {
                    m_preview_yaw+=(input.m_cursor.x-m_preview_last_x)*.018f;
                    m_preview_last_x=input.m_cursor.x;
                }
                const char* hint=look.hasCape?(look.cape?"Equipped cape - drag to rotate":"Cape download unavailable"):"No cape equipped";
                draw_text(truncate_text(hint,w-8,font_face::regular,9),cx,py+ph-7,font_face::regular,9,theme::text_muted,align_center|align_middle);
            } else {
                const char* message=look.state==appearance::State::loading?"Loading preview...":"Preview unavailable";
                draw_text(message,cx,py+ph*.5f,font_face::regular,11,theme::text_muted,align_center|align_middle);
            }
            {
                const int total=static_cast<int>(look.capes.size());
                const float arrow=28.f;
                const bool previous=button("<",x,options_y,arrow,false,total>0);
                const bool next=button(">",x+w-arrow,options_y,arrow,false,total>0);
                if(previous||next){m_cape_index=(m_cape_index+(next?1:total-1))%total;m_history_cape=look.capes[m_cape_index].id;}
                const std::string count=total?std::to_string(m_cape_index+1)+" / "+std::to_string(total)+" capes":"No public capes";
                draw_text(count,cx,options_y+12,font_face::regular,10,theme::text_primary,align_center|align_middle);
                if(!total && look.state!=appearance::State::loading)
                    draw_text(look.capeHistoryAvailable?"No recorded cape history":"Public cape history unavailable",cx,y+47,font_face::regular,9,theme::text_muted,align_center|align_middle);
            }
        } else {
            const char* labels[]={"Nick: ","Tab: "};
            const std::string values[]={person->m_name,person->m_display_name};
            for(int row=0;row<2;++row) {
                const float ry=y+39+row*21;
                draw_text(truncate_text(std::string(labels[row])+values[row],w-10,font_face::regular,10),x+5,ry+7,font_face::regular,10,theme::text_primary,align_left|align_middle);
                if(input.m_left_clicked&&input.hovered(x,ry-2,w,20)){m_name_choice=row;m_history_copy.clear();}
            }
            const float history_y=y+90;
            draw_text("Previous names / Laby",x+5,history_y-7,font_face::regular,9,theme::text_muted,align_left|align_middle);
            const int rows=std::max(1,static_cast<int>((options_y-history_y-4)/18));
            const int total=static_cast<int>(look.names.size());
            m_name_history_scroll=std::clamp(m_name_history_scroll,0,std::max(0,total-rows));
            for(int row=0;row<rows&&m_name_history_scroll+row<total;++row) {
                const auto& record=look.names[m_name_history_scroll+row];
                const float ry=history_y+row*18;
                if(m_history_copy==record.name)draw::rounded_rect(x,ry-2,w,17,4,theme::accent_violet_deep);
                draw_text(record.name,x+5,ry+6,font_face::regular,10,theme::text_primary,align_left|align_middle);
                if(w>240 && !record.changedAt.empty())draw_text(record.changedAt.substr(0,10),x+w-5,ry+6,font_face::regular,9,theme::text_muted,align_right|align_middle);
                if(input.m_left_clicked&&input.hovered(x,ry-2,w,17))m_history_copy=record.name;
            }
            if(total==0)draw_text(look.state==appearance::State::loading?"Loading public history...":look.nameHistoryAvailable?"No older names recorded":"Name history unavailable",x+5,history_y+8,font_face::regular,9,theme::text_muted,align_left|align_middle);
            const bool previous=button("<",x,options_y,24,false,m_name_history_scroll>0);
            const bool next=button(">",x+w-24,options_y,24,false,m_name_history_scroll+rows<total);
            if(previous)m_name_history_scroll=std::max(0,m_name_history_scroll-rows);
            if(next)m_name_history_scroll=std::min(std::max(0,total-rows),m_name_history_scroll+rows);
            const std::string copied=m_history_copy.empty()?values[m_name_choice]:m_history_copy;
            if(button(ImGui::GetTime()<m_copy_until?"Copied":"Copy",x+28,options_y,w-56,false,!copied.empty())) {
                ImGui::SetClipboardText(copied.c_str());m_copy_until=ImGui::GetTime()+1.5;
            }
        }
        draw_text(truncate_text(person->m_name,w-4,font_face::bold,13),cx,name_y,font_face::bold,13,theme::text_primary,align_center|align_middle);

	}

	/* ------------------------------------------------------------------------ */

	void c_player_list::render(float x, float y, float w, float h, const frame_input_t& input)
	{
		this->m_rect[0] = x; this->m_rect[1] = y;
		this->m_rect[2] = w; this->m_rect[3] = h;

		/* @the handle, published for the shell — see drag_rect().

		   The full width of the header row and nothing below it. k_header is the
		   same constant bar_y is derived from a few lines down, so the band and
		   the search strip meet exactly and can never overlap by a pixel however
		   either is retuned. Local, so it does not go stale mid-drag. */
		/* @the handle starts AFTER the range button.

		   The whole header strip is this card's title bar, and the shell
		   hit-tests it before anything the view draws -- so a handle spanning
		   the full width would take the button's press and drag the card
		   instead of toggling. Publishing the free space and letting the shell
		   have that is the same contract the sidebar's brand band uses. */
		const float handle_l = (k_pad + 18.f + 6.f * 2.f) - 4.f;

		this->m_drag[0] = handle_l;  this->m_drag[1] = 0.f;
		this->m_drag[2] = w - handle_l; this->m_drag[3] = k_header;

		draw::shadow(x, y, w, h, 16.f, 18.f, 0.f, 6.f, hue::c_color{ 0, 0, 0, 150 });
		/* The same sheer the panel's own backdrop uses, so with glass on the two
		   surfaces read as one pane. The blur beneath it is queued by the shell,
		   which is the only place that knows this surface is about to be drawn. */
		draw::rounded_rect(x, y, w, h, 16.f, pane(theme::bg_window, 0.34f));
		draw::border(x, y, w, h, 16.f, 1.f, hue::c_color{ 30, 30, 34, 255 });
        auto* grip=g_render->draw_list();
        grip->AddLine({x+w-17,y+h-7},{x+w-7,y+h-17},theme::text_muted.transform(),1.5f);
        grip->AddLine({x+w-12,y+h-7},{x+w-7,y+h-12},theme::text_muted.transform(),1.5f);

		/* @the split is derived from the card pitch, so it closes exactly.

		   budget      what is left for the grid after the padding, the divider and the detail
		               column's minimum
		   columns     how many whole cards that buys
		   grid_w      exactly that many cards and the gaps between them — never a leftover
		   detail_w    the remainder, which is >= k_detail_min by construction */
		const float inner   = w - k_pad * 2.f;
		const float gutter  = k_rule_gap * 2.f + 1.f;
		const float detail_min = std::max(k_detail_min,std::min(380.f,k_detail_min+(w-640.f)*.35f));
		const float budget  = inner - gutter - detail_min - 48.f;

		const int columns = std::max(1,
			static_cast<int>((budget + k_card_gap) / (k_card_w + k_card_gap)));

		const float grid_x   = x + k_pad + 12.f;
		const float grid_w   = static_cast<float>(columns) * k_card_w
		                     + static_cast<float>(columns - 1) * k_card_gap;
		const float rule_x   = grid_x + grid_w + k_rule_gap;
		const float detail_x = rule_x + 1.f + k_rule_gap;
		const float detail_w = x + w - k_pad - 12.f - detail_x;

        // Two quiet interior panes; player cards keep their head/name/status layout.
        const float pane_y=y+k_header;
        const float pane_h=h-k_header-k_pad;
        auto interior=[&](float px,float pw) {
            draw::rounded_rect(px,pane_y,pw,pane_h,10.f,theme::pane(hue::c_color(16,17,19,255),.80f));
            draw::border(px,pane_y,pw,pane_h,10.f,1.f,hue::c_color(95,96,108,65));
        };
        interior(grid_x-12.f,grid_w+24.f);
        interior(detail_x-12.f,detail_w+24.f);
        draw::gradient_line(x+k_pad,y+k_header-8.f,w-k_pad*2.f,1.f,hue::c_color(120,120,135,28));

		/* @the range filter, top left, before the title.

		   In range means the tab-list name has an ENTITY in the world.
		   The roster marks it on its own 4 Hz walk.

		   The rect is grown by six on every side for the hit test and drawn at
		   its true size, the same slack the search field takes: an eighteen px
		   glyph is a small thing to have to hit exactly. */
		constexpr float k_ico     = 18.f;
		constexpr float k_ico_pad = 6.f;

		const float ico_x = grid_x;
		const float ico_y = y + 21.f - k_ico * 0.5f;

		const bool ico_hot = input.hovered(ico_x - k_ico_pad, ico_y - k_ico_pad,
			k_ico + k_ico_pad * 2.f, k_ico + k_ico_pad * 2.f);

		if (ico_hot && input.m_left_clicked)
			this->m_only_nearby = !this->m_only_nearby;

		this->m_range_hover = anim::lerp(this->m_range_hover, ico_hot ? 1.f : 0.f,
			16.f, input.m_delta);
		this->m_range_glow  = anim::lerp(this->m_range_glow,
			this->m_only_nearby ? 1.f : 0.f, 15.f, input.m_delta);

		/* filter first, so the count in the header is what is actually on screen */
		const std::string needle = lowered(this->m_search);
		std::vector<const roster_entry_t*> shown;
		for (const roster_entry_t& p : g_roster->entries())
		{
			if (this->m_only_nearby && !p.m_loaded) continue;
			if (!needle.empty() && lowered(p.m_name).find(needle) == std::string::npos) continue;
			shown.push_back(&p);
		}

		{
			const float gl = this->m_range_glow;

			draw::rounded_rect(ico_x - k_ico_pad, ico_y - k_ico_pad,
				k_ico + k_ico_pad * 2.f, k_ico + k_ico_pad * 2.f, 8.f,
				pane(hue::c_color{
					static_cast<int>(22 + 14 * this->m_range_hover + 12 * gl),
					static_cast<int>(22 + 14 * this->m_range_hover + 8  * gl),
					static_cast<int>(25 + 14 * this->m_range_hover + 20 * gl), 255 }, 0.5f));

			if (gl > 0.01f)
				draw::border(ico_x - k_ico_pad, ico_y - k_ico_pad,
					k_ico + k_ico_pad * 2.f, k_ico + k_ico_pad * 2.f, 8.f, 1.f,
					theme::accent_violet.alpha(static_cast<int>(200.f * gl)));

			if (const ImTextureID ico = g_icons->get("user_range", static_cast<int>(k_ico)))
				draw::texture(ico, ico_x, ico_y, k_ico, k_ico, 0.f,
					theme::text_muted.lerp(theme::accent_violet, gl));
		}

		/* header: the title after the button, the count beside it in grey */
		const float title_x = ico_x + k_ico + k_ico_pad + 10.f;

		draw_text("Player List", title_x, y + 21.f, font_face::bold, 14.f,
			theme::text_primary, align_left | align_middle);
		draw_text(std::to_string(shown.size()),
			title_x + text_width("Player List", font_face::bold, 14.f) + 8.f,
			y + 21.f, font_face::regular, 12.f, theme::text_muted, align_left | align_middle);

		/* @the field spans the grid column, and nothing is drawn behind it.

		   Spanning grid_w keeps it reading as the grid's control rather than the
		   card's, and — as in the reference — it stops short of the full width:
		   the divider and the detail column are not its business.

		   The focus target is the field grown by k_search_hit on every side, and
		   is never painted. It clears list_y by ten px (the 16 gap less the 6 of
		   slack), so the band and the grid can never both answer one press. */
		const float bar_y = y + k_header + 12.f;
		if (input.m_left_clicked)
			this->m_search_focused = input.hovered(
				grid_x - k_search_hit, bar_y - k_search_hit,
				grid_w + k_search_hit * 2.f, k_search_h + k_search_hit * 2.f);

		this->render_search(grid_x, bar_y, grid_w, k_search_h, input);

		/* 418 - 42 header - 44 field - 16 gap - 16 pad = 300 = 3 * 92 + 2 * 12,
		   which is exactly three rows of cards and no fourth one half cut.

		   The field is the same 44 the strip was, so the only thing that grew is
		   the gap under it, 10 -> 16, which puts the card's own padding unit
		   below the field as well as to its left. k_players_height paid the six. */
		const float list_y = bar_y + k_search_h + k_search_gap;
		const float list_h = h - (list_y - y) - k_pad - 12.f;

		const int   rows      = (static_cast<int>(shown.size()) + columns - 1) / columns;
		/* n rows carry n-1 gaps, not n. Counting a trailing gap that is never drawn added a
		   phantom 12px of content and let the grid scroll to a strip of empty background. */
		const float content_h = rows > 0
			? static_cast<float>(rows) * k_card_h + static_cast<float>(rows - 1) * k_card_gap
			: 0.f;
		const float max_scroll = std::max(0.f, content_h - list_h);

		this->m_target_scroll = std::clamp(this->m_target_scroll, 0.f, max_scroll);
		this->m_scroll = anim::lerp(this->m_scroll, this->m_target_scroll, 18.f, input.m_delta);
		/* Snapped, so a card's edges land on pixel boundaries mid-scroll and the 1px selection
		   ring does not smear across two rows of them. */
		const float scroll = std::round(this->m_scroll);

		/* @the grid is clipped VERTICALLY, and padded horizontally.

		   Rows scrolled past the top must never draw over the search field — that is the whole
		   job. But draw::border and draw::shadow both paint OUTSIDE the rect they are given, so
		   a clip flush with the tiled grid shaves the selection ring off the first and last
		   columns; the reported blur was half a ring being scissored, not a soft one. Eight px
		   of horizontal slack costs nothing (the divider is sixteen away) and keeps every card's
		   outline whole.

		   On g_render's list, not the window's — that is where every draw:: and every draw_text
		   in this file lands, and a window-level push would not touch it. */
		/* Two px of vertical slack as well: the ring on the top row was being
		   cut flat along list_y for the same reason. The search field ends 16px
		   above, so nothing scrolled-out can reach it through the slack. */
		ImDrawList* dl = g_render->draw_list();
		dl->PushClipRect(ImVec2(grid_x - 8.f, list_y - 2.f),
			ImVec2(grid_x + grid_w + 8.f, list_y + list_h + 2.f), true);

		/* @the clip hides the overhang. It does not un-click it.

		   PushClipRect scissors PIXELS; the hit tests below run on the cursor and
		   know nothing about it. A row scrolled half out of view is still live
		   over its whole height, so the part of it hidden behind the search strip
		   answered the same click that focused the field — and the detail column
		   jumped to somebody who was not on screen. Outside the viewport the grid
		   simply gets no cursor. */
		frame_input_t grid_input = input;
		if (!input.hovered(grid_x, list_y, grid_w, list_h))
		{
			grid_input.m_cursor = math::c_vector_2d(-10000.f, -10000.f);
			grid_input.m_left_down = false;
			grid_input.m_left_clicked = false;
			grid_input.m_right_clicked = false;
		}

		/* @cards ease into their slots, they do not jump to them.

		   Toggling the filter re-slots every card at once. Snapping them there
		   reads as the list being swapped out; easing, the survivors visibly
		   slide up into the gaps the rest left, which is what makes it obvious
		   that this is the SAME list with fewer people in it.

		   Animated in LOCAL slot coordinates -- offsets from the grid's origin,
		   not absolute canvas positions. The card can be dragged around the
		   screen now, and an absolute target would make every card chase the
		   window on every frame of a drag. The scroll is likewise subtracted at
		   draw time rather than animated, so the wheel stays exactly 1:1. */
		for (std::size_t i = 0; i < shown.size(); ++i)
		{
			const int col  = static_cast<int>(i) % columns;
			const int rowi = static_cast<int>(i) / columns;

			const float tx = static_cast<float>(col)  * (k_card_w + k_card_gap);
			const float ty = static_cast<float>(rowi) * (k_card_h + k_card_gap);

			card_state_t& st = this->state_for(shown[i]->m_name);

			if (!st.m_placed) { st.m_x = tx; st.m_y = ty; st.m_placed = true; }
			else {
				st.m_x = anim::lerp(st.m_x, tx, 17.f, input.m_delta);
				st.m_y = anim::lerp(st.m_y, ty, 17.f, input.m_delta);
			}

			const float cx = grid_x + st.m_x;
			const float cy = list_y + st.m_y - scroll;

			/* Culled on where it is DRAWN, and after the easing above, so a card
			   sliding in from off screen is already in the right place when it
			   crosses the edge. */
			if (cy + k_card_h < list_y - 4.f || cy > list_y + list_h + 4.f)
				continue;

			if (this->render_card(*shown[i], cx, cy, k_card_w, k_card_h,
				shown[i]->m_name == this->m_selected, grid_input))
				this->m_selected = shown[i]->m_name;
		}

		dl->PopClipRect();

		/* An empty grid means one of two very different things, and only this says which. */
		if (shown.empty())
			draw_text(this->m_only_nearby ? "Nobody in range"
				: g_roster->attempted() && !g_roster->resolved()
					? "Player list unavailable"
					: (this->m_search.empty() ? "No players" : "No match"),
				grid_x + grid_w * 0.5f, list_y + 40.f, font_face::regular, 11.f,
				theme::text_muted, align_center | align_middle);

		/* the rule that replaces the detail column's container */
        this->render_detail(detail_x, y + k_header + 12.f, detail_w, h - k_header - k_pad - 24.f, input);

	}
}
