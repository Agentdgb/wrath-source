#pragma once

/* @vanta::panel_glass:
	a real frosted-glass backdrop for a panel drawn inside the vanta canvas.

	@the ordering problem, and how this gets around it.

	The ArrayList's glass runs as raw GL BEFORE the ImGui frame exists, straight
	onto the bound framebuffer. That works for the HUD because nothing is drawn
	over the HUD. It cannot work for a panel inside the client: everything the
	panel covers — the sidebar card, the content card — is drawn by ImGui AFTER
	that point, so a blur run early would sample the game and then be painted over
	by the very surfaces it was supposed to be blurring.

	The way out is ImGui's own draw callbacks. A callback is a command in the
	draw list, so it fires at exactly its position in the command stream: after
	everything queued before it, before everything queued after. Queue one just
	ahead of the panel and the blur lands on a framebuffer that already has the
	client on it, with the panel still to come.

	Coordinates are the other half of the trick. The panel is laid out in the
	1120x700 canvas and gl_hook maps that into the framebuffer only after
	ImGui::Render(), so a rect captured at queue time is in the wrong space. It is
	therefore stored in CANVAS space and converted inside the callback, where the
	panel placement for this frame is already settled.
*/
namespace vanta::panel_glass
{
	/* @blur_under:
		queues a blur of whatever is already on the framebuffer, inside the given
		rounded rect. Call it immediately before drawing the panel, on the same
		draw list. The rect is in canvas coordinates.

		Quietly does nothing when the shaders are unavailable, so a machine that
		cannot compile them simply gets the translucent panel on its own.
	*/
	/* `brightness` is the one number that decides whether this reads as glass or
		as a lamp, and it depends entirely on what is BEHIND the pane:

		  below 1 when the pane covers the game, which is bright — the ArrayList
		  uses 0.65 for exactly this reason, and anything above 1 blows the world
		  out into a white haze.

		  above 1 only when the pane covers the client's own near-black cards,
		  where an honest blur of #0C0C0C is #0C0C0C and there is nothing to see
		  unless it is lifted.

		There is no single right value, which is why it is a parameter rather than
		a constant: the same function serves both cases in the same frame. */
	/* `passes` is how many dual-Kawase rounds the region gets. Each one roughly
		doubles the reach of the blur for a fixed cost per round, so this is the
		knob that decides whether what is behind the pane is merely soft or
		genuinely unreadable — and unreadable is what makes a pane read as frosted
		rather than as smeared. Small panes can afford more of them than the whole
		window can. */
	void blur_under(float x, float y, float w, float h, float radius,
		float strength, float brightness, float darken, int passes = 6);

	/* @one composite, several panes — and why that is not a convenience.

	   There is ONE blur pipeline behind all of this, and it sizes its scene
	   texture and both ping-pong buffers to the region it was asked to capture.
	   Two panes of DIFFERENT sizes in the same frame therefore make it tear all
	   three down and rebuild them, twice, every frame, forever — the panel is
	   1120x700 and the player list is 640x412, and those can never agree.

	   Asking for both in one call fixes that by construction: one capture, one
	   set of buffers at one constant size, one blur chain. It is also the only
	   way the two surfaces can read as the SAME sheet of glass, because they
	   then come out of the same composite rather than two with their own
	   rounding of the same brightness.

	   The rects may be disjoint; the captured region is their union. */
	struct pane_t
	{
		float x{}, y{}, w{}, h{};
		float radius{};
	};

	void blur_under(const pane_t* panes, int count,
		float strength, float brightness, float darken, int passes = 6);

	/* @shutdown:
		releases the GL objects. Must run on the render thread.
	*/
	void capture_game();
	void blur_game(float x,float y,float w,float h,float radius);
	void shutdown();
}
