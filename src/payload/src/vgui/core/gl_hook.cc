#include "modules/combat/triggerbot/triggerbot_hook.h"
#include "modules/combat/antibot/antibot.h"
#include "modules/render/keybindhud/keybindhud.h"
#include "../includes.hh"

#include "gl_hook.hh"
#include "cursor_guard.hh"
#include "core/config/ConfigManager.h"
#include "core/ClientState.h"
#include "core/UnloadSafety.h"

#include "../dt/views/intro.hh"

#include "core/event/GameFramePump.h"
#include "core/keybind/KeybindManager.h"
#include "modules/render/arraylist/arraylist.h"
#include "modules/render/watermark/watermark.h"
#include "modules/render/hitfx/hitfx.h"
#include "modules/render/esp/esp.h"
#include "modules/combat/backtrack/backtrack.h"
#include "modules/render/blockesp/blockesp.h"
#include "modules/render/chams/item_shader_hook.h"
#include "modules/visuals/worldtweaks/worldtweaks.h"
#include "modules/render/targetesp/targetesp.h"
#include "modules/combat/triggerbot/triggerbot.h"
#include "modules/render/targethud/targethud.h"
#include "modules/render/radar/radar.h"
#include "modules/render/notifier/notifier.h"
#include "jvm/JvmContext.h"
#include "jvm/JniHelper.h"
#include "modules/misc/spectate/spectate.h"
#include "modules/render/outline/outline.h"
#include "modules/render/spotify/spotify.h"
#include "modules/render/chams/chams.h"
#include "modules/render/chams/chams_gl.h"   // set_suppressed() around our own context
#include "modules/render/arraylist/al_gl.h"
#include "entry/Bootstrap.h"
#include "modules/visuals/ports/VisualPorts.h"
#include "modules/visuals/bloom/Bloom.h"
#include "vgui/vgui.h"

