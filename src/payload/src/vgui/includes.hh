#pragma once

/* @includes:
	single translation unit entry point.
	every source file in the project includes this and nothing else, the order below is the
	dependency order of the whole client: platform -> std -> imgui -> utils -> framework -> gui.
*/

/* platform */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <psapi.h>

/* The client renders inside Minecraft's own OpenGL frame (see
   core/gl_hook.hh). The Direct3D and DWM includes that the standalone,
   separate-window build needed are deliberately gone. */
#include <gl/GL.h>

/* std */
#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/* imgui */
#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"

/* utils */
#include "utils/math.hh"
#include "utils/color.hh"

/* core */
#include "core/logger.hh"
#include "core/paths.hh"

/* framework */
#include "framework/animations/animations.hh"
#include "framework/render/render.hh"

#include "core/cursor_guard.hh"
#include "core/gl_hook.hh"

#include "framework/context/context.hh"

/* gui: design tokens, then the render vocabulary, then widgets, then the views built from them */
#include "dt/theme/theme.hh"
#include "dt/theme/animation.hh"
#include "dt/theme/input.hh"

#include "dt/render/svg.hh"
#include "dt/render/emoji.hh"
#include "dt/render/icons.hh"
#include "dt/render/profile_head.hh"
#include "dt/render/text.hh"
#include "dt/render/draw.hh"
#include "dt/render/panel_glass.hh"

#include "dt/state/social.hh"
#include "dt/state/roster.hh"

#include "bridge.h"

#include "dt/widgets/module.hh"
#include "dt/widgets/color_popup.hh"

#include "dt/views/module_manager.hh"
#include "dt/views/player_list.hh"
#include "dt/views/settings.hh"
#include "dt/views/themes.hh"
#include "dt/views/header.hh"
#include "dt/views/sidebar.hh"

#include "dt/app.hh"
