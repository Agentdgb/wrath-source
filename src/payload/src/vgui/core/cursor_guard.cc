#include "../includes.hh"

#include "cursor_guard.hh"
#include "core/UnloadSafety.h"

#include <MinHook.h>
#include <atomic>

namespace core::cursor_guard
{
	namespace
	{
		using PFN_SetCursorPos       = BOOL(WINAPI*)(int, int);
		using PFN_ClipCursor         = BOOL(WINAPI*)(const RECT*);
		using PFN_ShowCursor         = int (WINAPI*)(BOOL);
		using PFN_SetCursor          = HCURSOR(WINAPI*)(HCURSOR);
		using PFN_GetRawInputData    = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
		using PFN_GetRawInputBuffer  = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);

		PFN_SetCursorPos      o_SetCursorPos      = nullptr;
		PFN_ClipCursor        o_ClipCursor        = nullptr;
		PFN_ShowCursor        o_ShowCursor        = nullptr;
		PFN_SetCursor         o_SetCursor         = nullptr;
		PFN_GetRawInputData   o_GetRawInputData   = nullptr;
		PFN_GetRawInputBuffer o_GetRawInputBuffer = nullptr;

		constexpr int k_hooks = 6;

		/* Each hook is tracked individually. A previous version stored a single
		   (a || b) flag, so an install where only ShowCursor took would report
		   "not installed" and remove() would leave a live detour pointing into
		   the DLL image — which then became a wild jump the moment F8 unloaded
		   us. */
		void*  g_target[k_hooks] = {};
		bool   g_hooked[k_hooks] = {};

		std::atomic<bool> g_active{ false };
		std::atomic<bool> g_installed{ false };

		/* Number of detour bodies currently executing. remove() waits for this
		   to drain before unhooking, otherwise MinHook can free a trampoline
		   out from under a thread that is inside it. */
		std::atomic<int> g_in_detour{ 0 };

		/* Set once, on the render thread, so a swallowed SetCursor(nullptr) can
		   be answered with something rather than nothing. */
		HCURSOR g_arrow = nullptr;

		/* Where the game had the pointer when the panel opened, so it can be
		   handed back exactly there. */
		POINT g_resume_at{};
		bool  g_have_resume = false;

		/* Windows' ShowCursor keeps a display counter: >= 0 visible, < 0 hidden.
		   We swallow the game's hides, so we must both (a) report a plausible
		   decreasing value, and (b) remember how many hides we ate in order to
		   hand the counter back untouched when the panel closes. Leaking this
		   would leave the cursor permanently visible (or invisible) in-game. */
		std::atomic<int> g_swallowed_hides{ 0 };

		struct detour_scope_t
		{
			detour_scope_t()  { g_in_detour.fetch_add(1, std::memory_order_acquire); }
			~detour_scope_t() { g_in_detour.fetch_sub(1, std::memory_order_release); }
		};

		/* No thread exclusion any more. It existed because the gui used to run on
		   a thread of its own and had to be able to move its own cursor; the gui
		   now runs on the game's render thread, which is the very thread GLFW
		   makes these calls from, so excluding it would disable the guard
		   entirely. Our own cursor work goes through the o_* trampolines below,
		   which bypass the detours regardless of thread. */
		bool suppressing()
		{
			return g_active.load(std::memory_order_acquire);
		}

		BOOL WINAPI hkSetCursorPos(int x, int y)
		{
			detour_scope_t scope;

			// Swallow the game's re-centring. GLFW discards the return value of
			// this call, so TRUE is simply the honest "nothing went wrong".
			if (suppressing())
				return TRUE;

			return o_SetCursorPos ? o_SetCursorPos(x, y) : FALSE;
		}

		BOOL WINAPI hkClipCursor(const RECT* rect)
		{
			detour_scope_t scope;

			// Refuse only the confining calls. Releasing (nullptr) always goes
			// through so nothing can leave a stale clip behind.
			if (rect != nullptr && suppressing())
				return TRUE;

			return o_ClipCursor ? o_ClipCursor(rect) : FALSE;
		}

