#pragma once
#include <string>
namespace vanta { struct frame_input_t; }
namespace vanta::bind_popup {
void open(const std::string& module,float x,float y);
bool active();
void close();
void render(const frame_input_t& input);
}
