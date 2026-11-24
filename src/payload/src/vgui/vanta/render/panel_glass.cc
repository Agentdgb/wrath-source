#include "../../includes.hh"

#include "panel_glass.hh"
#include "../../../modules/render/arraylist/al_glass.h"

namespace vanta::panel_glass
{
	namespace
	{
		/* Its own pipeline, not the ArrayList's. Sharing one would mean two
		   callers driving the same FBOs at different points in the same frame,
		   and the second would capture a scene the first had already changed. */
		vanta::al::al_glass g_glass{}, g_popup_glass{};
        vanta::al::al_fbo g_game{};
        bool g_game_ready=false;

		/* @the queue.

		   A callback is handed one void* and fires later, so what it needs has to
		   outlive the call that queued it. A small ring of slots rather than a
		   single struct: nothing stops two panels asking for glass in one frame,
		   and a single struct would have the second overwrite the first before
		   either had run. */
		/* Four is the most any one composite has ever wanted, and a fixed array
		   keeps the request POD so the ring below stays a plain assignment. */
		constexpr int k_max_panes = 4;

		struct request_t
		{
			pane_t panes[k_max_panes]{};
			int   count{ 0 };
			float strength{};
			float brightness{};
			float darken{};
			int   passes{ 6 };
            bool game{false};
		};

		/* One per pane that can be on screen at once, with room to spare. Every
		   callback fires later, during the render — so a ring that wrapped inside a
		   single frame would overwrite a request that had not run yet, and two
		   panes would blur the same rectangle. */
		constexpr int k_slots = 12;

		request_t g_requests[k_slots]{};
		int g_next = 0;

		void ImGuiCallback(const ImDrawList*, const ImDrawCmd* cmd)
		{
			if (cmd == nullptr || cmd->UserCallbackData == nullptr)
				return;

			const request_t& request = *static_cast<const request_t*>(cmd->UserCallbackData);
            if(request.game && !g_game_ready) return;

			if (!g_glass.available())
			{
				/* Once, not every frame. Whether the shaders came up at all is the
				   one fact that separates "the effect is mistuned" from "the effect
				   never ran", and guessing between those two from a screenshot has
				   cost several rounds. */
				static bool warned = false;

				if (!warned)
				{
					warned = true;
					slog::log::error("glass: shaders unavailable, panes will draw flat");
				}

				return;
			}

			{
				static bool announced = false;

				if (!announced)
				{
					announced = true;
					slog::log::success("glass: first pane composited");
				}
			}

			int fb_w = 0, fb_h = 0;
			core::gl_hook::framebuffer_size(fb_w, fb_h);

			if (fb_w < 2 || fb_h < 2)
				return;

			/* canvas -> framebuffer, using the placement this frame settled on */
			const float s = vanta::g_app->panel_scale();
			const float ox = vanta::g_app->panel_x();
			const float oy = vanta::g_app->panel_y();

			std::vector<vanta::al::al_rect> rects{};
			rects.reserve(static_cast<std::size_t>(request.count));

			float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;

			for (int i = 0; i < request.count; ++i)
			{
				const pane_t& pane = request.panes[i];

				const float px = pane.x * s + ox;
				const float py = pane.y * s + oy;
				const float pw = pane.w * s;
				const float ph = pane.h * s;

				if (pw < 2.f || ph < 2.f)
					continue;

				/* al_rect is BOTTOM-LEFT origin, matching gl_FragCoord; everything
				   in the gui is top-left. */
				const float bottom = static_cast<float>(fb_h) - (py + ph);
				const float radius = pane.radius * s;

				if (rects.empty())
				{
					min_x = px;  max_x = px + pw;
					min_y = bottom; max_y = bottom + ph;
				}
				else
				{
					min_x = std::min(min_x, px);          max_x = std::max(max_x, px + pw);
					min_y = std::min(min_y, bottom);      max_y = std::max(max_y, bottom + ph);
				}

				rects.emplace_back(px, bottom, pw, ph, radius, radius, radius, radius);
			}

			if (rects.empty())
				return;

			/* The captured region is padded so the kernel has something to reach
			   into at its edges, and the padding grows with the pass count — each
			   round roughly doubles the reach, so a wider blur that is not given
			   more surround pulls the clamped edge pixels inward and the pane comes
			   out with a bright rim.

			   The region is the UNION of the panes, which is what keeps its size
			   constant frame to frame and stops the pipeline reallocating. */
			const int pad = static_cast<int>(std::ceil(10.f * s * static_cast<float>(request.passes)));

			const int bx = static_cast<int>(std::floor(min_x)) - pad;
			const int by = static_cast<int>(std::floor(min_y)) - pad;
			const int bw = static_cast<int>(std::ceil(max_x - min_x)) + pad * 2;
			const int bh = static_cast<int>(std::ceil(max_y - min_y)) + pad * 2;

			GLint bound = 0;
			glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &bound);