		int WINAPI hkShowCursor(BOOL show)
		{
			detour_scope_t scope;

			if (!o_ShowCursor)
				return 0;

			if (show || !suppressing())
				return o_ShowCursor(show);

			/* A suppressed hide. Returning a constant 0 would hang the very
			   common force-hide idiom
			       while (ShowCursor(FALSE) >= 0) {}
			   because the caller spins waiting for a negative value that never
			   arrives. Report a monotonically decreasing counter instead: the
			   loop terminates, the real counter is untouched, and we remember
			   how far we lied so set_active(false) can settle up. */
			const int swallowed = g_swallowed_hides.fetch_add(1, std::memory_order_relaxed) + 1;
			return -swallowed;
		}

		HCURSOR WINAPI hkSetCursor(HCURSOR cursor)
		{
			detour_scope_t scope;

			if (!o_SetCursor)
				return nullptr;

			/* THIS is how GLFW hides the pointer in CURSOR_DISABLED mode — not
			   ShowCursor. It calls SetCursor(nullptr) from its WM_SETCURSOR
			   handler, so every mouse move hid the pointer again a frame after
			   ImGui had drawn it: the cursor appeared and disappeared
			   continuously while the panel was open.

			   Only the hides are refused. Real cursors still go through, which
			   is what lets ImGui keep swapping in the text-beam and resize
			   shapes over the panel's own widgets. */
			if (cursor == nullptr && suppressing())
				return o_SetCursor(g_arrow ? g_arrow : LoadCursorW(nullptr, IDC_ARROW));

			return o_SetCursor(cursor);
		}

		/* ------------------------------------------------------------------ */
		/* raw input                                                           */
		/* ------------------------------------------------------------------ */

		/* Neutralising the deltas IN PLACE, rather than failing the call, is
		   deliberate. GLFW checks for failure and bails out early, which would
		   leave its own bookkeeping half-updated; handing it a successful read
		   that simply says "the mouse did not move" keeps it consistent. */
		void neutralise(RAWMOUSE& m)
		{
			/* Clearing MOUSE_MOVE_ABSOLUTE matters as much as zeroing the
			   deltas. In absolute mode — tablets, RDP sessions, some virtual
			   mice — lLastX/lLastY are a POSITION, not an increment, and GLFW
			   subtracts the previous one from it. Zeroing them there would read
			   as the pointer teleporting to the top-left corner and swing the
			   camera hard. Dropping the flag as well puts GLFW on its relative
			   branch, where zero honestly means "did not move". */
			m.usFlags      &= static_cast<USHORT>(~MOUSE_MOVE_ABSOLUTE);
			m.lLastX        = 0;
			m.lLastY        = 0;
			m.usButtonFlags = 0;
			m.usButtonData  = 0;
			m.ulRawButtons  = 0;
		}

		/* NEXTRAWINPUTBLOCK expands to a macro referencing QWORD, which is not
		   defined under every header configuration this file is built with. The
		   alignment is pointer-sized on both architectures. */
		RAWINPUT* next_block(RAWINPUT* p)
		{
			constexpr std::size_t align = sizeof(ULONG_PTR);
			const std::size_t stride = (p->header.dwSize + align - 1) & ~(align - 1);
			return reinterpret_cast<RAWINPUT*>(reinterpret_cast<BYTE*>(p) + stride);
		}