#include <MinHook.h>
#include <jnihook.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstring>
#include <dwmapi.h>
#include <gl/GL.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace core::gl_hook
{
	namespace
	{
		using PFN_wglSwapBuffers = BOOL(WINAPI*)(HDC);
		PFN_wglSwapBuffers o_wglSwapBuffers = nullptr;
		void*              g_swap_target    = nullptr;
		bool               g_swap_is_iat    = false;  // true when g_swap_target is an IAT cell ptr

		HWND    g_window       = nullptr;
		WNDPROC g_orig_wndproc = nullptr;

		HGLRC   g_our_ctx      = nullptr;
		std::atomic<bool> g_gui_ready{false};

		HWND               g_stream_hwnd           = nullptr;
		HDC                g_stream_dc             = nullptr;
		ATOM               g_stream_atom           = 0;
		bool               g_stream_visible        = false;
		bool               g_stream_topmost        = false;
		bool               g_stream_vsync_disabled = false;
		int                g_stream_x              = -32000;
		int                g_stream_y              = -32000;
		int                g_stream_w              = 0;
		int                g_stream_h              = 0;
		int                g_stream_last_fb_w      = 0;
		int                g_stream_last_fb_h      = 0;
		ULONGLONG          g_stream_last_geom_tick = 0;
		dt::al::al_fbo  g_stream_fbo{};

		/* Raised by a module driving a Minecraft screen on the player's behalf.
		   Written from the game/pump thread, read in the window procedure on
		   the message thread — hence atomic. See gl_hook.hh for the levels. */
		std::atomic<int> g_input_filter{ 0 };

		/* Freecam's own capture, separate from the filter levels above.

		   While the camera is detached the fly keys and the mouse buttons
		   belong to the camera, not to the player's body: a W that reached
		   GLFW would press KeyBinding.forward and walk the (invisible) real
		   player, a click would swing at whatever the frozen body faces. Only
		   the DOWN messages of that exact key set are swallowed -- releases,
		   mouse movement, Escape, E, T, F5 and every other key pass -- and the
		   wheel is kept as a speed dial, its raw delta accumulated here for
		   the module to take. GetAsyncKeyState still reports the swallowed
		   keys, which is how the module reads them (same trick as the panel
		   keys). Written from the pump thread, read on the message thread. */
		std::atomic<bool> g_freecam_capture{ false };
		std::atomic<int>  g_freecam_wheel_raw{ 0 };
		int     g_settle       = 0;

		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_shutdown_requested{ false };
		std::atomic<bool> g_shutdown_done{ false };
		std::atomic<int>  g_in_detour{ 0 };
		std::atomic<int>  g_in_wndproc{ 0 };

		std::atomic<int>  g_fb_w{ 0 };
		std::atomic<int>  g_fb_h{ 0 };

		std::atomic<int>       g_toggle_vk{ VK_INSERT };
		std::atomic<int>       g_panic_vk { 0 };
		std::atomic<bool>      g_toggle_key_down{ false };
		std::atomic<ULONGLONG> g_toggle_rebind_tick{ 0 };
		std::atomic<bool>      g_stream_proof{ false };

		/* GL enums we need but that <gl/GL.h> (an OpenGL 1.1 header) predates. */
		constexpr GLenum kMajorVersion            = 0x821B;
		constexpr GLenum kMinorVersion            = 0x821C;
		constexpr GLenum kContextProfileMask      = 0x9126;
		constexpr GLenum kDebugOutput             = 0x92E0;
		constexpr GLenum kDebugOutputSynchronous  = 0x8242;
		constexpr GLenum kFramebufferSrgb         = 0x8DB9;
		constexpr GLenum kFramebuffer             = 0x8D40;
		constexpr GLenum kReadFramebuffer         = 0x8CA8;
		constexpr GLenum kDrawFramebuffer         = 0x8CA9;

		/* FBO entry points arrived in GL 3.0; <gl/GL.h> stops at 1.1, so this
		   one has to be resolved through wglGetProcAddress. */
		using PFN_glBindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
		using PFN_glBlitFramebuffer = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
		using PFN_wglSwapIntervalEXT = BOOL(WINAPI*)(int);
		PFN_glBindFramebuffer  p_glBindFramebuffer  = nullptr;
		PFN_glBlitFramebuffer  p_glBlitFramebuffer  = nullptr;
		PFN_wglSwapIntervalEXT p_wglSwapIntervalEXT = nullptr;

		constexpr int kCtxMajorArb   = 0x2091;
		constexpr int kCtxMinorArb   = 0x2092;
		constexpr int kCtxProfileArb = 0x9126;
		constexpr int kCoreBit       = 0x00000001;
		constexpr int kCompatBit     = 0x00000002;

		using PFN_wglCreateContextAttribsARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

		/* ------------------------------------------------------------------ */
		/* window procedure                                                    */
		/* ------------------------------------------------------------------ */

		bool is_key_message(UINT msg)
		{
			return msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN
				|| msg == WM_KEYUP   || msg == WM_SYSKEYUP;
		}

		int resolve_vk(WPARAM wp, LPARAM lp)
		{
			const int vk = static_cast<int>(wp);
			const bool extended = (lp & (1 << 24)) != 0;
			if (vk == VK_SHIFT)
			{
				const UINT scancode = static_cast<UINT>((lp >> 16) & 0xFF);
				if (scancode != 0)
				{
					const UINT mapped = MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
					if (mapped == VK_LSHIFT || mapped == VK_RSHIFT)
						return static_cast<int>(mapped);
				}
				if ((GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0)
					return VK_RSHIFT;
				if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0)
					return VK_LSHIFT;
			}
			else if (vk == VK_CONTROL)
			{
				return extended ? VK_RCONTROL : VK_LCONTROL;
			}
			else if (vk == VK_MENU)
			{
				return extended ? VK_RMENU : VK_LMENU;
			}
			return vk;
		}

        void update_spotify_chat()
        {
            if (!::dt::modules::spotify::g_enabled)
            {
                ::dt::modules::spotify::set_chat_open(false);
                return;
            }

            static ULONGLONG s_last_check = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - s_last_check < 50)
                return;
            s_last_check = now;

            using namespace ::dt::jvm;
            bool open = false;
            JniScope scope;
            if (scope && ctx().jvmti.load()) {
                JNIEnv* env = scope.get();
                auto mc = staticCallObjectCanon(env, "MinecraftClient", "getInstance");
                if (mc) {
                    LocalObject screen(env, getObjectFieldCanon(env, mc.get(), "MinecraftClient", "currentScreen"));
                    if (screen) {
                        LocalRef<jclass> cls(env, env->GetObjectClass(screen.get()));
                        char* signature = nullptr;
                        if (cls && ctx().jvmti.load()->GetClassSignature(cls.get(), &signature, nullptr) == JVMTI_ERROR_NONE) {
                            open = signature && (std::strcmp(signature, "Lnet/minecraft/class_408;") == 0
                                || std::strcmp(signature, "Lnet/minecraft/client/gui/screen/ChatScreen;") == 0
                                || std::strcmp(signature, "Lnet/minecraft/client/gui/screens/ChatScreen;") == 0);
                            if (signature) ctx().jvmti.load()->Deallocate(reinterpret_cast<unsigned char*>(signature));
                        }
                    }
                }
                checkAndClearException(env, "Spotify chat state");
            }
            ::dt::modules::spotify::set_chat_open(open);
        }

		LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			g_in_wndproc.fetch_add(1, std::memory_order_acq_rel);
			struct leave_t { ~leave_t() { g_in_wndproc.fetch_sub(1, std::memory_order_release); } } leave;
			const auto original = g_orig_wndproc;
            ::dt::modules::antibot::observe_window_input(hwnd, msg, wp, lp,
                dt::g_app && dt::g_app->visible());
			std::unique_lock state(dt::clientStateMutex());
			if (msg == WM_CLOSE || msg == WM_DESTROY)
			{
				static std::atomic<bool> s_savedOnExit{ false };
				if (!s_savedOnExit.exchange(true))
				{
					auto& cm = ::dt::ConfigManager::get();
					cm.saveProfile(cm.currentProfile());
				}
			}

			/* Convert F11 Exclusive Fullscreen transitions into Borderless Composed
			   Fullscreen (+1px height + cancel CDS_FULLSCREEN) ONLY while StreamProof
			   is active. When StreamProof is off, F11 runs in 100% native Exclusive
			   Fullscreen with zero DWM composition overhead. */
			if (g_stream_proof.load(std::memory_order_relaxed) && msg == WM_WINDOWPOSCHANGING && lp != 0)
			{
				auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
				if ((pos->flags & SWP_NOSIZE) == 0 && pos->cx > 0 && pos->cy > 0)
				{
					const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
					if ((style & WS_POPUP) != 0)
					{
						MONITORINFOEXW mi{};
						mi.cbSize = sizeof(mi);
						HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
						if (mon && GetMonitorInfoW(mon, &mi))
						{
							const int mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
							const int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
							if (pos->cx == mon_w && pos->cy == mon_h)
							{
								ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
								SetPropW(hwnd, L"Microsoft.Windows.Windowing.DisallowDirectFlip",
									reinterpret_cast<HANDLE>(1));
								pos->cy = mon_h + 1;
							}
						}
					}
				}
			}

			if (dt::vgui::bridge::isMenuKeyBinding())
			{
				if (is_key_message(msg))
				{
					if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
					{
						const int ext_vk = resolve_vk(wp, lp);
						if (ext_vk == VK_ESCAPE)
						{
							g_toggle_key_down.store(true, std::memory_order_relaxed);
							g_toggle_rebind_tick.store(GetTickCount64(), std::memory_order_relaxed);
							dt::vgui::bridge::setMenuKeyBinding(false);
						}
						else if (ext_vk > 0 && ext_vk < 256
							&& ext_vk != VK_LBUTTON && ext_vk != VK_RBUTTON && ext_vk != VK_MBUTTON
							&& ext_vk != VK_XBUTTON1 && ext_vk != VK_XBUTTON2 && ext_vk != VK_CANCEL
							&& ext_vk != VK_SHIFT && ext_vk != VK_CONTROL && ext_vk != VK_MENU)
						{
							g_toggle_key_down.store(true, std::memory_order_relaxed);
							g_toggle_rebind_tick.store(GetTickCount64(), std::memory_order_relaxed);
							dt::vgui::bridge::setGuiToggleKey(ext_vk);
							dt::vgui::bridge::setMenuKeyBinding(false);
						}
					}
					return 0;
				}
				if (msg == WM_CHAR || msg == WM_SYSCHAR)
					return 0;
			}

			/* The panel's toggle and panic keys are handled directly here on keydown.
			   Edge-detected via bit 30 of lParam (0 = initial press, 1 = autorepeat).
			   This eliminates the 15 ms polling thread and provides 0 ms latency,
			   while ensuring GLFW/Minecraft never receive the keystroke. */
			if (is_key_message(msg))
			{
				const int raw_vk = static_cast<int>(wp);
				const int ext_vk = resolve_vk(wp, lp);
				const int toggle_vk = g_toggle_vk.load(std::memory_order_relaxed);
				const bool matches_toggle = (toggle_vk > 0 && (
					raw_vk == toggle_vk
					|| ext_vk == toggle_vk
					|| (raw_vk == VK_SHIFT && (toggle_vk == VK_LSHIFT || toggle_vk == VK_RSHIFT) && (GetAsyncKeyState(toggle_vk) & 0x8000) != 0)
					|| (raw_vk == VK_CONTROL && (toggle_vk == VK_LCONTROL || toggle_vk == VK_RCONTROL) && (GetAsyncKeyState(toggle_vk) & 0x8000) != 0)
					|| (raw_vk == VK_MENU && (toggle_vk == VK_LMENU || toggle_vk == VK_RMENU) && (GetAsyncKeyState(toggle_vk) & 0x8000) != 0)))
					|| raw_vk == VK_INSERT || ext_vk == VK_INSERT
					|| (raw_vk == VK_SHIFT && (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);

				const bool editing_text = g_gui_ready && dt::g_app && dt::g_app->visible()
					&& dt::g_app->text_focus();
				if (matches_toggle && !dt::vgui::bridge::isMenuKeyBinding() && !editing_text)
				{
					if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
					{
						const bool is_repeat = (lp & (1 << 30)) != 0;
						const ULONGLONG now_tick = GetTickCount64();
						if (!is_repeat
							&& (now_tick - g_toggle_rebind_tick.load(std::memory_order_relaxed) > 200)
							&& !g_toggle_key_down.exchange(true, std::memory_order_relaxed))
						{
							if (g_gui_ready && dt::g_app)
							{
								dt::vgui::toggleVisible();
							}
						}
					}
					else if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
					{
						g_toggle_key_down.store(false, std::memory_order_relaxed);
					}
					return 0;
				}
				const int panic_vk = g_panic_vk.load(std::memory_order_relaxed);
				if (panic_vk > 0 && (raw_vk == panic_vk || ext_vk == panic_vk))
				{
					if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
					{
						const bool is_repeat = (lp & (1 << 30)) != 0;
						if (!is_repeat)
						{
							dt::entry::requestFullUnload();
						}
					}
					return 0;
				}
			}

			const bool gui_open = g_gui_ready && dt::g_app && dt::g_app->visible();

			/* Freecam capture (see g_freecam_capture). Never while the panel is
			   up: the panel owns the mouse then and swallows its own input
			   below. The module drops the flag itself while a Minecraft screen
			   is open, so chat and containers keep their keys. */
			if (g_freecam_capture.load(std::memory_order_relaxed) && !gui_open)
			{
				switch (msg)
				{
				case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
				case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
				case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
					return 0;

				case WM_MOUSEWHEEL:
					g_freecam_wheel_raw.fetch_add(static_cast<int>(GET_WHEEL_DELTA_WPARAM(wp)),
					                              std::memory_order_relaxed);
					return 0;

				case WM_MOUSEHWHEEL:
					return 0;

				case WM_KEYDOWN: case WM_SYSKEYDOWN:
				{
					/* Repeats included: a repeat that reached GLFW would press
					   the binding the module just left unpressed. */
					const int vk = static_cast<int>(wp);
					if (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' || vk == VK_SPACE
						|| vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT
						|| vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
						return 0;
					break;
				}

				default:
					break;
				}
			}

			/* Module-driven screen in progress (AutoTotem inventory mode).
			   Drop the input that could move an item, or be mistaken for a
			   world interaction, before the game sees it. */
			if (const int filter = g_input_filter.load(std::memory_order_relaxed))
			{
				/* One-shot proof that this filter is actually on the message
				   path. If a module raises the lock and this never fires, the
				   subclass is not seeing input at all and every behaviour built
				   on the lock is silently a no-op. */
				static bool s_logged = false;
				if (!s_logged) {
					s_logged = true;
					slog::log::info("gl: input filter is live (first message swallowed)");
				}

				switch (msg)
				{
				/* Mouse PRESSES go at both levels.

				   Attack and use are what the server sees as a world
				   interaction, and a module that has just driven a container
				   must not have one land next to its own traffic. Level 1
				   exists precisely so this stays blocked while the keyboard
				   is handed back. Double-click counts as a press: Windows
				   sends it INSTEAD of the second WM_*BUTTONDOWN. */
				case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
				case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
				case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
					return 0;

				/* Mouse RELEASES deliberately fall through, for exactly the
				   reason key releases do — and this half was missing.

				   Swallowing a release latches whatever was held when the
				   filter went up. Hold left-click to mine, let AutoTotem raise
				   the filter mid-swing, let go: the WM_LBUTTONUP never reached
				   GLFW, so KeyBinding.attack stayed pressed and the game kept
				   breaking the block long after the module was done. That is
				   the "it gets stuck left-clicking after a silent totem" report.

				   Passing a release is always safe. A release only ENDS an
				   action, never starts one, and if the matching press was
				   swallowed the game just sets an already-unpressed binding to
				   unpressed, which is a no-op. */
				case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
					break;

				case WM_KEYDOWN: case WM_SYSKEYDOWN:
				{
					/* Key-downs only at level 2, and there for one reason:
					   Windows key REPEAT. Holding a key makes Windows resend
					   WM_KEYDOWN ~30x/second; GLFW turns those into
					   GLFW_REPEAT, and Minecraft answers each one with
					   KeyBinding.setKeyPressed(key, true).

					   A module that zeroes a KeyBinding from the swapBuffers
					   pump is writing in the very window where glfwPollEvents
					   runs, so a repeat lands immediately after and undoes it.
					   That is why holding a direction sometimes defeated the
					   movement suppression entirely. Dropping the repeats at
					   the window is what makes the zero stick.

					   Escape is swallowed too. A module holding the player's
					   inventory open needs that window to stay exactly as it
					   found it: closing mid-swap leaves a click addressed to a
					   container the server has already torn down, which reads
					   as an invalid interaction. The lock is bounded instead —
					   a hard tick ceiling plus a revalidation on every pump —
					   so the screen can never stay stuck.

					   The panic and GUI-toggle keys are handled ABOVE this
					   block and never reach here, so the player always keeps a
					   way out of the client itself.

					   Once the screen is down the module drops to level 1, and
					   the very next repeat re-presses whatever the player is
					   still holding — which is what hands walking back without
					   the module having to synthesise a single key event.

					   Key RELEASES deliberately still pass at every level:
					   swallowing those would latch whatever was held down. */
					if (filter >= 2) return 0;
					break;
				}

				default:
					break;
				}
			}

            // Chat keeps keyboard input; only Spotify mouse gestures are consumed.
            static bool spotify_press = false;
            /* Set when the Spotify branch already queued this message into ImGui,
               so the release forwarding below does not queue it a second time. */
            bool forwarded_by_spotify = false;
            if (!gui_open && g_gui_ready && ::dt::modules::spotify::chat_open()
                && ::dt::modules::spotify::g_enabled) {
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(hwnd, &cursor);
                RECT client{};
                GetClientRect(hwnd, &client);
                const ImVec2 display = ImGui::GetIO().DisplaySize;
                const float mouse_x = cursor.x * display.x / std::max(1L, client.right);
                const float mouse_y = cursor.y * display.y / std::max(1L, client.bottom);
                float x, y, w, h;
                ::dt::modules::spotify::query_bbox(x, y, w, h);
                const bool over = w > 0.f && mouse_x >= x && mouse_x < x + w
                    && mouse_y >= y && mouse_y < y + h;
                if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP
                    || msg == WM_LBUTTONDBLCLK || (msg == WM_MOUSEWHEEL && over))
                {
                    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
                    forwarded_by_spotify = true;
                }
                if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && over) {
                    spotify_press = true;
                    return 0;
                }
                if (msg == WM_LBUTTONUP && spotify_press) {
                    spotify_press = false;
                    return 0;
                }
                if (msg == WM_MOUSEWHEEL && over) return 0;
            } else {
                spotify_press = false;
            }

			/* Releases and focus loss reach ImGui even while the panel is CLOSED.

			   The press was forwarded while the panel was open (the block below);
			   when that same press is what hid the panel — Escape — its WM_KEYUP
			   arrives here with gui_open == false and used to go straight to the
			   game, so ImGui never saw the key let go. NewFrame keeps running on
			   every swap whether or not the panel paints, so the key aged as
			   "held" for as long as the panel stayed hidden, and the next time it
			   came up IsKeyPressed(Escape) with repeat fired every 50 ms: the
			   keybind popup opened and shut itself within a couple of frames. A
			   mouse button held across the hide latched the same way and ate the
			   first click after reopen.

			   Only releases and focus messages go through — never a press, a
			   move or a char, or ImGui would start tracking input whose release
			   the open-panel path then swallows, which is this bug in reverse.
			   Nothing here is swallowed: the game needs every one of these. */
			if (!gui_open && g_gui_ready && !forwarded_by_spotify)
			{
				switch (msg)
				{
				case WM_KEYUP: case WM_SYSKEYUP:
				case WM_KILLFOCUS: case WM_SETFOCUS:
					ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
					break;

				case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
				{
					/* Gated on ImGui actually believing the button is held. The
					   backend answers a release with ReleaseCapture(), and GLFW
					   runs its own SetCapture/ReleaseCapture over the buttons it
					   tracks; an unprompted release while GLFW still holds
					   another button would end its capture early. When the gate
					   is true the ReleaseCapture is exactly the one that should
					   have happened when the panel closed. */
					const int button = msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : msg == WM_MBUTTONUP ? 2
						: (GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4);
					if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().MouseDown[button])
						ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
					break;
				}

				default:
					break;
				}
			}

			if (gui_open)
			{
				/* Answer WM_SETCURSOR ourselves and do NOT chain. GLFW's handler
				   calls SetCursor(nullptr) whenever its cursor mode is disabled,
				   which is every mouse move while the panel is open — the
				   pointer would be hidden a frame after ImGui drew it and blink
				   continuously. Returning TRUE here says "handled, leave it
				   alone". cursor_guard covers the paths that do not come through
				   a window message. */
				if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT)
				{
					SetCursor(LoadCursorW(nullptr, IDC_ARROW));
					return TRUE;
				}

				ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);

				/* Swallow the input the panel is consuming so the game does not
				   also act on it — otherwise clicking a module would swing the
				   camera and typing in the search box would trigger keybinds. */
				switch (msg)
				{
				case WM_MOUSEMOVE:
				case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
				case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
				case WM_MBUTTONDOWN: case WM_MBUTTONUP:
				case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
				case WM_CHAR:        case WM_SYSCHAR:
					return 0;

				case WM_KEYDOWN: case WM_SYSKEYDOWN:
				{
					/* Let movement keys reach the game so the player can still
					   walk/strafe/jump/sneak with the panel open — UNLESS a text
					   field (the module search box) is focused, in which case
					   typing must not also drive the game. The mouse still fully
					   drives the panel; only these movement key-downs fall
					   through to the game. */
					const int vk = static_cast<int>(wp);
					const bool is_move =
						vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' ||
						vk == VK_SPACE ||
						vk == VK_SHIFT || vk == VK_LSHIFT ||
						vk == VK_CONTROL || vk == VK_LCONTROL;
					/* Asked of the client, not of ImGui. WantCaptureKeyboard is
					   only ever raised by an ImGui widget and this gui has none —
					   every field in it is drawn by hand — so the guard the comment
					   above describes has never actually fired, and typing a name
					   into any search box strafed the player at the same time. */
					if (is_move && !(dt::g_app && dt::g_app->text_focus()))
						break;      /* fall through to CallWindowProcW → game */
					return 0;
				}

				case WM_KEYUP: case WM_SYSKEYUP:
					/* Releases MUST reach the game. Swallowing them leaves any
					   key that was held when the panel opened latched down —
					   open the gui while walking and the player keeps running
					   into a wall until the panel closes. */
					break;

				default:
					break;
				}
			}

			state.unlock();
			return original ? CallWindowProcW(original, hwnd, msg, wp, lp)
			                : DefWindowProcW(hwnd, msg, wp, lp);
		}

		/* ------------------------------------------------------------------ */
		/* context creation                                                    */
		/* ------------------------------------------------------------------ */

		HGLRC create_overlay_context(HDC dc, HGLRC host)
		{
			auto wglCreateContextAttribsARB = reinterpret_cast<PFN_wglCreateContextAttribsARB>(
				wglGetProcAddress("wglCreateContextAttribsARB"));

			GLint host_major = 0, host_minor = 0, host_profile = 0;
			glGetIntegerv(kMajorVersion, &host_major);
			glGetIntegerv(kMinorVersion, &host_minor);
			if (host_major >= 3 && (host_major > 3 || host_minor >= 2))
				glGetIntegerv(kContextProfileMask, &host_profile);

			char profile_hex[16];
			std::snprintf(profile_hex, sizeof(profile_hex), "0x%X", static_cast<unsigned>(host_profile));
			slog::log::info("gl: host context is {}.{} profile_mask={}",
				host_major, host_minor, profile_hex);

			if (wglCreateContextAttribsARB && host)
			{
				/* Mirror the host's VERSION but always ask for the
				   COMPATIBILITY profile.

				   wglCreateContext() alone hands back a legacy 1.x compat
				   context. Minecraft 1.21 with Sodium runs a 3.3 CORE context,
				   and sharing a legacy context with a core one is where this
				   goes wrong: the drivers report success and then silently fail
				   to share VAOs, samplers and FBOs, after which
				   ImGui_ImplOpenGL3 dereferences null function pointers and
				   javaw dies a few seconds later. Requesting the same version
				   through the ARB entry point, with the share context passed
				   in, is what makes the sharing real. */
				const int major = host_major >= 3 ? host_major : 3;
				const int minor = host_major >= 3 ? host_minor : 3;

				const int attribs[] = {
					kCtxMajorArb,   major,
					kCtxMinorArb,   minor,
					kCtxProfileArb, kCompatBit,
					0
				};

				if (HGLRC ctx = wglCreateContextAttribsARB(dc, host, attribs))
				{
					slog::log::success("gl: created {}.{} compatibility context (shared)", major, minor);
					return ctx;
				}

				slog::log::warning("gl: ARB context creation failed (err {}), falling back",
					static_cast<unsigned>(GetLastError()));
			}

			HGLRC ctx = wglCreateContext(dc);
			if (!ctx)
			{
				slog::log::error("gl: wglCreateContext failed, err {}",
					static_cast<unsigned>(GetLastError()));
				return nullptr;
			}

			/* Only attempt legacy sharing when the host is NOT a core profile.
			   wglShareLists across a core/legacy boundary returns TRUE and does
			   not actually share — the single most common crash in this class
			   of overlay. */
			const bool host_is_core = (host_profile & kCoreBit) != 0;
			if (host_is_core)
			{
				slog::log::warning("gl: host is a core profile — skipping wglShareLists "
					"(it would report a false positive)");
			}
			else if (host && wglShareLists(host, ctx))
			{
				slog::log::info("gl: legacy context sharing established");
			}

			return ctx;
		}

		void quiet_debug_output()
		{
			/* Sodium forces GL_DEBUG_OUTPUT_SYNCHRONOUS on NVIDIA so its own
			   pipeline sees errors on the causing thread. The side effect for
			   an overlay is fatal: any GL error we emit aborts inside the
			   driver and takes javaw with it, with no catchable exception.
			   Debug output is per-context, so turning it off on OURS leaves
			   the game's context untouched. */
			glDisable(kDebugOutput);
			glDisable(kDebugOutputSynchronous);
			while (glGetError() != GL_NO_ERROR) { /* drain */ }
		}

		/* @reset_our_state:
			our context keeps whatever we left in it last frame, and it also
			inherits nothing useful from the game's. Put the handful of things
			ImGui depends on back to known values before drawing.
		*/
		void reset_our_state(int width, int height, GLuint target_fbo = 0)
		{
			/* Iris and Sodium render most of the world into their own
			   framebuffers. If one is still bound our draw calls land in an
			   offscreen target and the panel is simply not on screen. */
			if (p_glBindFramebuffer)
				p_glBindFramebuffer(kFramebuffer, target_fbo);

			glViewport(0, 0, width, height);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_CULL_FACE);
			glDisable(GL_SCISSOR_TEST);

			/* Shader packs enable sRGB conversion on the default framebuffer.
			   ImGui's vertex colours are already in sRGB, so leaving it on
			   converts them twice and the whole panel washes out. */
			glDisable(kFramebufferSrgb);

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}

		/* ------------------------------------------------------------------ */
		/* StreamProof hardware overlay window (WDA_EXCLUDEFROMCAPTURE)        */
		/* ------------------------------------------------------------------ */

		inline const wchar_t* stream_class_name()
		{
			static wchar_t s_name[48] = { 0 };
			if (s_name[0] == 0)
			{
				swprintf_s(s_name, L"CiceroUI_%08X", GetCurrentProcessId() ^ 0x5A5A);
			}
			return s_name;
		}

		LRESULT CALLBACK stream_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			switch (msg)
			{
			case WM_ERASEBKGND:
				return 1;
			case WM_NCHITTEST:
				return HTTRANSPARENT;
			case WM_MOUSEACTIVATE:
				return MA_NOACTIVATE;
			case WM_CLOSE:
				if (g_stream_dc)
				{
					ReleaseDC(hwnd, g_stream_dc);
					g_stream_dc = nullptr;
				}
				DestroyWindow(hwnd);
				if (g_stream_hwnd == hwnd)
					g_stream_hwnd = nullptr;
				return 0;
			default:
				return DefWindowProcW(hwnd, msg, wp, lp);
			}
		}

		void hide_stream_window()
		{
			if (g_stream_hwnd && g_stream_visible)
			{
				ShowWindow(g_stream_hwnd, SW_HIDE);
				g_stream_visible = false;
			}
		}

		void destroy_stream_window()
		{
			if (g_stream_hwnd)
			{
				if (g_stream_dc)
				{
					ReleaseDC(g_stream_hwnd, g_stream_dc);
					g_stream_dc = nullptr;
				}
				DestroyWindow(g_stream_hwnd);
				g_stream_hwnd = nullptr;
			}
			if (g_stream_atom)
			{
				UnregisterClassW(stream_class_name(), GetModuleHandleW(nullptr));
				g_stream_atom = 0;
			}
			g_stream_visible        = false;
			g_stream_topmost        = false;
			g_stream_vsync_disabled = false;
			g_stream_x              = -32000;
			g_stream_y              = -32000;
			g_stream_w              = 0;
			g_stream_h              = 0;
			g_stream_last_fb_w      = 0;
			g_stream_last_fb_h      = 0;
			g_stream_last_geom_tick = 0;

			/* Restore the game window's native fullscreen geometry and Direct Flip
			   eligibility when StreamProof is turned off. */
			if (g_window && IsWindow(g_window))
			{
				RemovePropW(g_window, L"Microsoft.Windows.Windowing.DisallowDirectFlip");
				const LONG_PTR style = GetWindowLongPtrW(g_window, GWL_STYLE);
				if ((style & WS_POPUP) != 0)
				{
					MONITORINFOEXW mi{};
					mi.cbSize = sizeof(mi);
					if (HMONITOR mon = MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST))
					{
						if (GetMonitorInfoW(mon, &mi))
						{
							const int mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
							const int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
							RECT win_rc{};
							if (GetWindowRect(g_window, &win_rc)
								&& (win_rc.right - win_rc.left) == mon_w
								&& (win_rc.bottom - win_rc.top) == mon_h + 1)
							{
								SetWindowPos(
									g_window,
									HWND_TOP,
									mi.rcMonitor.left,
									mi.rcMonitor.top,
									mon_w,
									mon_h,
									SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
							}
						}
					}
				}
			}
		}

		bool ensure_stream_window(HDC game_dc, int fb_w, int fb_h)
		{
			const ULONGLONG now = GetTickCount64();
			if (g_stream_hwnd && g_stream_dc
				&& fb_w == g_stream_last_fb_w && fb_h == g_stream_last_fb_h
				&& (now - g_stream_last_geom_tick) < 50)
			{
				MSG msg{};
				while (PeekMessageW(&msg, g_stream_hwnd, 0, 0, PM_REMOVE))
				{
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}
				return true;
			}
			g_stream_last_geom_tick = now;
			g_stream_last_fb_w      = fb_w;
			g_stream_last_fb_h      = fb_h;

			if (!g_window || !IsWindow(g_window) || IsIconic(g_window) || !IsWindowVisible(g_window))
			{
				hide_stream_window();
				return false;
			}

			int mon_w = 0;
			int mon_h = 0;
			MONITORINFOEXW mi{};
			mi.cbSize = sizeof(mi);
			if (HMONITOR mon = MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST))
			{
				if (GetMonitorInfoW(mon, &mi))
				{
					mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
					mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;

					/* If Minecraft was already in F11 Exclusive Fullscreen before injection
					   (or transitioned outside WM_WINDOWPOSCHANGING), demote it to
					   Borderless Composed Fullscreen (+1px height) immediately so the
					   OpenGL driver never enters Exclusive Fullscreen scanout. */
					RECT win_rc{};
					GetWindowRect(g_window, &win_rc);
					const LONG_PTR style = GetWindowLongPtrW(g_window, GWL_STYLE);
					if ((style & WS_POPUP) != 0
						&& (win_rc.right - win_rc.left) == mon_w
						&& (win_rc.bottom - win_rc.top) == mon_h)
					{
						ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
						SetPropW(g_window, L"Microsoft.Windows.Windowing.DisallowDirectFlip",
							reinterpret_cast<HANDLE>(1));
						SetWindowPos(
							g_window,
							HWND_TOP,
							mi.rcMonitor.left,
							mi.rcMonitor.top,
							mon_w,
							mon_h + 1,
							SWP_NOACTIVATE | SWP_NOZORDER);
					}
				}
			}

			/* Never allow the overlay window's rectangle to match the full monitor
			   dimensions (mon_w x mon_h). If a WS_POPUP OpenGL window equals the
			   monitor rectangle, DWM / the GPU driver promotes it to Fullscreen
			   Direct Flip, which disables DwmExtendFrameIntoClientArea alpha
			   blending and turns its transparent background into flashing black. */
			const bool is_fullscreen = (mon_w > 0 && mon_h > 0 && fb_w >= mon_w && fb_h >= mon_h);
			const int  stream_w      = fb_w;
			const int  stream_h      = is_fullscreen ? std::max(1, mon_h - 1) : fb_h;

			if (!g_stream_hwnd || !IsWindow(g_stream_hwnd))
			{
				g_stream_hwnd           = nullptr;
				g_stream_dc             = nullptr;
				g_stream_visible        = false;
				g_stream_vsync_disabled = false;

				HINSTANCE hinst = GetModuleHandleW(nullptr);
				if (!g_stream_atom)
				{
					WNDCLASSEXW wc{};
					wc.cbSize        = sizeof(wc);
					wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
					wc.lpfnWndProc   = &stream_wndproc;
					wc.hInstance     = hinst;
					wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
					wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
					wc.lpszClassName = stream_class_name();
					g_stream_atom    = RegisterClassExW(&wc);
				}

				POINT pt{ 0, 0 };
				ClientToScreen(g_window, &pt);

				g_stream_hwnd = CreateWindowExW(
					WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
					stream_class_name(),
					L"",
					WS_POPUP,
					pt.x, pt.y, stream_w, stream_h,
					nullptr,
					nullptr,
					hinst,
					nullptr);

				if (!g_stream_hwnd)
					return false;

				SetPropW(g_stream_hwnd, L"Microsoft.Windows.Windowing.DisallowDirectFlip",
					reinterpret_cast<HANDLE>(1));
				SetLayeredWindowAttributes(g_stream_hwnd, 0, 255, LWA_ALPHA);
				const MARGINS margins = { -1, -1, -1, -1 };
				DwmExtendFrameIntoClientArea(g_stream_hwnd, &margins);
				SetWindowDisplayAffinity(g_stream_hwnd, WDA_EXCLUDEFROMCAPTURE);

				g_stream_dc = GetDC(g_stream_hwnd);
				if (!g_stream_dc)
				{
					destroy_stream_window();
					return false;
				}

				const int fmt = GetPixelFormat(game_dc);
				PIXELFORMATDESCRIPTOR pfd{};
				if (fmt <= 0 || !DescribePixelFormat(game_dc, fmt, sizeof(pfd), &pfd))
				{
					destroy_stream_window();
					return false;
				}
				pfd.dwFlags   |= PFD_SUPPORT_COMPOSITION;
				pfd.cAlphaBits = 8;
				if (!SetPixelFormat(g_stream_dc, fmt, &pfd))
				{
					destroy_stream_window();
					return false;
				}

				g_stream_x = pt.x;
				g_stream_y = pt.y;
				g_stream_w = stream_w;
				g_stream_h = stream_h;
			}

			MSG msg{};
			while (PeekMessageW(&msg, g_stream_hwnd, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}

			POINT pt{ 0, 0 };
			ClientToScreen(g_window, &pt);
			const bool fg = (GetForegroundWindow() == g_window);

			if (!fg)
			{
				hide_stream_window();
				return true;
			}

			if (!g_stream_visible || pt.x != g_stream_x || pt.y != g_stream_y
				|| stream_w != g_stream_w || stream_h != g_stream_h || fg != g_stream_topmost)
			{
				g_stream_x       = pt.x;
				g_stream_y       = pt.y;
				g_stream_w       = stream_w;
				g_stream_h       = stream_h;
				g_stream_topmost = fg;
				g_stream_visible = true;

				SetWindowPos(
					g_stream_hwnd,
					HWND_TOPMOST,
					pt.x, pt.y, stream_w, stream_h,
					SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);

				const MARGINS margins = { -1, -1, -1, -1 };
				DwmExtendFrameIntoClientArea(g_stream_hwnd, &margins);
			}

			return true;
		}

		/* ------------------------------------------------------------------ */
		/* the detour                                                          */
		/* ------------------------------------------------------------------ */

		bool init_gui_on_render_thread(HDC dc, HGLRC host_ctx)
		{
			/* Settle first. Sodium and Iris tear down and rebuild their render
			   pipeline several times while connecting to a server; creating a
			   context and textures inside that window is a reliable crash.
			   Roughly a second at 60fps is enough for it to stabilise. */
			if (g_settle < 60)
			{
				++g_settle;
				return false;
			}

			g_our_ctx = create_overlay_context(dc, host_ctx);
			if (!g_our_ctx)
				return false;

			if (!wglMakeCurrent(dc, g_our_ctx))
			{
				slog::log::error("gl: wglMakeCurrent on the overlay context failed");
				wglDeleteContext(g_our_ctx);
				g_our_ctx = nullptr;
				return false;
			}

			quiet_debug_output();

			p_glBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(
				wglGetProcAddress("glBindFramebuffer"));
			p_glBlitFramebuffer = reinterpret_cast<PFN_glBlitFramebuffer>(
				wglGetProcAddress("glBlitFramebuffer"));
			p_wglSwapIntervalEXT = reinterpret_cast<PFN_wglSwapIntervalEXT>(
				wglGetProcAddress("wglSwapIntervalEXT"));
			if (!p_glBindFramebuffer)
				slog::log::warning("gl: glBindFramebuffer unavailable — the panel may be "
					"drawn into a shader pack's offscreen target");

			IMGUI_CHECKVERSION();
			ImGui::CreateContext();

			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;

			ImGui::StyleColorsDark();
			{
				ImGuiStyle& style = ImGui::GetStyle();
				style.AntiAliasedLines = true;
				style.AntiAliasedFill  = true;
				style.AntiAliasedLinesUseTex = false;
				style.CircleTessellationMaxError = 0.10f;
				style.CurveTessellationTol       = 0.65f;
			}

			if (!ImGui_ImplWin32_Init(g_window))
			{
				slog::log::error("gl: ImGui_ImplWin32_Init failed");
				return false;
			}

			/* "#version 130" keeps the backend on a GLSL the compatibility
			   context is guaranteed to accept. */
			if (!ImGui_ImplOpenGL3_Init("#version 130"))
			{
				slog::log::error("gl: ImGui_ImplOpenGL3_Init failed");
				return false;
			}

			if (!dt::g_app->initialize())
			{
				slog::log::error("gl: app initialize failed");
				return false;
			}

			slog::log::success("gl: overlay ready inside the game frame");
			return true;
		}

		void shutdown_gui_on_render_thread(HDC dc, HGLRC host_ctx)
		{
			if (g_gui_ready && (!g_our_ctx || !wglMakeCurrent(dc, g_our_ctx))) {
				dt::blockUnload();
				return;
			}
			if (g_gui_ready && g_our_ctx && wglMakeCurrent(dc, g_our_ctx))
			{
				g_stream_fbo.destroy();
				try { dt::g_app->shutdown(); } catch (...) { dt::blockUnload(); return; }
				ImGui_ImplOpenGL3_Shutdown();
				ImGui_ImplWin32_Shutdown();
				ImGui::DestroyContext();
			}

			if (host_ctx && wglMakeCurrent(dc, host_ctx)) {
				dt::modules::visualports::shutdownGl();
				dt::modules::itemshader::shutdown_gl();
			}

			if (g_our_ctx)
			{
				wglDeleteContext(g_our_ctx);
				g_our_ctx = nullptr;
			}
			destroy_stream_window();
			g_gui_ready = false;
		}

		BOOL WINAPI hk_wglSwapBuffers(HDC dc)
		{
			g_in_detour.fetch_add(1, std::memory_order_acquire);
			struct leave_t { ~leave_t() { g_in_detour.fetch_sub(1, std::memory_order_release); } } leave;
			if (!o_wglSwapBuffers) return TRUE;
			std::unique_lock state(dt::clientStateMutex());
			auto present = [&] { state.unlock(); return o_wglSwapBuffers(dc); };
			if (g_stream_dc && dc == g_stream_dc) return present();

			HGLRC host_ctx = wglGetCurrentContext();

			/* Cooperative teardown. GL objects must die on the thread that owns
			   the context, so the unloading thread raises a flag and waits here
			   rather than destroying anything itself. Must run BEFORE any module
			   pump so no JNI/GL work is re-armed while shutdown() is tearing down. */
			if (g_shutdown_requested.load(std::memory_order_acquire)
				&& !g_shutdown_done.load(std::memory_order_acquire))
			{
				if (!dt::safeToUnload()) return present();
				// Keep presenting until the JVM has restored its original visual
				// resources, before retiring native callbacks and the DLL image.
				if (!dt::modules::visualports::prepareShutdown()) return present();
				// Restore every Java hook before module-owned JNI caches are freed.
				// Pending callbacks may dispatch events, so never drain with the
				// state mutex held.
				state.unlock();
				const auto restored = JNIHook_Shutdown();
				state.lock();
				if (restored != JNIHOOK_OK) {
					dt::blockUnload();
					return present();
				}
				shutdown_gui_on_render_thread(dc, host_ctx);
				if (!dt::safeToUnload()) return present();
				g_shutdown_done.store(true, std::memory_order_release);
				return present();
			}

			if (g_shutdown_done.load(std::memory_order_acquire))
				return present();
			if (!dt::entry::bootstrapFinished()) return present();

			/* Drive the game-side modules first, before any GL work and
			   regardless of whether the gui has initialised. This thread is
			   Minecraft's render thread, which in 1.21 is also the thread that
			   runs the game loop — so it is the correct place to call into the
			   game over JNI. */
			::dt::KeybindManager::get().dispatchQueuedBinds();
			::dt::core::pumpGameFrame();
			::dt::ConfigManager::get().pumpAutosave();

			/* Arm the chams GL layer here, and ONLY here: wglGetProcAddress needs a
			   current context, and this is the one point in the frame where the
			   GAME's context is guaranteed current — which is the context whose
			   entry points must be resolved. Idempotent, and a cheap atomic read
			   once installed. */
			::dt::modules::chams::tick_game_context();
			update_spotify_chat();

			/* Spectate re-applies EVERY frame, and here rather than in an
			   overlay pass: the camera must keep following whoever you picked
			   with the ClickGui closed, and the free-look rotation it writes is
			   only true for the frame it is written in. */
			{
				::dt::jvm::JniScope spec_scope;
				if (spec_scope)
					::dt::modules::spectate::tick(spec_scope.get());
			}

			if (!g_gui_ready)
			{
				g_gui_ready = init_gui_on_render_thread(dc, host_ctx);
				if (host_ctx) wglMakeCurrent(dc, host_ctx);
				if (!g_gui_ready)
					return present();
			}

			/* Synchronized fallback poll for the toggle hotkey on the render thread:
			   handles left/right modifiers (e.g. VK_RSHIFT) or special keys even if
			   Win32 message translation mapped wParam to a generic VK, while sharing
			   g_toggle_key_down with hooked_wndproc so it never double-toggles. */
			{
				const int toggle_vk = g_toggle_vk.load(std::memory_order_relaxed);
				if (!dt::vgui::bridge::isMenuKeyBinding())
				{
					const bool phys_down = (toggle_vk > 0 && (GetAsyncKeyState(toggle_vk) & 0x8000) != 0)
						|| (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0
						|| (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
					if (!phys_down)
					{
						g_toggle_key_down.store(false, std::memory_order_relaxed);
					}
					else if (dt::g_app && dt::g_app->visible() && dt::g_app->text_focus())
					{
						g_toggle_key_down.store(true, std::memory_order_relaxed);
					}
					else if (dt::g_app && (!g_window || GetForegroundWindow() == g_window))
					{
						const ULONGLONG now_tick = GetTickCount64();
						if ((now_tick - g_toggle_rebind_tick.load(std::memory_order_relaxed) > 200)
							&& !g_toggle_key_down.exchange(true, std::memory_order_relaxed))
						{
							dt::vgui::toggleVisible();
						}
					}
				}
			}

			/* The size of the default framebuffer we are about to present, which
			   is by definition the window's client area.

			   It is NOT glGetIntegerv(GL_VIEWPORT). The viewport is merely
			   whatever the game happened to set last, and with Sodium or Iris
			   that is routinely a post-processing pass at some other resolution.
			   Worse, the two disagreeing is silent and asymmetric: the layout
			   below would use the viewport while ImGui_ImplWin32_NewFrame sets
			   io.DisplaySize — and therefore the scissor rects — from the client
			   rect, so the panel got placed against one size and clipped against
			   another. That is what cut a strip off the gui in fullscreen. */
			int fb_w = 0, fb_h = 0;
			{
				RECT rc{};
				if (g_window && GetClientRect(g_window, &rc))
				{
					fb_w = rc.right - rc.left;
					fb_h = rc.bottom - rc.top;
				}
			}

			/* Only if the window gives us nothing usable (mid-resize, minimised)
			   is the viewport worth guessing from. */
			GLint vp[4] = {};
			glGetIntegerv(GL_VIEWPORT, vp);
			if (fb_w <= 0 || fb_h <= 0)
			{
				fb_w = vp[2];
				fb_h = vp[3];
			}

			if (fb_w <= 0 || fb_h <= 0)
				return present();

			g_fb_w.store(fb_w, std::memory_order_relaxed);
			g_fb_h.store(fb_h, std::memory_order_relaxed);

			const bool want_stream_proof = g_stream_proof.load(std::memory_order_acquire);
			if (!want_stream_proof && g_stream_hwnd)
				destroy_stream_window();

			/* Drive the cursor guard from the panel's ACTUAL visibility, every
			   frame, rather than from the toggle key.

			   Keying it to the toggle left two holes: the panel comes up
			   visible on inject without the toggle ever being pressed, so the
			   pointer stayed captured until you pressed INSERT twice; and
			   Escape hides the panel through a different path, which left the
			   pointer free while you were trying to play. set_active early-outs
			   when nothing changed, so this only does work on a transition. */
			cursor_guard::set_active(dt::g_app && dt::g_app->visible());

			if (g_our_ctx && wglMakeCurrent(dc, g_our_ctx))
			{
				/* Everything below runs on DT's context, not the game's.

				   The chams module patches the driver's own draw, program and texture
				   stubs to classify Minecraft's entity draws. ImGui's GL3 backend and
				   every blur pass in this client resolve their entry points through
				   the same wglGetProcAddress, so they call the IDENTICAL stubs — every
				   draw the panel makes would otherwise be fed to that classifier and
				   would corrupt its current-program and bound-texture tracking.

				   Raising the flag here is not an optimisation, it is what keeps the
				   client's own rendering out of the game's bookkeeping. Lowered again
				   once the host context is restored, so it also covers the panel's
				   glass callback, the draw-list reorder and the inject sequence. */
				::dt::modules::chams::gl::set_suppressed(true);

				if (!want_stream_proof && g_stream_fbo.framebuffer_id() != 0)
					g_stream_fbo.destroy();

				const bool use_stream_fbo = want_stream_proof
					&& p_glBindFramebuffer != nullptr
					&& p_glBlitFramebuffer != nullptr
					&& ensure_stream_window(dc, fb_w, fb_h)
					&& g_stream_fbo.ensure(fb_w, fb_h);

				reset_our_state(fb_w, fb_h, use_stream_fbo ? g_stream_fbo.framebuffer_id() : 0);
				if (use_stream_fbo)
				{
					glClearColor(0.f, 0.f, 0.f, 0.f);
					glClear(GL_COLOR_BUFFER_BIT);
				}


				/* Rebake the font atlas at the on-screen scale before NewFrame runs. Once
				   NewFrame runs Fonts->Locked is true and the atlas can't be rebuilt without
				   crashing the current frame. The last frame filed the request via
				   set_pending_atlas_scale; here is the only safe window to service it. */
				if (dt::g_fonts)
					dt::g_fonts->apply_pending_atlas_scale();

				/* ArrayList background: raw-GL shader pipeline (dual-Kawase +
				   composite) drawn directly to the currently bound framebuffer
				   BEFORE any ImGui state exists this frame. The text pass
				   happens further down, inside the ImGui frame, on the
				   foreground draw list that the panel-scale transform skips. */
				{
					GLint fb_binding = 0;
					glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fb_binding);

					/* First of everything: the Outline belongs to the world, not
					   to the HUD. Drawn here it is part of the scene the glass
					   panes below then blur, which is what a light in the world
					   does to a sheet of frosted glass held in front of it. */
					dt::modules::outline::render_background(
						fb_w, fb_h, static_cast<unsigned int>(fb_binding));

					dt::modules::arraylist::render_background(
						fb_w, fb_h, static_cast<unsigned int>(fb_binding));
                    dt::modules::keybindhud::render_background(fb_w, fb_h, static_cast<unsigned int>(fb_binding));
					dt::modules::watermark::render_background(
						fb_w, fb_h, static_cast<unsigned int>(fb_binding));
					dt::modules::spotify::render_background(
						fb_w, fb_h, static_cast<unsigned int>(fb_binding));
					dt::modules::targethud::render_background(
						fb_w, fb_h, static_cast<unsigned int>(fb_binding));
					dt::modules::radar::render_background(
						fb_w, fb_h, static_cast<unsigned int>(fb_binding));

					/* Last, because each of these composites onto the same
					   framebuffer the next one then blurs — so a notification
					   passing over another element's pane picks that pane up
					   in its own blur, which is what a sheet of glass sliding
					   over another sheet of glass actually does. */
					dt::modules::notifier::render_background(
						fb_w, fb_h, static_cast<unsigned int>(fb_binding));
				}

				ImGui_ImplOpenGL3_NewFrame();
				ImGui_ImplWin32_NewFrame();

				/* Tell ImGui the display is the CANVAS, not the game window.

				   This looks wrong and is the crux of the whole placement
				   scheme. The views draw in a fixed 1120x700 space and are
				   mapped into the framebuffer afterwards, but ImGui does not
				   know that: it seeds every draw list's clip stack with
				   ImRect(0, 0, DisplaySize) and intersects every PushClipRect
				   against it. With DisplaySize set to the game window, a canvas
				   clip reaching x=1108 was cut to the window width FIRST, in
				   canvas units, and only then scaled down — so the panel lost
				   its right-hand side on any window narrower than 1120, and its
				   bottom on any window shorter than 700.

				   That is one bug wearing two hats: "cut in fullscreen" and
				   "cut when the window is made narrower". Setting the display to
				   the canvas lets the clips survive intact; DisplaySize is put
				   back to the real framebuffer on the draw data below, after the
				   vertices have been transformed into it. */
				ImGuiIO& io = ImGui::GetIO();
				io.DisplaySize = ImVec2(static_cast<float>(dt::c_app::k_window_width),
				                        static_cast<float>(dt::c_app::k_window_height));

				ImGui::NewFrame();

				dt::g_app->frame();

				/* ArrayList text: queues the module names on ImGui's foreground
				   draw list. The transform loop below detects the foreground
				   list by owner name and skips it, so these vertices stay in
				   framebuffer pixels. */
				dt::modules::arraylist::render_text(fb_w, fb_h);
				dt::modules::watermark::render_text(fb_w, fb_h);
                dt::modules::keybindhud::render_text(fb_w, fb_h);
				dt::modules::spotify::render_text(fb_w, fb_h);
				dt::modules::hitfx::render_text(fb_w, fb_h);
				dt::modules::chams::render_text(fb_w, fb_h);
				dt::modules::esp::render_text(fb_w, fb_h);
				dt::modules::backtrack::render(fb_w, fb_h);
                dt::modules::blockesp::render_text(fb_w, fb_h);
                dt::modules::worldtweaks::render_text(fb_w, fb_h);
				dt::modules::targetesp::render(fb_w, fb_h);
                dt::modules::triggerbot::render_focus_visual(fb_w, fb_h);
				dt::modules::targethud::render_text(fb_w, fb_h);
				dt::modules::radar::render_text(fb_w, fb_h);

				/* Last of the HUD passes: a notification is the one thing on
				   screen that is allowed to sit over the others. */
				dt::modules::notifier::render_text(fb_w, fb_h);

				/* The inject sequence, last of everything.

				   It is a splash: for the few seconds it runs it IS the screen, so
				   nothing may be punched through it. The HUD passes above share
				   this list, and whoever queues last wins — hence last. The panel
				   used to be safe for free, being on the background list, and is
				   not any more: the reorder after ImGui::Render() drops this whole
				   list underneath it, which is why that reorder stands down while
				   the sequence is running. Ticked here too, so the sequence keeps
				   its own clock whether or not the panel happens to be visible. */
				dt::intro::tick(ImGui::GetIO().DeltaTime);
				dt::intro::render(fb_w, fb_h);

				ImGui::Render();

				/* Place the canvas inside the game's framebuffer by
				   transforming the finished draw data.

				   The views draw in a fixed 1120x700 space and know nothing
				   about where the panel sits or how large it is. Scaling the
				   vertices here — rather than threading an offset and a scale
				   through every widget — is what let the entire existing gui
				   port across unchanged. */
				if (ImDrawData* dd = ImGui::GetDrawData())
				{
					/* @the HUD goes UNDER the panel, and the panel's own glass
					   blurs it.

					   ImGui hands RenderDrawData an array and the backend walks
					   it in order, so the index IS the z-order: background list,
					   then windows, then foreground. Every HUD element draws on
					   the foreground list -- it has to, because that is the one
					   list the canvas transform below skips -- so the arraylist,
					   the radar and the rest were painting straight over an open
					   panel.

					   Moving that list to the front fixes more than the
					   overlap. The panel's frosted backing is not geometry: it is
					   a user callback that runs midway through RenderDrawData and
					   copies whatever is in the framebuffer at that moment
					   (panel_glass.cc). Emitting the HUD first therefore puts it
					   INSIDE that copy, so it comes back blurred underneath the
					   panel instead of merely being covered by it -- which is
					   what a sheet of glass over a HUD should look like.

					   Only while the panel is up, and never during the inject
					   sequence: that is a splash, it owns the screen for the few
					   seconds it runs, and it draws on this same list. Demoting it
					   would let the panel punch a hole through it.

					   The count test is the third condition and it earns its
					   place: it asks "did anything paint on a non-foreground list
					   this frame". On the HUD editor page the panel is not painted
					   at all, so the foreground list is the only one there is and
					   there is nothing to go behind. */
					if (dt::g_app->visible() && !dt::intro::active()
						&& dd->CmdListsCount > 1)
					{
						for (int n = 0; n < dd->CmdListsCount; ++n)
						{
							ImDrawList* list = dd->CmdLists[n];

							if (list->_OwnerName == nullptr
								|| std::strcmp(list->_OwnerName, "##Foreground") != 0)
								continue;

							for (int m = n; m > 0; --m)
								dd->CmdLists[m] = dd->CmdLists[m - 1];

							dd->CmdLists[0] = list;
							break;
						}
					}

					const float s  = dt::g_app->panel_scale();
					const float ox = dt::g_app->panel_x();
					const float oy = dt::g_app->panel_y();

					/* @the open / close animation.

					   Applied to the finished vertices rather than threaded through
					   the views, for the same reason the canvas placement is: this
					   is one loop over one buffer, and the alternative is teaching a
					   hundred draw calls about a value none of them care about.

					   The panel grows the last 4% into place as it fades in, about
					   its own centre so it expands rather than drifts. Both curves
					   are eased so neither the size nor the opacity arrives at a
					   constant rate. */
					const float reveal = dt::g_app->reveal();
					const float travel = dt::g_app->open_travel();

					/* Both clocks, because the alpha now finishes long before
					   the geometry does — testing only the alpha would stop
					   transforming a panel that is still moving. */
					const bool  animating = reveal < 0.999f || travel < 0.9995f;

					const float grow = dt::g_app->open_grow();
					const float rise = dt::g_app->open_rise() * s;   /* canvas px -> fb */

					const float pw = static_cast<float>(dt::c_app::k_window_width) * s;
					const float ph = static_cast<float>(dt::c_app::k_window_height) * s;

					const float centre_x = ox + dt::g_app->pivot_x() * s;
					const float centre_y = oy + dt::g_app->pivot_y() * s;

					for (int n = 0; n < dd->CmdListsCount; ++n)
					{
						ImDrawList* list = dd->CmdLists[n];

						/* The arraylist draws text on the foreground list
						   using framebuffer-space coordinates directly, so
						   the panel-scale transform must NOT apply to it —
						   otherwise the HUD would ride the panel around and
						   shrink with it. Identify the list by owner name
						   (ImGui sets "##Foreground" when it creates it in
						   GetViewportBgFgDrawList). */
						if (list->_OwnerName != nullptr
							&& std::strcmp(list->_OwnerName, "##Foreground") == 0)
							continue;

						for (int v = 0; v < list->VtxBuffer.Size; ++v)
						{
							ImDrawVert& vertex = list->VtxBuffer[v];

							vertex.pos.x = vertex.pos.x * s + ox;
							vertex.pos.y = vertex.pos.y * s + oy;

							if (!animating)
								continue;

							vertex.pos.x = centre_x + (vertex.pos.x - centre_x) * grow;
							vertex.pos.y = centre_y + (vertex.pos.y - centre_y) * grow + rise;

							const unsigned int a = std::min(255u, static_cast<unsigned int>(
								((vertex.col >> IM_COL32_A_SHIFT) & 0xFF) * reveal));

							vertex.col = (vertex.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
						}

						/* @the clip rects take the transform TOO.

						   They only ever got the scale and the offset. That was
						   invisible while the geometry deviated four percent at
						   an alpha already near zero — but the alpha finishes at
						   55% of the travel now, so the panel is fully opaque
						   while it is still six percent small and ten pixels
						   high, and a clip rect left behind would shear the
						   content off its own card for the rest of the entrance. */
						for (int c = 0; c < list->CmdBuffer.Size; ++c)
						{
							ImVec4& r = list->CmdBuffer[c].ClipRect;
							r.x = r.x * s + ox;  r.y = r.y * s + oy;
							r.z = r.z * s + ox;  r.w = r.w * s + oy;

							if (!animating)
								continue;

							r.x = centre_x + (r.x - centre_x) * grow;
							r.z = centre_x + (r.z - centre_x) * grow;
							r.y = centre_y + (r.y - centre_y) * grow + rise;
							r.w = centre_y + (r.w - centre_y) * grow + rise;
						}
					}

					/* Everything is in framebuffer space now, so the draw data
					   must describe the framebuffer — the GL backend builds both
					   its ortho projection and its scissor rects from these. */
					dd->DisplayPos  = ImVec2(0.f, 0.f);
					dd->DisplaySize = ImVec2(static_cast<float>(fb_w), static_cast<float>(fb_h));
				}

				ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

				if (use_stream_fbo && g_stream_dc && g_stream_visible && g_stream_w > 0 && g_stream_h > 0)
				{
					p_glBindFramebuffer(kFramebuffer, 0);
					if (wglMakeCurrent(g_stream_dc, g_our_ctx))
					{
						if (p_wglSwapIntervalEXT && !g_stream_vsync_disabled)
						{
							p_wglSwapIntervalEXT(0);
							g_stream_vsync_disabled = true;
						}

						const int sw    = std::min(fb_w, g_stream_w);
						const int sh    = std::min(fb_h, g_stream_h);
						const int src_y = std::max(0, fb_h - sh);

						glDisable(GL_SCISSOR_TEST);
						glDisable(kFramebufferSrgb);
						p_glBindFramebuffer(kReadFramebuffer, g_stream_fbo.framebuffer_id());
						p_glBindFramebuffer(kDrawFramebuffer, 0);
						glDrawBuffer(GL_BACK);
						glViewport(0, 0, sw, sh);
						glClearColor(0.f, 0.f, 0.f, 0.f);
						glClear(GL_COLOR_BUFFER_BIT);
						p_glBlitFramebuffer(
							0, src_y, sw, src_y + sh,
							0, 0,     sw, sh,
							GL_COLOR_BUFFER_BIT, GL_NEAREST);
						p_glBindFramebuffer(kFramebuffer, 0);

						while (glGetError() != GL_NO_ERROR) {}
						o_wglSwapBuffers(g_stream_dc);
					}
				}

				/* Never let an error we caused reach the game's context: with
				   Sodium's synchronous debug output that is an in-driver abort. */
				while (glGetError() != GL_NO_ERROR) {}

				if (host_ctx) wglMakeCurrent(dc, host_ctx);

				/* Back on the game's context — the classifier may look again
				   only when not in StreamProof mode (3D Chams draws directly
				   into the game's framebuffer). */
				::dt::modules::chams::gl::set_suppressed(want_stream_proof);
			}

			return present();
		}
	}

	bool install(HWND game_window)
	{
		if (g_installed.load()) return true;
		if (!game_window) return false;

		g_window = game_window;

		const MH_STATUS init = MH_Initialize();
		if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
		{
			slog::log::error("gl: MH_Initialize failed ({})", static_cast<int>(init));
			return false;
		}

		HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
		if (!opengl32)
		{
			slog::log::error("gl: opengl32.dll is not loaded");
			return false;
		}

		void* wgl_proc = reinterpret_cast<void*>(GetProcAddress(opengl32, "wglSwapBuffers"));
		if (!wgl_proc)
		{
			slog::log::error("gl: wglSwapBuffers export not found");
			return false;
		}

		// IAT hook: find the IAT cell in any loaded module that points to wglSwapBuffers,
		// patch the pointer there. opengl32.dll's own code bytes stay untouched.
		void** iat_cell = nullptr;
		{
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			MODULEENTRY32W me{};
			me.dwSize = sizeof(me);
			if (snap != INVALID_HANDLE_VALUE && Module32FirstW(snap, &me))
			{
				do {
					auto* base = reinterpret_cast<BYTE*>(me.modBaseAddr);
					auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
					if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
					auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
					if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
					auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
					if (!imp_dir.VirtualAddress || !imp_dir.Size) continue;
					auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imp_dir.VirtualAddress);
					for (; desc->Name; ++desc)
					{
						const char* mod_name = reinterpret_cast<const char*>(base + desc->Name);
						if (_stricmp(mod_name, "opengl32.dll") != 0) continue;
						auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
						for (; thunk->u1.Function; ++thunk)
						{
							if (reinterpret_cast<void*>(thunk->u1.Function) == wgl_proc)
							{
								iat_cell = reinterpret_cast<void**>(&thunk->u1.Function);
								break;
							}
						}
						if (iat_cell) break;
					}
					if (iat_cell) break;
				} while (Module32NextW(snap, &me));
				CloseHandle(snap);
			}
		}

		if (!iat_cell)
		{
			// No IAT entry found — fall back to prologue hook as last resort
			// but log the detection risk clearly.
			slog::log::warning("gl: no IAT entry for wglSwapBuffers found; using prologue hook (detection risk)");
			if (MH_CreateHook(wgl_proc, reinterpret_cast<void*>(&hk_wglSwapBuffers),
					reinterpret_cast<void**>(&o_wglSwapBuffers)) != MH_OK
				|| MH_EnableHook(wgl_proc) != MH_OK)
			{
				slog::log::error("gl: failed to hook wglSwapBuffers");
				return false;
			}
			g_swap_target  = wgl_proc;
			g_swap_is_iat  = false;
		}
		else
		{
			// IAT hook: patch the pointer, no prologue modification
			DWORD old_protect = 0;
			VirtualProtect(iat_cell, sizeof(void*), PAGE_READWRITE, &old_protect);
			o_wglSwapBuffers = reinterpret_cast<PFN_wglSwapBuffers>(*iat_cell);
			*iat_cell = reinterpret_cast<void*>(&hk_wglSwapBuffers);
			VirtualProtect(iat_cell, sizeof(void*), old_protect, &old_protect);
			g_swap_target = reinterpret_cast<void*>(iat_cell);  // store IAT cell ptr
			g_swap_is_iat = true;
			slog::log::success("gl: IAT-hooked wglSwapBuffers (opengl32.dll prologue untouched)");
		}

		// A key already held while injecting is not a new press. The render
		// poll or key-up clears this latch once the configured key is released.
		g_toggle_key_down.store(true, std::memory_order_relaxed);
		g_orig_wndproc = reinterpret_cast<WNDPROC>(
			SetWindowLongPtrW(g_window, GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(&hooked_wndproc)));

		cursor_guard::install();

		g_installed.store(true);
		char hwnd_hex[24];
		std::snprintf(hwnd_hex, sizeof(hwnd_hex), "0x%p", static_cast<void*>(g_window));
		slog::log::success("gl: hooked wglSwapBuffers, subclassed HWND {}", hwnd_hex);
		return true;
	}

	void request_shutdown()
	{
		if (!g_installed.load()) return;

		g_shutdown_requested.store(true, std::memory_order_release);

		// The render thread does the actual teardown; wait for it, bounded.
		for (int i = 0; i < 250 && !g_shutdown_done.load(std::memory_order_acquire); ++i)
			Sleep(20);

		if (!g_shutdown_done.load(std::memory_order_acquire))
			slog::log::warning("gl: render thread did not acknowledge shutdown");
	}

	void uninstall()
	{
		if (!g_installed.load()) return;
		if (!g_shutdown_done.load(std::memory_order_acquire)) return;

		if (g_window && g_orig_wndproc)
		{
			SetLastError(0);
			const auto previous = SetWindowLongPtrW(g_window, GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(g_orig_wndproc));
			if (!previous && GetLastError() != 0) { dt::blockUnload(); return; }
			for (int spins = 0; g_in_wndproc.load() > 0 && spins < 2000; ++spins) Sleep(1);
			if (g_in_wndproc.load() > 0) { dt::blockUnload(); return; }
			g_orig_wndproc = nullptr;
		}

		if (g_stream_hwnd && IsWindow(g_stream_hwnd))
		{
			// DestroyWindow causes an immediate synchronous destruction of
			// the window on the calling thread (the render thread, which is
			// also the UI thread in MC 1.21). SendMessage WM_CLOSE was the
			// previous approach and relied on the window's WndProc posting
			// a delayed close, which left a brief window where the HWND was
			// still alive and visible to any enumerator.
			DestroyWindow(g_stream_hwnd);
			// Pump any pending messages so the destruction completes fully
			// before we null the handle and unregister the class.
			MSG m{};
			for (int i = 0; i < 200 && IsWindow(g_stream_hwnd); ++i) {
				PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE);
				Sleep(1);
			}
			g_stream_hwnd = nullptr;
		}
		if (g_stream_atom)
		{
			UnregisterClassW(stream_class_name(), GetModuleHandleW(nullptr));
			g_stream_atom = 0;
		}

		if (g_swap_target)
		{
			if (g_swap_is_iat)
			{
				// IAT hook: restore the original pointer in the IAT cell
				void** iat_cell = reinterpret_cast<void**>(g_swap_target);
				DWORD old_protect = 0;
				VirtualProtect(iat_cell, sizeof(void*), PAGE_READWRITE, &old_protect);
				*iat_cell = reinterpret_cast<void*>(o_wglSwapBuffers);
				VirtualProtect(iat_cell, sizeof(void*), old_protect, &old_protect);
				g_swap_is_iat  = false;
				g_swap_target  = nullptr;
			}
			else
			{
				const auto disabled = MH_DisableHook(g_swap_target);
				if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) {
					dt::blockUnload();
					return;
				}

				// Drain: MH_RemoveHook frees the trampoline, and a thread still
				// executing inside the detour would return into unmapped memory.
				for (int spins = 0; g_in_detour.load(std::memory_order_acquire) > 0 && spins < 2000; ++spins)
					Sleep(1);

				if (g_in_detour.load(std::memory_order_acquire) > 0) {
					dt::blockUnload();
					return;
				}
				if (MH_RemoveHook(g_swap_target) != MH_OK) {
					dt::blockUnload();
					return;
				}
				g_swap_target = nullptr;
			}
		}

		o_wglSwapBuffers = nullptr;
		cursor_guard::remove();
		MH_Uninitialize();
		g_installed.store(false);
	}

	void set_hotkeys(int toggle_vk, int panic_vk)
	{
		const int prev = g_toggle_vk.exchange(toggle_vk, std::memory_order_relaxed);
		g_panic_vk.store(panic_vk, std::memory_order_relaxed);
		if (prev != toggle_vk)
		{
			g_toggle_key_down.store(true, std::memory_order_relaxed);
			g_toggle_rebind_tick.store(GetTickCount64(), std::memory_order_relaxed);
		}
	}

	bool ready() { return g_gui_ready.load(); }
	bool installed() { return g_installed.load(); }

	void set_input_filter(int level) { g_input_filter.store(level, std::memory_order_relaxed); }
	int  input_filter()              { return g_input_filter.load(std::memory_order_relaxed); }

	/* Freecam capture; see g_freecam_capture. Declared for the bridge by a
	   local prototype block in bridge.cc -- the signatures must match these. */
	void set_freecam_capture(bool on) { g_freecam_capture.store(on, std::memory_order_relaxed); }
	int  take_freecam_wheel()         { return g_freecam_wheel_raw.exchange(0, std::memory_order_relaxed); }

	void framebuffer_size(int& w, int& h)
	{
		w = g_fb_w.load(std::memory_order_relaxed);
		h = g_fb_h.load(std::memory_order_relaxed);
	}

	void set_stream_proof(bool enabled)
	{
		g_stream_proof.store(enabled, std::memory_order_release);
	}

	bool stream_proof()
	{
		return g_stream_proof.load(std::memory_order_acquire);
	}
}