			/* @the scissor, which is why this drew nothing at all.

			   ImGui's GL backend enables GL_SCISSOR_TEST once and then sets a rect
			   per draw command. A user callback fires in the middle of that loop
			   with the PREVIOUS command's scissor still active — and the glass
			   pipeline never touches the scissor, because the HUD's copy of it runs
			   before ImGui exists and has no scissor to worry about. So the blur was
			   running correctly and being clipped to whatever the client happened to
			   have drawn last.

			   Turned off around the call and put back exactly as found; the
			   ResetRenderState callback queued after this one restores the rest. */
			const GLboolean scissor_was = glIsEnabled(GL_SCISSOR_TEST);
			glDisable(GL_SCISSOR_TEST);

			/* @faded by coverage, which is what a fade is.

			   This used to be done by easing the brightness back to 1 and the tint
			   to nothing, because the composite had no opacity — and it did not
			   work: at brightness 1 with no tint the shader still writes a BLURRED
			   copy of the region over itself at full coverage, so what was left
			   standing for the best part of a second was a pale, smeared box with
			   nothing in it. The shader now takes a real opacity, so the pane can
			   simply be less there. */
			const vanta::al::al_tint tint{ 0.f, 0.f, 0.f, request.darken };

			(request.game ? g_popup_glass : g_glass).render(rects,
				fb_w, fb_h,
				static_cast<unsigned int>(bound),
				bx, by, bw, bh,
				/*blur_passes=*/request.passes,
				/*brightness=*/request.brightness,
				tint,
				/*edge_softness=*/std::max(1.5f, 1.75f * s),
				/*shadow_radius=*/0.f,      /* the panel casts its own */
				/*shadow_strength=*/0.f,
				/*shadow_gamma=*/1.f,
				/*opacity=*/request.strength,
                request.game && g_game_ready ? g_game.framebuffer_id() : 0);

			if (scissor_was)
				glEnable(GL_SCISSOR_TEST);

			while (glGetError() != GL_NO_ERROR) {}
		}
	}

	void blur_under(float x, float y, float w, float h, float radius,
		float strength, float brightness, float darken, int passes)
	{
		const pane_t one{ x, y, w, h, radius };
		blur_under(&one, 1, strength, brightness, darken, passes);
	}

	void blur_under(const pane_t* panes, int count,
		float strength, float brightness, float darken, int passes)
	{
		if (panes == nullptr || count <= 0 || strength <= 0.01f)
			return;

		ImDrawList* draw_list = g_render->draw_list();
		if (draw_list == nullptr)
			return;

		request_t& request = g_requests[g_next];
		g_next = (g_next + 1) % k_slots;

		request.game = false;
        request.count = 0;
		for (int i = 0; i < count && request.count < k_max_panes; ++i)
		{
			if (panes[i].w <= 1.f || panes[i].h <= 1.f)
				continue;

			request.panes[request.count++] = panes[i];
		}

		if (request.count == 0)
			return;

		request.strength = std::clamp(strength, 0.f, 1.f);
		request.brightness = brightness;
		request.darken = std::clamp(darken, 0.f, 1.f);
		request.passes = std::clamp(passes, 1, 10);

		draw_list->AddCallback(&ImGuiCallback, &request);

		/* Hands the backend its own state back. The pipeline above binds its own
		   program, FBOs and vertex array, and the renderer would otherwise carry
		   straight on drawing with them. */
		draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
	}

    static void capture_callback(const ImDrawList*,const ImDrawCmd*) {
        g_game_ready=false;
        const auto& gl=al::gl::api();
        if(!gl.BindFramebuffer) return;
        GLint viewport[4]{}, bound=0, texture=0;
        glGetIntegerv(GL_VIEWPORT,viewport);
        glGetIntegerv(0x8CA6,&bound);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        if(g_game.ensure(viewport[2],viewport[3])) {
            gl.BindFramebuffer(0x8CA8 /*GL_READ_FRAMEBUFFER*/,0);
            glReadBuffer(GL_BACK);
            glBindTexture(GL_TEXTURE_2D,g_game.color_texture_id());
            glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,viewport[2],viewport[3]);
            g_game_ready=glGetError()==GL_NO_ERROR;
        }
        gl.BindFramebuffer(0x8D40,static_cast<unsigned>(bound));
        glBindTexture(GL_TEXTURE_2D,static_cast<unsigned>(texture));
    }
    void capture_game() {
        auto* dl=g_render->draw_list(); if(!dl) return;
        dl->AddCallback(capture_callback,nullptr);
        dl->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
    }
    void blur_game(float x,float y,float w,float h,float radius) {
        blur_under(x,y,w,h,radius,1.f,.85f,.25f,5);
        g_requests[(g_next+k_slots-1)%k_slots].game=true;
    }

	void shutdown()
	{
		g_glass.destroy();
        g_popup_glass.destroy(); g_game.destroy(); g_game_ready=false;
	}
}
