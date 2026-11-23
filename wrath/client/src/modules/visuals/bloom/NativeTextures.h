#pragma once
namespace vanta::modules::bloom {
// Call on the owning GL context; the caller owns the returned texture name.
unsigned loadNativeTexture(int resource, bool mipmaps = true, bool repeat = false);
} // namespace vanta::modules::bloom
