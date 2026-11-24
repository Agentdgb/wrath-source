#pragma once
#include <windows.h>
namespace vanta::modules::antibot {
bool allows_combat();
void observe_window_input(HWND window, UINT message, WPARAM wp, LPARAM lp, bool overlay_visible);
}