		UINT WINAPI hkGetRawInputData(HRAWINPUT ri, UINT command, LPVOID data,
			PUINT size, UINT header_size)
		{
			detour_scope_t scope;

			const UINT r = o_GetRawInputData
				? o_GetRawInputData(ri, command, data, size, header_size)
				: static_cast<UINT>(-1);

			/* THIS is how the camera still moved with the panel open. Minecraft
			   1.21 enables GLFW_RAW_MOUSE_MOTION, so GLFW takes its look deltas
			   from WM_INPUT via this call — not from WM_MOUSEMOVE, which the
			   window subclass was already swallowing. Blocking one and not the
			   other let every pointer movement over the gui turn the player. */
			if (r != static_cast<UINT>(-1) && command == RID_INPUT && data
				&& r >= sizeof(RAWINPUTHEADER) && suppressing())
			{
				auto* raw = static_cast<RAWINPUT*>(data);
				if (raw->header.dwType == RIM_TYPEMOUSE)
					neutralise(raw->data.mouse);
			}

			return r;
		}

		UINT WINAPI hkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header_size)
		{
			detour_scope_t scope;

			const UINT r = o_GetRawInputBuffer
				? o_GetRawInputBuffer(data, size, header_size)
				: static_cast<UINT>(-1);

			/* The batch API. Vanilla GLFW does not use it, but Feather- and
			   Lunar-style clients poll raw input this way and would otherwise
			   keep turning the camera. */
			if (r != static_cast<UINT>(-1) && r > 0 && data && suppressing())
			{
				RAWINPUT* raw = data;
				for (UINT i = 0; i < r; ++i)
				{
					if (raw->header.dwType == RIM_TYPEMOUSE)
						neutralise(raw->data.mouse);
					raw = next_block(raw);
				}
			}

			return r;
		}

		bool hook_one(int slot, const char* name, void* detour, void** original)
		{
			HMODULE user32 = GetModuleHandleA("user32.dll");
			if (!user32) return false;

			void* target = reinterpret_cast<void*>(GetProcAddress(user32, name));
			if (!target) return false;

			if (MH_CreateHook(target, detour, original) != MH_OK)
				return false;

			if (MH_EnableHook(target) != MH_OK)
			{
				MH_RemoveHook(target);
				return false;
			}

			g_target[slot] = target;
			g_hooked[slot] = true;
			return true;
		}
	}

	bool install()
	{
		if (g_installed.load()) return true;

		const MH_STATUS init = MH_Initialize();
		if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
		{
			slog::log::warning("cursor guard: MH_Initialize failed ({})", static_cast<int>(init));
			return false;
		}

		g_arrow = LoadCursorW(nullptr, IDC_ARROW);

		hook_one(0, "SetCursorPos", reinterpret_cast<void*>(&hkSetCursorPos), reinterpret_cast<void**>(&o_SetCursorPos));
		hook_one(1, "ClipCursor",   reinterpret_cast<void*>(&hkClipCursor),   reinterpret_cast<void**>(&o_ClipCursor));
		hook_one(2, "ShowCursor",   reinterpret_cast<void*>(&hkShowCursor),   reinterpret_cast<void**>(&o_ShowCursor));
		hook_one(3, "SetCursor",    reinterpret_cast<void*>(&hkSetCursor),    reinterpret_cast<void**>(&o_SetCursor));
		hook_one(4, "GetRawInputData",   reinterpret_cast<void*>(&hkGetRawInputData),   reinterpret_cast<void**>(&o_GetRawInputData));
		hook_one(5, "GetRawInputBuffer", reinterpret_cast<void*>(&hkGetRawInputBuffer), reinterpret_cast<void**>(&o_GetRawInputBuffer));

		// ANY successful hook must be tracked, or remove() will leave a live
		// detour behind at unload time.
		bool any = false;
		for (int i = 0; i < k_hooks; ++i) any = any || g_hooked[i];
		g_installed.store(any);

		if (!any)
			slog::log::warning("cursor guard: no hooks installed");
		else if (!g_hooked[0] || !g_hooked[1] || !g_hooked[3] || !g_hooked[4])
			slog::log::warning("cursor guard: partial install (SetCursorPos={}, ClipCursor={}, "
				"ShowCursor={}, SetCursor={}, RawData={}, RawBuffer={})",
				g_hooked[0], g_hooked[1], g_hooked[2], g_hooked[3], g_hooked[4], g_hooked[5]);
		else
			slog::log::success("cursor guard installed");

		return any;
	}

	void remove()
	{
		if (!g_installed.load()) return;

		// Stop suppressing first, and settle the display counter, so nothing
		// observes a half-removed guard.
		set_active(false);

		/* Disable before removing: MH_DisableHook restores the original bytes,
		   after which no new thread can enter a detour. Threads already inside
		   one are drained below — MinHook frees the trampoline in MH_RemoveHook,
		   and a thread still executing there would return into unmapped memory. */
		for (int i = 0; i < k_hooks; ++i) {
			if (!g_hooked[i] || !g_target[i]) continue;
			const auto result = MH_DisableHook(g_target[i]);
			if (result != MH_OK && result != MH_ERROR_DISABLED) {
				dt::blockUnload();
				return;
			}
		}

		for (int spins = 0; g_in_detour.load(std::memory_order_acquire) > 0 && spins < 2000; ++spins)
			Sleep(1);
		if (g_in_detour.load(std::memory_order_acquire) > 0) {
			dt::blockUnload();
			return;
		}

		for (int i = 0; i < k_hooks; ++i)
		{
			if (g_hooked[i] && g_target[i]) {
				const auto result = MH_RemoveHook(g_target[i]);
				if (result != MH_OK && result != MH_ERROR_NOT_CREATED) {
					dt::blockUnload();
					return;
				}
			}
			g_hooked[i] = false;
			g_target[i] = nullptr;
		}

		// Null the trampolines only after removal, so a late detour body that
		// slipped through calls a valid pointer rather than a freed one.
		o_SetCursorPos      = nullptr;
		o_ClipCursor        = nullptr;
		o_ShowCursor        = nullptr;
		o_SetCursor         = nullptr;
		o_GetRawInputData   = nullptr;
		o_GetRawInputBuffer = nullptr;
		g_installed.store(false);

		/* Deliberately NOT calling MH_Uninitialize: it is process-global and
		   would tear out hooks belonging to Discord, Steam or RTSS overlays
		   living in the same process. */
	}

	void set_active(bool active)
	{
		const bool was = g_active.exchange(active);
		if (was == active) return;

		if (active)
		{
			/* Remember where the game had the pointer. It is put back on the way
			   out: GLFW derives its look delta from "where the cursor is now
			   minus where it was last time it looked", and it has not looked
			   since the panel opened. Letting the panel close with the pointer
			   somewhere else makes that first delta the whole width of the
			   screen, and the camera snaps. */
			GetCursorPos(&g_resume_at);
			g_have_resume = true;

			// Break any clip the game already installed and make sure the
			// pointer is actually drawn.
			if (o_ClipCursor) o_ClipCursor(nullptr);
			if (o_ShowCursor) { int c = o_ShowCursor(TRUE); while (c < 0) c = o_ShowCursor(TRUE); }
			if (o_SetCursor && g_arrow) o_SetCursor(g_arrow);
			g_swallowed_hides.store(0, std::memory_order_relaxed);
		}
		else
		{
			/* Put the pointer back before the game looks again — see above. The
			   trampoline is used directly because g_active is already false but
			   ordering here should not matter to correctness. */
			if (g_have_resume && o_SetCursorPos)
				o_SetCursorPos(g_resume_at.x, g_resume_at.y);
			g_have_resume = false;

			/* Replay the hides we swallowed so the game's display counter ends
			   up exactly where it would have been. Without this the cursor
			   stays visible over the game after the panel closes. */
			int owed = g_swallowed_hides.exchange(0, std::memory_order_relaxed);
			while (owed-- > 0 && o_ShowCursor)
				o_ShowCursor(FALSE);
		}
	}

	bool active() { return g_active.load(std::memory_order_acquire); }
}
