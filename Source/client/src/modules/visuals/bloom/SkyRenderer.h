#pragma once
#include "modules/render/hitfx/hitfx_render_context.h"
namespace vanta::modules::bloom {
void drawAtmosphereSky(const hitfx::render_frame &);
void drawAtmosphereTint();
void shutdownAtmosphereGl();
const char *atmosphereSkyError();
} // namespace vanta::modules::bloom
