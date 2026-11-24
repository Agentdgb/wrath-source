#include "includes.hh"

#include "vgui.h"
#include "core/cursor_guard.hh"

/* @entry:
	In-game build: the client has no loop and no window of its own. gl_hook
	installs a wglSwapBuffers detour and drives c_app::frame() from inside
	Minecraft's own render loop, so these entry points only arm and disarm that
	hook.
*/
namespace vanta::vgui
{
	bool start(void* game_window)
	{
		return ::core::gl_hook::install(static_cast<HWND>(game_window));
	}

	void stop()
	{
		// GL objects have to die on the render thread; gl_hook coordinates that
		// and only returns once the render thread has acknowledged.
		::core::gl_hook::request_shutdown();
		::core::gl_hook::uninstall();
	}

	bool isRunning() { return ::core::gl_hook::installed(); }

	void toggleVisible()
	{
		if (!vanta::g_app) return;

		vanta::g_app->toggle_visible();

		/* The cursor is NOT handed over here. gl_hook does it every frame from
		   the panel's actual visibility, so that every way of showing or hiding
		   it — this key, Escape, the panel coming up on inject — releases and
		   recaptures the pointer identically. */
		slog::log::info("gui: panel {}", vanta::g_app->visible() ? "opened" : "closed");
	}

	bool isVisible() { return vanta::g_app && vanta::g_app->visible(); }

	void setHotkeys(int toggle_vk, int panic_vk)
	{
		::core::gl_hook::set_hotkeys(toggle_vk, panic_vk);
	}

	void setStreamProof(bool enabled)
	{
		::core::gl_hook::set_stream_proof(enabled);
	}

	bool isStreamProof()
	{
		return ::core::gl_hook::stream_proof();
	}
}
