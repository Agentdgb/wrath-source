# vendor/

Third-party sources checked in verbatim so the DLL builds with only what is in
this repo + a JDK + MSVC.

Drop the following into place before your first build (they are gitignored
above so the tree is small — the CMake targets expect them at these exact
paths):

| Path | What to drop | Source |
| --- | --- | --- |
| `vendor/imgui/`             | `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui.h`, `imconfig.h`, `imgui_internal.h`, `imstb_*.h`, plus `backends/imgui_impl_opengl3.{h,cpp}` and `backends/imgui_impl_win32.{h,cpp}` | Dear ImGui 1.90.x (docking branch is fine) |
| `vendor/json/nlohmann/json.hpp` | single-header nlohmann/json (any 3.11+) | https://github.com/nlohmann/json |
| `vendor/ankerl/unordered_dense.h` | single header | https://github.com/martinus/unordered_dense |
| `vendor/fonts/Inter.ttf`   | main body font (optional; ImGui default font is used when absent) | https://rsms.me/inter |
| `vendor/fonts/FontAwesome.ttf` | icon font (optional) | https://fontawesome.com |

The MinHook static library is pulled at configure time by
`cmake/FetchDeps.cmake` (FetchContent) — you do not vendor it.

## Why not FetchContent for ImGui too?

Because dropping ImGui into `vendor/imgui/` is a one-time operation that keeps
the DLL reproducibly buildable on machines with no Internet. Everything else
here follows the same rule.
